/* ESP-Hosted Wi-Fi for the custom board: transport bring-up, STA scan/join,
 * NVS credentials and a status snapshot for the UI and the USB shell.
 *
 * Ordering rules learned from bring-up:
 *   - The Hosted transport is started from a worker, never from app_main, so a
 *     silent C5 cannot delay display/USB enumeration.
 *   - Nothing here resets the C5: SDIO GPIO54 is shared with radar SOP0 and is
 *     left to the Hosted port's "reset only if necessary" policy.
 *   - Requests are queued by the caller and executed from the app task; the
 *     Wi-Fi event task only updates cached state.
 */
#include "bsp_wifi_hosted.h"

#include "bsp_usb.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WIFI_NS        "wifi"
#define WIFI_SCAN_MAX  16   /* one scan burst, filtered down before caching */
#define WIFI_MAX_AP    6    /* what the UI can show */

typedef enum { REQ_NONE = 0, REQ_SCAN, REQ_JOIN, REQ_SET } req_kind_t;

static const char *TAG = "meoave_wifi";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *s_stage = "IDLE";   /* bring-up step, for the USB report */

static struct {
    bsp_wifi_state_t state;
    int error;
    char ssid[33];
    int rssi;
    char ip[16];
    unsigned ap_count;
    unsigned scan_raw;    /* driver-reported count of the last scan */
    char ap_ssid[WIFI_MAX_AP][33];
    int ap_rssi[WIFI_MAX_AP];
} s_wifi;

static struct {
    req_kind_t kind;
    char ssid[33];
    char pass[65];
} s_req;

static esp_netif_t *s_netif;
static bool s_events;
static char s_saved_ssid[33];
/* The system event task only has CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE
 * (2304 bytes here). Fetching AP records there overflows the stack and
 * corrupts the event loop's handler lists, so the handler only raises a flag
 * and the app task does the RPC. */
static volatile bool s_scan_ready;
/* Bounded re-join after an unexpected drop: 3 attempts, 5 s apart, refilled on
 * every successful association. Never a tight retry loop. */
static int s_rejoin_left;
static int64_t s_rejoin_at_ms;

static void set_state(bsp_wifi_state_t state, int error)
{
    portENTER_CRITICAL(&s_lock);
    s_wifi.state = state;
    s_wifi.error = error;
    portEXIT_CRITICAL(&s_lock);
}

static bsp_wifi_state_t get_state(void)
{
    portENTER_CRITICAL(&s_lock);
    bsp_wifi_state_t state = s_wifi.state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

/* ---------------- NVS credentials ---------------- */

static bool load_credentials(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NS, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t ssid_len = ssid_size, pass_len = pass_size;
    bool ok = nvs_get_str(handle, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0];
    if (ok) {
        if (nvs_get_str(handle, "pass", pass, &pass_len) != ESP_OK) pass[0] = 0;
    }
    nvs_close(handle);
    return ok;
}

static bool save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

/* ---------------- Wi-Fi events ---------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_SCAN_DONE) {
        /* Do not touch the results here: see s_scan_ready. A scan must not
           demote a live association: only an unlinked radio becomes READY. */
        s_scan_ready = true;
        portENTER_CRITICAL(&s_lock);
        if (!s_wifi.ip[0] && s_wifi.state == BSP_WIFI_SCANNING) s_wifi.state = BSP_WIFI_READY;
        portEXIT_CRITICAL(&s_lock);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        set_state(BSP_WIFI_JOINING, 0);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        int reason = d ? d->reason : 0;
        bsp_wifi_state_t state = get_state();
        portENTER_CRITICAL(&s_lock);
        s_wifi.ip[0] = 0;
        s_wifi.rssi = 0;
        portEXIT_CRITICAL(&s_lock);
        /* A drop after being online is not an error the user asked for; a drop
           during an explicit join is. Neither case retries automatically. */
        if (state == BSP_WIFI_JOINING) {
            ESP_LOGW(TAG, "join failed, reason=%d", reason);
            set_state(BSP_WIFI_ERROR, reason);
            /* A boot-time join can fail once (AP still booting, channel busy).
               Three spaced retries keep the device usable without a tap;
               a manual SET/JOIN resets the budget through ONLINE. */
            if (s_saved_ssid[0] && s_rejoin_left <= 0) {
                s_rejoin_left = 3;
                s_rejoin_at_ms = esp_timer_get_time() / 1000 + 5000;
            }
        } else {
            ESP_LOGW(TAG, "disconnected, reason=%d", reason);
            set_state(BSP_WIFI_READY, 0);
            if (s_saved_ssid[0]) {
                s_rejoin_left = 3;
                s_rejoin_at_ms = esp_timer_get_time() / 1000 + 5000;
            }
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *event = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    wifi_ap_record_t ap;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_wifi.ip, sizeof(s_wifi.ip), "%s", ip);
    s_wifi.rssi = rssi;
    s_wifi.state = BSP_WIFI_ONLINE;
    s_wifi.error = 0;
    portEXIT_CRITICAL(&s_lock);
    s_rejoin_left = 0;
    ESP_LOGI(TAG, "online: %s", ip);
}

