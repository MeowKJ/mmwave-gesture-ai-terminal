/* App-only A/B maintenance. Bootloader/table/NVS are never writable via this API.
 * Explicit hash+boot-nonce confirmation; pending images are NOT auto-confirmed.
 * GPIO2 is default high-Z. Only this leased API may temporarily drive it. */
#include <string.h>
#include "c5_maintenance.h"
#include "c5_io2_guard.h"
#include "../../shared/c5_maintenance_protocol.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_hosted_coprocessor.h"
#include "esp_hosted_peer_data.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG="c5_maintenance";
static QueueHandle_t queue;
static uint32_t boot_id, pin_mode;
static int64_t lease_deadline;
static unsigned health_reads;
static uint32_t last_health_seq;
static bool last_health_valid;

static esp_ota_img_states_t image_state(void)
{
    esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t *p=esp_ota_get_running_partition();
    if (p) (void)esp_ota_get_state_partition(p, &state);
    return state;
}

void c5_maintenance_pre_init(void)
{
#if defined(CONFIG_C5_MAINT_TEST_FAIL_PENDING) && defined(CONFIG_C5_MAINT_TEST_HANG_PENDING)
#error "Select only one maintenance failure injection"
#endif
#ifdef CONFIG_C5_MAINT_TEST_HANG_PENDING
    if (image_state()==ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "HANG_TEST pending image blocks before Hosted; external EN recovery required");
        /* Keep the scheduler/idle watchdog alive, but never initialize SDIO or
         * the 90s maintenance task. Tests an unresponsive candidate, not its
         * own watchdog/self-reset. GPIO2 was already released by app_main. */
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
#ifdef CONFIG_C5_MAINT_TEST_FAIL_PENDING
    if (image_state()==ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "FAULT_TEST pending image resets before Hosted; bootloader must roll back");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
#endif
}

static esp_err_t pin_apply(uint32_t mode, uint32_t ms)
{
    if (mode>MEO_C5_DRIVE_HIGH || (mode!=MEO_C5_HIZ && (ms<100 || ms>5000)))
        return ESP_ERR_INVALID_ARG;
    /* Break-before-make, even for changing the output level. On any partial
     * failure, leave the shared audio pad high-Z rather than keep stale drive. */
    esp_err_t err=c5_io2_guard_apply();
    pin_mode=MEO_C5_HIZ;
    lease_deadline=0;
    if (err!=ESP_OK || mode==MEO_C5_HIZ) return err;
    if (mode==MEO_C5_PULLUP || mode==MEO_C5_PULLDOWN) {
        err=gpio_set_pull_mode(C5_IO2_SHARED_GPIO,
                             mode==MEO_C5_PULLUP ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    } else {
        err=gpio_set_level(C5_IO2_SHARED_GPIO, mode==MEO_C5_DRIVE_HIGH);
        if (err==ESP_OK) err=gpio_set_direction(C5_IO2_SHARED_GPIO, GPIO_MODE_INPUT_OUTPUT);
    }
    if (err!=ESP_OK) { (void)c5_io2_guard_apply(); return err; }
    pin_mode=mode;
    lease_deadline=esp_timer_get_time()+(int64_t)ms*1000;
    return ESP_OK;
}

static void respond(const meo_c5_request_t *req, esp_err_t error)
{
    const esp_app_desc_t *app=esp_app_get_description();
    const esp_partition_t *running=esp_ota_get_running_partition(), *boot=esp_ota_get_boot_partition();
    meo_c5_response_t r={.magic=MEO_C5_MAINT_MAGIC,.version=MEO_C5_MAINT_VERSION,
        .op=req->op,.seq=req->seq,.error=(uint32_t)error,.boot_id=boot_id,
        .reset_reason=esp_reset_reason(),.slot_address=running ? running->address : 0,
        .boot_address=boot ? boot->address : 0,.ota_state=image_state(),
        .flags=MEO_C5_LOCAL_READY,.pin_mode=pin_mode,
        .uptime_ms=(uint32_t)(esp_timer_get_time()/1000),
        .pin_level=(uint32_t)gpio_get_level(C5_IO2_SHARED_GPIO)};
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    r.flags|=MEO_C5_ROLLBACK;
#endif
    if (r.ota_state==ESP_OTA_IMG_PENDING_VERIFY) r.flags|=MEO_C5_PENDING;
    if (pin_mode==MEO_C5_HIZ) r.flags|=MEO_C5_PIN_HIGHZ;
    int64_t left=lease_deadline-esp_timer_get_time();
    r.lease_ms=left>0 ? (uint32_t)(left/1000) : 0;
    memcpy(r.elf_sha, app->app_elf_sha256, sizeof(r.elf_sha));
    memcpy(r.project, app->project_name, sizeof(r.project));
    memcpy(r.app_version, app->version, sizeof(r.app_version));
    esp_err_t sent=esp_hosted_send_custom_data(MEO_C5_MAINT_MSG, (const uint8_t *)&r, sizeof(r));
    if (sent!=ESP_OK) ESP_LOGW(TAG, "response failed: %s", esp_err_to_name(sent));
}

static void advance_timers(int64_t now)
{
    if (lease_deadline && now>=lease_deadline) {
        if (pin_apply(MEO_C5_HIZ,0)!=ESP_OK) {
            ESP_LOGE(TAG,"shared pin release failed; resetting to safe boot");
            esp_restart();
        }
    }
    /* A host that never confirms a pending image must not strand it forever.
     * A hung app is separately recoverable through P4's single EN reset. */
    if (now>90000000 && image_state()==ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG,"no host confirmation within 90s; reboot for rollback");
        (void)pin_apply(MEO_C5_HIZ,0);
        esp_restart();
    }
}

