#include "bsp_wifi_hosted.h"

#include "bsp_usb.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_sdio_diagnostic.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    HOSTED_IDLE = 0,
    HOSTED_QUEUED,
    HOSTED_CONNECTING,
    HOSTED_READY,
    HOSTED_FAILED,
} hosted_state_t;

typedef struct {
    hosted_state_t state;
    esp_err_t error;
    uint32_t chip_id;
    char chip[16];
    esp_hosted_coprocessor_fwver_t fw;
    bool fw_valid;
    bool wifi_started;
    char token[17];
    unsigned width;
    unsigned khz;
    bool diagnostic;
    unsigned poll_ms;
    unsigned init_ms;
} hosted_status_t;

static const char *TAG = "meoave_hosted";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static hosted_status_t s_status = {.state = HOSTED_IDLE, .chip = "unknown", .token = "none"};
static esp_netif_t *s_sta_netif;
static bool s_wifi_initialized;

static const char *state_name(hosted_state_t state)
{
    static const char *const names[] = {"IDLE", "QUEUED", "CONNECTING", "READY", "FAILED"};
    return state <= HOSTED_FAILED ? names[state] : "UNKNOWN";
}

static void report(const char *token)
{
    hosted_status_t s;
    portENTER_CRITICAL(&s_lock);
    s = s_status;
    portEXIT_CRITICAL(&s_lock);
    char line[448];
    snprintf(line, sizeof(line),
             "MEO:WIFI_HOSTED token=%s state=%s error=0x%x chip_id=%lu chip=%s "
             "fw=%s%lu.%lu.%lu rev=%ld pre=%ld build=%ld wifi_started=%u "
             "gpio6=UNUSED reset_gpio=54 proto=2 operation=%s diag=%u width=%u khz=%u poll_ms=%u init_ms=%u\n",
             token, state_name(s.state), (unsigned)s.error, (unsigned long)s.chip_id,
             s.chip, s.fw_valid ? "" : "unknown:", (unsigned long)s.fw.major1,
             (unsigned long)s.fw.minor1, (unsigned long)s.fw.patch1, (long)s.fw.revision,
             (long)s.fw.prerelease, (long)s.fw.build, s.wifi_started,
             s.token, s.diagnostic, s.width, s.khz, s.poll_ms, s.init_ms);
    (void)bsp_usb_cdc_write(line);
}

static void mark_failed(esp_err_t err)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = HOSTED_FAILED;
    s_status.error = err;
    portEXIT_CRITICAL(&s_lock);
}

static void hosted_worker(void *arg)
{
    (void)arg;
    esp_err_t err = (esp_err_t)esp_hosted_connect_to_slave();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C5 Hosted transport connect failed: %s", esp_err_to_name(err));
        mark_failed(err);
        vTaskDelete(NULL);
        return;
    }

    uint32_t chip_id = 0;
    char chip[16] = {0};
    esp_hosted_coprocessor_fwver_t fw = {0};
    int fw_ret = esp_hosted_get_coprocessor_fwversion(&fw);
    if (fw_ret != ESP_OK) {
        mark_failed(fw_ret);
        vTaskDelete(NULL);
        return;
    }
    int chip_ret = esp_hosted_get_cp_info(&chip_id, chip, sizeof(chip));
    if (chip_ret != ESP_OK || strcmp(chip, "esp32c5") != 0) {
        mark_failed(chip_ret != ESP_OK ? chip_ret : ESP_ERR_INVALID_RESPONSE);
        vTaskDelete(NULL);
        return;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        mark_failed(err);
        vTaskDelete(NULL);
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        mark_failed(err);
        vTaskDelete(NULL);
        return;
    }
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            mark_failed(ESP_ERR_NO_MEM);
            vTaskDelete(NULL);
            return;
        }
    }
    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            mark_failed(err);
            vTaskDelete(NULL);
            return;
        }
        s_wifi_initialized = true;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        mark_failed(err);
        vTaskDelete(NULL);
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.state = HOSTED_READY;
    s_status.error = ESP_OK;
    s_status.chip_id = chip_ret == ESP_OK ? chip_id : 0;
    snprintf(s_status.chip, sizeof(s_status.chip), "%s", chip_ret == ESP_OK ? chip : "unknown");
    s_status.fw = fw;
    s_status.fw_valid = fw_ret == ESP_OK;
    s_status.wifi_started = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "C5 Hosted Wi-Fi ready; GPIO6 remains unused");
    vTaskDelete(NULL);
}