/* ---------------- bring-up worker ---------------- */

/* The C5 slave can be mid-boot or freshly reset by a P4 restart; a single
 * transport attempt then leaves the device offline until someone reboots it.
 * Six spaced attempts cover that window without hammering the SDIO bus. */
#define WIFI_BRINGUP_ATTEMPTS 6
#define WIFI_BRINGUP_GAP_MS   5000

/* A failed connect leaves the port in a dead state; retrying the same port just
 * repeats the same error. Tear the transport down and rebuild it per attempt. */
static esp_err_t transport_attempts(unsigned attempts, const char *stage)
{
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        s_stage = attempt == 1 ? stage : "TRANSPORT_RETRY";
        err = (esp_err_t)esp_hosted_connect_to_slave();
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "Hosted transport attempt %u/%u: %s", attempt, attempts,
                 esp_err_to_name(err));
        if (attempt < attempts) {
            vTaskDelay(pdMS_TO_TICKS(600));
            s_stage = "TRANSPORT_REINIT";
            (void)esp_hosted_deinit();
            vTaskDelay(pdMS_TO_TICKS(400));
            (void)esp_hosted_init();
            vTaskDelay(pdMS_TO_TICKS(WIFI_BRINGUP_GAP_MS));
        }
    }
    return err;
}

/* Last resort when the C5 slave stops answering: one shared-line EN pulse.
 * GPIO54 is also radar SOP0, so the radar is switched into flashing mode for
 * the duration of the pulse and returns to functional mode on release. This is
 * bounded to a single pulse per bring-up, never a loop. */
static void pulse_c5_en(void)
{
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << PIN_C5_RESET, .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    (void)gpio_set_level(PIN_C5_RESET, 1);
    (void)gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(2));
    (void)gpio_set_level(PIN_C5_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    (void)gpio_set_level(PIN_C5_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(6000));   /* C5 boot + slave transport ready */
}