static esp_err_t handle_request(const meo_c5_request_t *req, int64_t now)
{
    if (req->op!=MEO_C5_INFO && req->boot_id!=boot_id) return ESP_ERR_INVALID_STATE;
    switch (req->op) {
    case MEO_C5_INFO:
        if (!last_health_valid || req->seq!=last_health_seq) ++health_reads;
        last_health_seq=req->seq; last_health_valid=true;
        return ESP_OK;
    case MEO_C5_CONFIRM:
        if (health_reads<3 || now<2000000 || pin_mode!=MEO_C5_HIZ ||
            memcmp(req->elf_sha,esp_app_get_description()->app_elf_sha256,32))
            return ESP_ERR_INVALID_STATE;
        return esp_ota_mark_app_valid_cancel_rollback();
    case MEO_C5_PIN: return pin_apply(req->arg0,req->arg1);
    case MEO_C5_REBOOT: return pin_apply(MEO_C5_HIZ,0);
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        meo_c5_request_t req;
        bool received=xQueueReceive(queue,&req,pdMS_TO_TICKS(50))==pdTRUE;
        int64_t now=esp_timer_get_time();
        advance_timers(now);
        if (!received) continue;
        esp_err_t error=handle_request(&req,now);
        respond(&req,error);
        if (req.op==MEO_C5_REBOOT && error==ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    }
}

static void receive(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (id!=MEO_C5_MAINT_MSG || !data || len!=sizeof(meo_c5_request_t)) return;
    meo_c5_request_t r;
    memcpy(&r,data,sizeof(r));
    if (r.magic!=MEO_C5_MAINT_MAGIC || r.version!=MEO_C5_MAINT_VERSION) return;
    /* This callback runs in Hosted RX; queue only, never block on flash/GPIO. */
    if (xQueueSend(queue,&r,0)!=pdTRUE) ESP_LOGW(TAG,"request queue full");
}

esp_err_t c5_maintenance_init(void)
{
    boot_id=esp_random();
    if (!boot_id) boot_id=1;
    queue=xQueueCreate(4,sizeof(meo_c5_request_t));
    if (!queue) return ESP_ERR_NO_MEM;
    esp_err_t error=esp_hosted_register_custom_callback(MEO_C5_MAINT_MSG,receive,NULL);
    if (error!=ESP_OK) return error;
    if (xTaskCreate(worker,"c5_maint",4096,NULL,2,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG,"ready boot_id=%08lx state=%u explicit_host_confirm=1 gpio2_default=HIZ",
             (unsigned long)boot_id,(unsigned)image_state());
    return ESP_OK;
}