void bsp_wifi_hosted_init(void)
{
    /* The component's port constructor only allocates the transport. It does
     * not connect or toggle GPIO54; connection remains an explicit action. */
    portENTER_CRITICAL(&s_lock);
    s_status = (hosted_status_t){.state = HOSTED_IDLE, .chip = "unknown", .token = "none",
        .width = CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH, .khz = CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ,
        .init_ms = 4000};
    portEXIT_CRITICAL(&s_lock);
}

bool bsp_wifi_hosted_command(const char *line)
{
    if (!line || strncmp(line, "WIFI HOSTED ", 12) != 0) return false;
    char cmd[12], token[17], extra;
    if (strncmp(line, "WIFI HOSTED CONFIG ", 19) == 0) {
        unsigned width, khz, poll_ms = 0, init_ms = 4000;
        int count = sscanf(line, "WIFI HOSTED CONFIG %16s %u %u %u %u %c",
                           token, &width, &khz, &poll_ms, &init_ms, &extra);
        if (count != 5) {
            poll_ms = 0;
            init_ms = 4000;
            if (sscanf(line, "WIFI HOSTED CONFIG %16s %u %u %c", token, &width, &khz, &extra) != 3) return true;
        }
        for (const char *p = token; *p; ++p) if (!isalnum((unsigned char)*p)) return true;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        /* USB commands and service run on the same app task. The port also
         * refuses changes after any card-init, even if app state is stale. */
        if (s_status.state == HOSTED_IDLE && !s_status.diagnostic) {
            err = esp_hosted_sdio_diagnostic_configure_ex(width, khz, poll_ms, init_ms);
            if (err == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                s_status.width = width;
                s_status.khz = khz;
                s_status.diagnostic = true;
                s_status.poll_ms = poll_ms;
                s_status.init_ms = init_ms;
                portEXIT_CRITICAL(&s_lock);
            }
        }
        char reply[160];
        snprintf(reply, sizeof(reply), "MEO:WIFI_HOSTED_CONFIG token=%s accepted=%u error=0x%x width=%u khz=%u poll_ms=%u init_ms=%u\n",
                 token, err == ESP_OK, (unsigned)err, width, khz, poll_ms, init_ms);
        (void)bsp_usb_cdc_write(reply);
        return true;
    }
    if (sscanf(line, "WIFI HOSTED %11s %16s %c", cmd, token, &extra) != 2) return true;
    for (const char *p = token; *p; ++p) {
        if (!isalnum((unsigned char)*p)) return true;
    }
    if (strcmp(cmd, "STATUS") == 0) {
        report(token);
    } else if (strcmp(cmd, "CONNECT") == 0) {
        bool accepted = false;
        portENTER_CRITICAL(&s_lock);
        if (s_status.state == HOSTED_IDLE) {
            s_status.state = HOSTED_QUEUED;
            s_status.error = ESP_OK;
            snprintf(s_status.token, sizeof(s_status.token), "%s", token);
            accepted = true;
        }
        portEXIT_CRITICAL(&s_lock);
        char reply[96];
        snprintf(reply, sizeof(reply), "MEO:WIFI_HOSTED_ACCEPT token=%s accepted=%u\n", token, accepted);
        (void)bsp_usb_cdc_write(reply);
    }
    return true;
}

void bsp_wifi_hosted_service(void)
{
    bool start = false;
    portENTER_CRITICAL(&s_lock);
    if (s_status.state == HOSTED_QUEUED) {
        s_status.state = HOSTED_CONNECTING;
        start = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (start && xTaskCreate(hosted_worker, "hosted_connect", 8192, NULL, 2, NULL) != pdPASS) {
        mark_failed(ESP_ERR_NO_MEM);
    }
}