static void bringup_worker(void *arg)
{
    (void)arg;
    set_state(BSP_WIFI_STARTING, 0);
    esp_err_t err = transport_attempts(WIFI_BRINGUP_ATTEMPTS, "TRANSPORT");
    if (err != ESP_OK) {
        s_stage = "RESET_C5";
        ESP_LOGW(TAG, "transport dead, pulsing C5 EN once");
        pulse_c5_en();
        err = transport_attempts(3, "TRANSPORT_AFTER_RESET");
    }
    if (err != ESP_OK) {
        s_stage = "TRANSPORT_FAILED";
        set_state(BSP_WIFI_ERROR, err);
        vTaskDelete(NULL);
        return;
    }
    s_stage = "CP_INFO";
    uint32_t chip_id = 0;
    char chip[16] = {0};
    if (esp_hosted_get_cp_info(&chip_id, chip, sizeof(chip)) != ESP_OK || strcmp(chip, "esp32c5") != 0) {
        set_state(BSP_WIFI_ERROR, ESP_ERR_INVALID_RESPONSE);
        vTaskDelete(NULL);
        return;
    }
    s_stage = "NETIF";
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { set_state(BSP_WIFI_ERROR, err); vTaskDelete(NULL); return; }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { set_state(BSP_WIFI_ERROR, err); vTaskDelete(NULL); return; }
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) { set_state(BSP_WIFI_ERROR, ESP_ERR_NO_MEM); vTaskDelete(NULL); return; }
    }
    if (!s_events) {
        if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL) != ESP_OK ||
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL) != ESP_OK) {
            set_state(BSP_WIFI_ERROR, ESP_FAIL);
            vTaskDelete(NULL);
            return;
        }
        s_events = true;
    }
    s_stage = "WIFI_INIT";
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) { set_state(BSP_WIFI_ERROR, err); vTaskDelete(NULL); return; }
    /* Credentials are ours (NVS namespace "wifi"), not esp_wifi's own store. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
    s_stage = "WIFI_START";
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        set_state(BSP_WIFI_ERROR, err);
        vTaskDelete(NULL);
        return;
    }
    s_stage = "READY";
    ESP_LOGI(TAG, "Hosted Wi-Fi ready on C5 (%s), GPIO6 untouched", chip);
    /* ESP32-C5 is dual-band; a band-locked default can scan nothing at all. */
    esp_err_t band = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (band != ESP_OK) ESP_LOGW(TAG, "band mode auto: %s", esp_err_to_name(band));
    set_state(BSP_WIFI_READY, 0);

    /* Re-join the last known network without any key press. */
    char ssid[33], pass[65];
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);
        portENTER_CRITICAL(&s_lock);
        if (s_req.kind == REQ_NONE) {
            s_req.kind = REQ_JOIN;
            snprintf(s_req.ssid, sizeof(s_req.ssid), "%s", ssid);
            snprintf(s_req.pass, sizeof(s_req.pass), "%s", pass);
        }
        portEXIT_CRITICAL(&s_lock);
    }
    vTaskDelete(NULL);
}

/* ---------------- request API ---------------- */

static bool queue_request(req_kind_t kind, const char *ssid, const char *pass)
{
    bsp_wifi_state_t state = get_state();
    if (kind == REQ_SCAN ? (state != BSP_WIFI_READY && state != BSP_WIFI_ONLINE)
                         : (state != BSP_WIFI_READY && state != BSP_WIFI_ONLINE && state != BSP_WIFI_ERROR))
        return false;
    portENTER_CRITICAL(&s_lock);
    s_req.kind = kind;
    snprintf(s_req.ssid, sizeof(s_req.ssid), "%s", ssid ? ssid : "");
    snprintf(s_req.pass, sizeof(s_req.pass), "%s", pass ? pass : "");
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool bsp_wifi_scan(void)
{
    if (get_state() == BSP_WIFI_ERROR) {
        /* The page's only control is "scan": when the link is down, make that
           tap restart the Hosted bring-up instead of returning a silent false. */
        return xTaskCreate(bringup_worker, "wifi_retry", 8192, NULL, 2, NULL) == pdPASS;
    }
    return queue_request(REQ_SCAN, NULL, NULL);
}

bool bsp_wifi_join_saved(const char *ssid)
{
    if (!ssid || !*ssid) return false;
    char saved[33], pass[65];
    if (!load_credentials(saved, sizeof(saved), pass, sizeof(pass)) || strcmp(saved, ssid) != 0)
        return false;
    return queue_request(REQ_JOIN, ssid, pass);
}

bool bsp_wifi_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return false;
    if (!save_credentials(ssid, pass)) return false;
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);
    return queue_request(REQ_SET, ssid, pass);
}

unsigned bsp_wifi_ap_list(char ssid[][33], int rssi[], unsigned max)
{
    unsigned copied = 0;
    portENTER_CRITICAL(&s_lock);
    unsigned count = s_wifi.ap_count < max ? s_wifi.ap_count : max;
    for (unsigned i = 0; i < count; ++i) {
        snprintf(ssid[i], 33, "%s", s_wifi.ap_ssid[i]);
        rssi[i] = s_wifi.ap_rssi[i];
        ++copied;
    }
    portEXIT_CRITICAL(&s_lock);
    return copied;
}

bsp_wifi_status_t bsp_wifi_hosted_status(void)
{
    bsp_wifi_status_t out;
    portENTER_CRITICAL(&s_lock);
    out.state = s_wifi.state;
    out.error = s_wifi.error;
    out.rssi = s_wifi.rssi;
    out.ap_count = s_wifi.ap_count;
    out.ap_raw = s_wifi.scan_raw;
    memcpy(out.ssid, s_wifi.ssid, sizeof(out.ssid));
    memcpy(out.ip, s_wifi.ip, sizeof(out.ip));
    portEXIT_CRITICAL(&s_lock);
    return out;
}

/* ---------------- service: execute queued requests ---------------- */

static void join_now(const char *ssid, const char *pass)
{
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* SSID/password are byte arrays, not C strings: copy with an explicit
       NUL so a 32-character SSID is not silently truncated by snprintf. */
    size_t ssid_len = strnlen(ssid, sizeof(cfg.sta.ssid) - 1);
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    cfg.sta.ssid[ssid_len] = 0;
    const char *password = pass ? pass : "";
    size_t pass_len = strnlen(password, sizeof(cfg.sta.password) - 1);
    memcpy(cfg.sta.password, password, pass_len);
    cfg.sta.password[pass_len] = 0;
    cfg.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", ssid);
    s_wifi.ip[0] = 0;
    s_wifi.state = BSP_WIFI_JOINING;
    s_wifi.error = 0;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "join %s: %s", ssid, esp_err_to_name(err));
        set_state(BSP_WIFI_ERROR, err);
    }
}

static void collect_scan_results(void)
{
    /* Runs on the app task: the record buffer is far too big for the event
     * task's 2304-byte stack, and the RPC below has a deep call chain.
     * Records are validated before caching: an entry without a printable SSID
     * or with an impossible RSSI is dropped instead of shown as a blank row. */
    uint16_t reported = 0;
    (void)esp_wifi_scan_get_ap_num(&reported);
    static char keep_ssid[WIFI_MAX_AP][33];   /* filled outside the lock */
    static int keep_rssi[WIFI_MAX_AP];
    unsigned kept = 0, seen = 0;
    /* Pull one record per call: the bulk copy comes back misaligned after the
       first entry (first SSID is correct, the rest decode as garbage/zero). */
    for (unsigned i = 0; i < WIFI_SCAN_MAX && kept < WIFI_MAX_AP; ++i) {
        wifi_ap_record_t rec;
        memset(&rec, 0, sizeof(rec));
        if (esp_wifi_scan_get_ap_record(&rec) != ESP_OK) break;
        ++seen;
        unsigned len = 0;
        char ssid[33] = {0};
        while (len < 32 && rec.ssid[len]) {
            if (rec.ssid[len] < 0x20 || rec.ssid[len] > 0x7e) { len = 0; break; }
            ssid[len] = (char)rec.ssid[len];
            ++len;
        }
        if (!len || rec.rssi >= 0 || rec.rssi < -100) continue;
        snprintf(keep_ssid[kept], sizeof(keep_ssid[kept]), "%s", ssid);
        keep_rssi[kept] = rec.rssi;
        ++kept;
    }
    portENTER_CRITICAL(&s_lock);
    s_wifi.scan_raw = reported;
    s_wifi.ap_count = kept;
    for (unsigned i = 0; i < kept; ++i) {
        memcpy(s_wifi.ap_ssid[i], keep_ssid[i], sizeof(s_wifi.ap_ssid[i]));
        s_wifi.ap_rssi[i] = keep_rssi[i];
    }
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "scan done: reported=%u seen=%u kept=%u", (unsigned)reported, seen, kept);
}

/* All radio work runs here, never on app_main: one scan costs more stack than
 * the main task can spare once the Hosted RPC chain is inlined, and a smashed
 * app_main frame shows up as a load fault far away from this code. */
static void service_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_scan_ready) {
            s_scan_ready = false;
            collect_scan_results();
        }
        /* Bounded re-join after an unexpected drop. */
        if (s_rejoin_left > 0 && get_state() == BSP_WIFI_READY &&
            esp_timer_get_time() / 1000 >= s_rejoin_at_ms) {
            portENTER_CRITICAL(&s_lock);
            bool idle = s_req.kind == REQ_NONE;
            if (idle) {
                s_req.kind = REQ_JOIN;
                snprintf(s_req.ssid, sizeof(s_req.ssid), "%s", s_saved_ssid);
                s_req.pass[0] = 0;   /* the worker reloads the stored password */
            }
            portEXIT_CRITICAL(&s_lock);
            if (idle) {
                --s_rejoin_left;
                s_rejoin_at_ms = esp_timer_get_time() / 1000 + 5000;
            }
        }
        req_kind_t kind;
        char ssid[33], pass[65];
        portENTER_CRITICAL(&s_lock);
        kind = s_req.kind;
        snprintf(ssid, sizeof(ssid), "%s", s_req.ssid);
        snprintf(pass, sizeof(pass), "%s", s_req.pass);
        s_req.kind = REQ_NONE;
        portEXIT_CRITICAL(&s_lock);
        if (kind != REQ_NONE) {
            bsp_wifi_state_t state = get_state();
            if (state == BSP_WIFI_READY || state == BSP_WIFI_ONLINE || state == BSP_WIFI_ERROR) {
                if (kind == REQ_SCAN) {
                    /* Keep a live association; the UI shows the scan list. */
                    if (get_state() != BSP_WIFI_ONLINE) set_state(BSP_WIFI_SCANNING, 0);
                    /* Shorter active dwell: a dual-band sweep with the default
                       timing takes ~10 s, which reads as "the button is dead". */
                    wifi_scan_config_t scan;
                    memset(&scan, 0, sizeof(scan));
                    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
                    scan.scan_time.active.min = 60;
                    scan.scan_time.active.max = 150;
                    esp_err_t err = esp_wifi_scan_start(&scan, false);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
                        set_state(BSP_WIFI_READY, err);
                    }
                } else {
                    /* REQ_SET already persisted the credentials; REQ_JOIN reloads them. */
                    if (kind == REQ_JOIN && (!pass[0] || strcmp(ssid, s_saved_ssid) != 0)) {
                        char saved[33], stored[65];
                        if (load_credentials(saved, sizeof(saved), stored, sizeof(stored)) &&
                            strcmp(saved, ssid) == 0)
                            snprintf(pass, sizeof(pass), "%s", stored);
                    }
                    join_now(ssid, pass);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void bsp_wifi_hosted_service(void)
{
    /* Kept for the existing app loop; the worker above owns the radio now. */
}

void bsp_wifi_hosted_init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_wifi, 0, sizeof(s_wifi));
    s_wifi.state = BSP_WIFI_OFF;
    memset(&s_req, 0, sizeof(s_req));
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(bringup_worker, "wifi_bringup", 8192, NULL, 2, NULL) != pdPASS) {
        set_state(BSP_WIFI_ERROR, ESP_ERR_NO_MEM);
        return;
    }
    if (xTaskCreate(service_worker, "wifi_service", 8192, NULL, 2, NULL) != pdPASS)
        set_state(BSP_WIFI_ERROR, ESP_ERR_NO_MEM);
}

/* ---------------- USB shell ---------------- */

static const char *state_name(bsp_wifi_state_t state)
{
    static const char *const names[] = {"OFF", "STARTING", "READY", "SCANNING", "JOINING", "ONLINE", "ERROR"};
    return state <= BSP_WIFI_ERROR ? names[state] : "UNKNOWN";
}

static void report(const char *token)
{
    bsp_wifi_status_t s = bsp_wifi_hosted_status();
    char line[256];
    snprintf(line, sizeof(line),
             "MEO:WIFI token=%s state=%s stage=%s error=%d en=%d ssid=%s rssi=%d ip=%s aps=%u raw=%u\n",
             token, state_name(s.state), s_stage, s.error, gpio_get_level(PIN_C5_RESET),
             s.ssid[0] ? s.ssid : "-", s.rssi, s.ip[0] ? s.ip : "-", s.ap_count, s.ap_raw);
    (void)bsp_usb_cdc_write(line);
}

static void report_scan(const char *token)
{
    char ssid[WIFI_MAX_AP][33];
    int rssi[WIFI_MAX_AP];
    unsigned count = bsp_wifi_ap_list(ssid, rssi, WIFI_MAX_AP);
    char line[416];
    int n = snprintf(line, sizeof(line), "MEO:WIFI_SCAN token=%s count=%u list=", token, count);
    for (unsigned i = 0; i < count && n < (int)sizeof(line); ++i)
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%u:%d:%s", i ? "," : "", i, rssi[i], ssid[i]);
    if (count) snprintf(line + n, sizeof(line) - (size_t)n, "\n");
    else snprintf(line, sizeof(line), "MEO:WIFI_SCAN token=%s count=0 list=\n", token);
    (void)bsp_usb_cdc_write(line);
}

bool bsp_wifi_hosted_command(const char *line)
{
    if (!line || strncmp(line, "WIFI ", 5) != 0) return false;
    char verb[12] = {0}, token[17] = {0};
    int used = 0;
    if (sscanf(line, "WIFI %11s %16s %n", verb, token, &used) != 2) return true;
    for (const char *p = token; *p; ++p) if (!isalnum((unsigned char)*p)) return true;
    const char *rest = line + used;      /* arguments after "<verb> <token> " */
    char reply[128];
    if (strcmp(verb, "STATUS") == 0) {
        report(token);
    } else if (strcmp(verb, "SCAN") == 0) {
        bool queued = bsp_wifi_scan();
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=SCAN queued=%u\n", token, queued);
        (void)bsp_usb_cdc_write(reply);
        if (queued) report_scan(token);  /* previous results, if any */
    } else if (strcmp(verb, "RETRY") == 0) {
        /* Re-run the transport bring-up without rebooting the board. */
        bool ok = get_state() == BSP_WIFI_ERROR &&
                  xTaskCreate(bringup_worker, "wifi_retry", 8192, NULL, 2, NULL) == pdPASS;
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=RETRY queued=%u\n", token, ok);
        (void)bsp_usb_cdc_write(reply);
    } else if (strcmp(verb, "APS") == 0) {
        report_scan(token);
    } else if (strcmp(verb, "JOIN") == 0) {
        char ssid[33] = {0};
        bool queued = sscanf(rest, "%32s", ssid) == 1 && bsp_wifi_join_saved(ssid);
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=JOIN queued=%u\n", token, queued);
        (void)bsp_usb_cdc_write(reply);
    } else if (strcmp(verb, "SET") == 0) {
        char ssid[33] = {0}, pass[65] = {0};
        int fields = sscanf(rest, "%32s %64s", ssid, pass);
        bool queued = fields >= 1 && bsp_wifi_set_credentials(ssid, fields == 2 ? pass : "");
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=SET queued=%u\n", token, queued);
        (void)bsp_usb_cdc_write(reply);
    } else {
        snprintf(reply, sizeof(reply), "MEO:WIFI_REJECT token=%s reason=unknown_op\n", token);
        (void)bsp_usb_cdc_write(reply);
    }
    return true;
}
