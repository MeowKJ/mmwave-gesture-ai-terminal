/* ESP-Hosted Wi-Fi for the custom board: transport bring-up, STA scan/join,
 * NVS credentials and a status snapshot for the UI and the USB shell.
 *
 * Ordering rules learned from bring-up:
 *   - The Hosted transport is started from a worker, never from app_main, so a
 *     silent C5 cannot delay display/USB enumeration.
 *   - Neither this layer nor Hosted ATTACH resets C5 (shared radar SOP0).
 *   - Requests are queued by the caller and executed by one radio worker; the
 *     Wi-Fi event task only updates cached state.
 */
#include "bsp_wifi_hosted.h"

#include "bsp_usb.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_sdio_diagnostic.h"
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
#define WIFI_SCAN_MAX  32   /* bounded driver snapshot */
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
    bool scanning;
    uint32_t scan_seq, scan_done_seq;
    int scan_error, save_error;
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
static char s_saved_pass[65];
static char s_target_pass[65];
static bool s_radio_ready, s_started, s_save_pending, s_link_up;
static bool s_disconnect_wait, s_connect_pending;
static int64_t s_scan_deadline, s_join_deadline, s_scan_cooldown;
static uint32_t s_scan_status;
/* The system event task only has CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE
 * (2304 bytes here). Fetching AP records there overflows the stack and
 * corrupts the event loop's handler lists, so the handler only raises a flag
 * and the radio worker does the RPC. */
static bool s_scan_ready; /* protected by s_lock, like all event-shared flags */
/* Bounded re-join: three retries, five seconds apart. Failed retries never
 * refill their own budget. No C5 reset/transport recovery is hidden here. */
static int s_rejoin_left;
static int64_t s_rejoin_at_ms;

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool valid_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid || strnlen(ssid, 33) > 32) return false;
    size_t n = pass ? strnlen(pass, 65) : 0;
    if (!n || (n >= 8 && n <= 63)) return true;
    if (n != 64) return false;
    for (size_t i = 0; i < n; ++i) if (!isxdigit((unsigned char)pass[i])) return false;
    return true;
}

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
    /* Single-blob format makes SSID/password one atomic NVS value. */
    struct { char ssid[33], pass[65]; } key = {0};
    size_t size = sizeof(key);
    if (nvs_get_blob(handle, "profile", &key, &size) == ESP_OK && size == sizeof(key) &&
        key.ssid[32] == 0 && key.pass[64] == 0 && valid_credentials(key.ssid, key.pass)) {
        snprintf(ssid, ssid_size, "%s", key.ssid);
        snprintf(pass, pass_size, "%s", key.pass);
        wipe(&key, sizeof(key));
        nvs_close(handle);
        return true;
    }
    wipe(&key, sizeof(key));
    size_t ssid_len = ssid_size, pass_len = pass_size;
    bool ok = nvs_get_str(handle, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0];
    if (ok) {
        if (nvs_get_str(handle, "pass", pass, &pass_len) != ESP_OK) pass[0] = 0;
    }
    nvs_close(handle);
    return ok && valid_credentials(ssid, pass);
}

static bool save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    struct { char ssid[33], pass[65]; } key = {0};
    snprintf(key.ssid, sizeof(key.ssid), "%s", ssid);
    snprintf(key.pass, sizeof(key.pass), "%s", pass ? pass : "");
    esp_err_t err = nvs_set_blob(handle, "profile", &key, sizeof(key));
    if (err == ESP_OK) err = nvs_commit(handle);
    wipe(&key, sizeof(key));
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
        portENTER_CRITICAL(&s_lock);
        if (s_wifi.scanning) {
            const wifi_event_sta_scan_done_t *event = data;
            s_scan_status = event ? event->status : 1;
            s_scan_ready = true;
        }
        portEXIT_CRITICAL(&s_lock);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *d = data;
        portENTER_CRITICAL(&s_lock);
        if (!s_disconnect_wait && s_wifi.state == BSP_WIFI_JOINING && d &&
            d->ssid_len == strlen(s_wifi.ssid) && !memcmp(d->ssid, s_wifi.ssid, d->ssid_len))
            s_link_up = true;
        portEXIT_CRITICAL(&s_lock);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        int reason = d ? d->reason : 0;
        portENTER_CRITICAL(&s_lock);
        bsp_wifi_state_t state = s_wifi.state;
        s_link_up = false;
        s_wifi.ip[0] = 0;
        s_wifi.rssi = 0;
        if (s_disconnect_wait) {
            s_disconnect_wait = false;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        /* Intentional disconnects were consumed above. Unexpected drops and
         * failed joins share the remaining, bounded retry budget. */
        if (state == BSP_WIFI_JOINING) {
            s_wifi.state = BSP_WIFI_ERROR;
            s_wifi.error = reason;
            /* A boot-time join can fail once (AP still booting, channel busy).
               Three spaced retries keep the device usable without a tap;
               a manual SET/JOIN resets the budget through ONLINE. */
            if (s_rejoin_left > 0) {
                s_rejoin_at_ms = esp_timer_get_time() / 1000 + 5000;
            }
        } else {
            if (state == BSP_WIFI_ONLINE) {
                s_wifi.state = BSP_WIFI_ERROR;
                s_wifi.error = reason;
                s_rejoin_left = 3;
                s_rejoin_at_ms = esp_timer_get_time() / 1000 + 5000;
            }
        }
        portEXIT_CRITICAL(&s_lock);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_LOST_IP) {
        portENTER_CRITICAL(&s_lock);
        if (s_wifi.state == BSP_WIFI_ONLINE) {
            s_wifi.ip[0] = 0;
            s_wifi.state = BSP_WIFI_ERROR;
            s_wifi.error = ESP_ERR_TIMEOUT;
            s_rejoin_left = 3;
            s_rejoin_at_ms = esp_timer_get_time() / 1000 + 5000;
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (id != IP_EVENT_STA_GOT_IP || !data) return;
    const ip_event_got_ip_t *event = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    portENTER_CRITICAL(&s_lock);
    if (!s_link_up || s_disconnect_wait || s_wifi.state != BSP_WIFI_JOINING) {
        portEXIT_CRITICAL(&s_lock); return;
    }
    snprintf(s_wifi.ip, sizeof(s_wifi.ip), "%s", ip);
    s_wifi.state = BSP_WIFI_ONLINE;
    s_wifi.error = 0;
    s_rejoin_left = 0;
    s_rejoin_at_ms = 0;
    s_save_pending = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "online: %s", ip);
}

/* ---------------- bring-up worker ---------------- */

/* One bounded, no-reset ATTACH. This local Hosted API is also used by the
 * audited maintenance path: do not pulse shared GPIO54 on a UI retry. */
static esp_err_t attach_c5(void)
{
    esp_err_t err = esp_hosted_sdio_diagnostic_configure_ex(4,
        CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ, 0, 10000);
    if (err == ESP_OK) err = esp_hosted_sdio_diagnostic_set_reset(false);
    if (err == ESP_OK) err = (esp_err_t)esp_hosted_connect_to_slave();
    return err;
}

static void bringup_worker(void *arg)
{
    (void)arg;
    set_state(BSP_WIFI_STARTING, 0);
    esp_err_t err = attach_c5();
    if (err != ESP_OK) {
        s_stage = "TRANSPORT_FAILED_NO_RESET";
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
        esp_event_handler_instance_t wifi_handler;
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &wifi_handler);
        if (err == ESP_OK) {
            err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL);
            if (err != ESP_OK) (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
        }
        if (err != ESP_OK) {
            set_state(BSP_WIFI_ERROR, ESP_FAIL);
            vTaskDelete(NULL);
            return;
        }
        s_events = true;
    }
    s_stage = "WIFI_INIT";
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
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
    /* ESP32-C5 is dual-band; a band-locked default can scan nothing at all. */
    esp_err_t band = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (band != ESP_OK) ESP_LOGW(TAG, "band mode auto: %s", esp_err_to_name(band));
    if (band != ESP_OK) { set_state(BSP_WIFI_ERROR, band); vTaskDelete(NULL); return; }
    s_stage = "READY";
    ESP_LOGI(TAG, "Hosted Wi-Fi ready on C5 (%s), GPIO6 untouched", chip);
    /* Re-join the last known network without any key press. */
    char ssid[33], pass[65];
    /* UI creation initializes settings later on another task; do not race it
     * and silently miss boot credentials. Never erase NVS to recover it. */
    esp_err_t storage = nvs_flash_init();
    if (storage != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_wifi.save_error = storage;
        portEXIT_CRITICAL(&s_lock);
    }
    if (storage == ESP_OK && load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        portENTER_CRITICAL(&s_lock);
        snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);
        snprintf(s_saved_pass, sizeof(s_saved_pass), "%s", pass);
        snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", ssid);
        snprintf(s_target_pass, sizeof(s_target_pass), "%s", pass);
        s_rejoin_left = 3;
        s_rejoin_at_ms = esp_timer_get_time() / 1000 + 5000;
        if (s_req.kind == REQ_NONE) {
            s_req.kind = REQ_JOIN;
            snprintf(s_req.ssid, sizeof(s_req.ssid), "%s", ssid);
            snprintf(s_req.pass, sizeof(s_req.pass), "%s", pass);
            s_rejoin_left = 3;
            s_wifi.state = BSP_WIFI_JOINING;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    portENTER_CRITICAL(&s_lock);
    s_radio_ready = true;
    s_wifi.state = s_req.kind == REQ_JOIN ? BSP_WIFI_JOINING : BSP_WIFI_READY;
    s_wifi.error = 0;
    portEXIT_CRITICAL(&s_lock);
    wipe(pass, sizeof(pass));
    vTaskDelete(NULL);
}

/* ---------------- request API ---------------- */

static bool queue_request(req_kind_t kind, const char *ssid, const char *pass)
{
    portENTER_CRITICAL(&s_lock);
    bsp_wifi_state_t state = s_wifi.state;
    bool startup_scan = kind == REQ_SCAN && state == BSP_WIFI_STARTING;
    if ((!s_radio_ready && !startup_scan) || s_req.kind != REQ_NONE || s_wifi.scanning ||
        state == BSP_WIFI_JOINING || s_connect_pending ||
        esp_timer_get_time() / 1000 < s_scan_cooldown) {
        portEXIT_CRITICAL(&s_lock); return false;
    }
    s_req.kind = kind;
    snprintf(s_req.ssid, sizeof(s_req.ssid), "%s", ssid ? ssid : "");
    snprintf(s_req.pass, sizeof(s_req.pass), "%s", pass ? pass : "");
    if (kind == REQ_SCAN) {
        s_wifi.scanning = true;
        s_wifi.scan_error = 0;
        ++s_wifi.scan_seq;
    } else {
        s_wifi.state = BSP_WIFI_JOINING; /* reserve before the worker runs */
        s_wifi.error = 0;
        s_rejoin_left = 3;
        s_rejoin_at_ms = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool bsp_wifi_scan(void)
{
    return queue_request(REQ_SCAN, NULL, NULL);
}

bool bsp_wifi_join_saved(const char *ssid)
{
    if (!ssid || !*ssid) return false;
    char saved[33], pass[65];
    portENTER_CRITICAL(&s_lock);
    memcpy(saved, s_saved_ssid, sizeof(saved));
    memcpy(pass, s_saved_pass, sizeof(pass));
    portEXIT_CRITICAL(&s_lock);
    bool ok = !strcmp(saved, ssid) && valid_credentials(saved, pass) && queue_request(REQ_JOIN, ssid, pass);
    wipe(pass, sizeof(pass));
    return ok;
}

bool bsp_wifi_set_credentials(const char *ssid, const char *pass)
{
    return valid_credentials(ssid, pass) && queue_request(REQ_SET, ssid, pass);
}

static unsigned copy_ap_list_locked(char ssid[][33], int rssi[], unsigned max)
{
    unsigned copied = 0;
    if (!ssid || !rssi) return 0;
    unsigned count = s_wifi.ap_count < max ? s_wifi.ap_count : max;
    for (unsigned i = 0; i < count; ++i) {
        snprintf(ssid[i], 33, "%s", s_wifi.ap_ssid[i]);
        rssi[i] = s_wifi.ap_rssi[i];
        ++copied;
    }
    return copied;
}

static bsp_wifi_status_t copy_status_locked(void)
{
    bsp_wifi_status_t out;
    out.state = s_wifi.state;
    out.error = s_wifi.error;
    out.rssi = s_wifi.rssi;
    out.ap_count = s_wifi.ap_count;
    out.ap_raw = s_wifi.scan_raw;
    out.scanning = s_wifi.scanning;
    out.scan_seq = s_wifi.scan_seq;
    out.scan_done_seq = s_wifi.scan_done_seq;
    out.scan_error = s_wifi.scan_error;
    out.save_error = s_wifi.save_error;
    out.radio_ready = s_radio_ready;
    memcpy(out.ssid, s_wifi.ssid, sizeof(out.ssid));
    memcpy(out.ip, s_wifi.ip, sizeof(out.ip));
    return out;
}

unsigned bsp_wifi_snapshot(char ssid[][33], int rssi[], unsigned max, bsp_wifi_status_t *status)
{
    /* A scan may finish between two independent reads. Publish its generation
     * and APs under one lock, never tag an old list with a newer completion. */
    portENTER_CRITICAL(&s_lock);
    unsigned copied = copy_ap_list_locked(ssid, rssi, max);
    if (status) *status = copy_status_locked();
    portEXIT_CRITICAL(&s_lock);
    return copied;
}

unsigned bsp_wifi_ap_list(char ssid[][33], int rssi[], unsigned max)
{
    return bsp_wifi_snapshot(ssid, rssi, max, NULL);
}

bsp_wifi_status_t bsp_wifi_hosted_status(void)
{
    bsp_wifi_status_t out;
    (void)bsp_wifi_snapshot(NULL, NULL, 0, &out);
    return out;
}

/* ---------------- service: execute queued requests ---------------- */

static void connect_now(void)
{
    wifi_config_t cfg = {0};
    /* Full 32-byte SSID and 64-byte hex PSK need not be NUL terminated. */
    memcpy(cfg.sta.ssid, s_wifi.ssid, strlen(s_wifi.ssid));
    memcpy(cfg.sta.password, s_target_pass, strlen(s_target_pass));
    cfg.sta.threshold.authmode = s_target_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    wipe(&cfg, sizeof(cfg));
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        set_state(BSP_WIFI_ERROR, err);
        portENTER_CRITICAL(&s_lock);
        s_rejoin_at_ms = s_rejoin_left ? esp_timer_get_time() / 1000 + 5000 : 0;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void join_now(const char *ssid, const char *pass)
{
    portENTER_CRITICAL(&s_lock);
    bool disconnect = s_link_up || s_wifi.ip[0];
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", ssid);
    snprintf(s_target_pass, sizeof(s_target_pass), "%s", pass);
    s_wifi.ip[0] = 0;
    s_wifi.rssi = 0;
    s_wifi.state = BSP_WIFI_JOINING;
    s_wifi.error = 0;
    s_save_pending = false;
    s_link_up = false;
    s_disconnect_wait = disconnect;
    s_connect_pending = true;
    s_join_deadline = esp_timer_get_time() / 1000 + 30000;
    portEXIT_CRITICAL(&s_lock);
    /* Applying a config while already connected fails. Wait for deliberate
     * disconnect's event before configuring; do not treat it as a join failure. */
    /* Some drivers return OK without a DISCONNECTED event when already idle.
     * Only await that event if there really was a live association. */
    if (!disconnect) return;
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        portENTER_CRITICAL(&s_lock);
        s_disconnect_wait = false;
        portEXIT_CRITICAL(&s_lock);
    } else if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_disconnect_wait = s_connect_pending = false;
        portEXIT_CRITICAL(&s_lock);
        set_state(BSP_WIFI_ERROR, err);
    }
}

static void finish_scan(esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_wifi.scanning = false;
    s_wifi.scan_error = error;
    s_wifi.scan_done_seq = s_wifi.scan_seq;
    s_scan_ready = false;
    if (s_wifi.state == BSP_WIFI_SCANNING) s_wifi.state = BSP_WIFI_READY;
    portEXIT_CRITICAL(&s_lock);
}

static void collect_scan_results(void)
{
    /* Bulk API frees its driver snapshot. Bounded static storage avoids the
     * tiny event stack and a truncated scan cannot leak leftover AP records. */
    static wifi_ap_record_t records[WIFI_SCAN_MAX];
    uint16_t reported = 0, count = WIFI_SCAN_MAX;
    esp_err_t err = esp_wifi_scan_get_ap_num(&reported);
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&count, records);
    esp_err_t clear = esp_wifi_clear_ap_list();
    if (err == ESP_OK && clear != ESP_OK) err = clear;
    char names[WIFI_MAX_AP][33] = {{0}};
    int signal[WIFI_MAX_AP] = {0};
    unsigned kept = 0;
    if (err == ESP_OK) {
        for (unsigned i = 0; i < count && i < WIFI_SCAN_MAX && kept < WIFI_MAX_AP; ++i) {
            const wifi_ap_record_t *ap = &records[i];
            size_t len = strnlen((const char *)ap->ssid, 32);
            if (!len || ap->rssi > 0 || ap->rssi < -127) continue;
            bool control = false, duplicate = false;
            for (size_t k = 0; k < len; ++k)
                if (ap->ssid[k] < 0x20 || ap->ssid[k] == 0x7f) control = true;
            for (unsigned k = 0; k < kept; ++k)
                if (strlen(names[k]) == len && !memcmp(names[k], ap->ssid, len)) duplicate = true;
            if (control || duplicate) continue;
            /* UTF-8 and unknown glyphs remain real network identities. */
            memcpy(names[kept], ap->ssid, len);
            signal[kept++] = ap->rssi;
        }
        portENTER_CRITICAL(&s_lock);
        s_wifi.scan_raw = reported;
        s_wifi.ap_count = kept;
        memcpy(s_wifi.ap_ssid, names, sizeof(names));
        memcpy(s_wifi.ap_rssi, signal, sizeof(signal));
        portEXIT_CRITICAL(&s_lock);
    }
    finish_scan(err); /* Failed RPC retains last good list; empty success clears it. */
    ESP_LOGI(TAG, "scan complete: raw=%u cached=%u error=%d", reported, kept, err);
}

static void start_scan(void)
{
    portENTER_CRITICAL(&s_lock);
    s_scan_ready = false;
    s_scan_deadline = esp_timer_get_time() / 1000 + 20000;
    if (!s_wifi.ip[0]) s_wifi.state = BSP_WIFI_SCANNING;
    portEXIT_CRITICAL(&s_lock);
    wifi_scan_config_t scan = {0};
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = 60;
    scan.scan_time.active.max = 150;
    esp_err_t err = esp_wifi_scan_start(&scan, false);
    if (err != ESP_OK) finish_scan(err);
}

/* Factored owner tick is exercised with deterministic IDF doubles in tests.
 * No worker, event, UI callback or NVS operation runs on the display task. */
static void service_step(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_radio_ready;
    bool done = s_scan_ready;
    uint32_t scan_status = s_scan_status;
    bool scan_timeout = s_wifi.scanning && s_req.kind != REQ_SCAN &&
                        esp_timer_get_time() / 1000 >= s_scan_deadline;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        if (get_state() == BSP_WIFI_ERROR) {
            portENTER_CRITICAL(&s_lock);
            bool pending = s_wifi.scanning;
            wipe(&s_req, sizeof(s_req));
            int error = s_wifi.error;
            portEXIT_CRITICAL(&s_lock);
            if (pending) finish_scan(error);
        }
        return;
    }
    if (done) {
        if (!scan_status) collect_scan_results();
        else { (void)esp_wifi_clear_ap_list(); finish_scan(ESP_FAIL); }
    } else if (scan_timeout) {
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_clear_ap_list();
        portENTER_CRITICAL(&s_lock);
        s_scan_cooldown = esp_timer_get_time() / 1000 + 1000;
        portEXIT_CRITICAL(&s_lock);
        finish_scan(ESP_ERR_TIMEOUT);
    }
    char ssid[33] = {0}, pass[65] = {0};
    req_kind_t kind = REQ_NONE;
    bool connect = false, persist = false, timeout = false;
    char saved_ssid[33] = {0}, saved_pass[65] = {0};
    portENTER_CRITICAL(&s_lock);
    if (s_save_pending) {
        s_save_pending = false;
        persist = true;
        memcpy(saved_ssid, s_wifi.ssid, sizeof(saved_ssid));
        memcpy(saved_pass, s_target_pass, sizeof(saved_pass));
    }
    if (s_connect_pending && !s_disconnect_wait) {
        s_connect_pending = false;
        connect = true;
    }
    if (s_wifi.state == BSP_WIFI_JOINING && s_req.kind == REQ_NONE &&
        esp_timer_get_time() / 1000 >= s_join_deadline) {
        s_wifi.state = BSP_WIFI_ERROR;
        s_wifi.error = ESP_ERR_TIMEOUT;
        s_disconnect_wait = s_connect_pending = s_link_up = false;
        s_rejoin_at_ms = s_rejoin_left ? esp_timer_get_time() / 1000 + 5000 : 0;
        timeout = true; connect = false;
    }
    if (s_req.kind != REQ_NONE) {
        kind = s_req.kind;
        memcpy(ssid, s_req.ssid, sizeof(ssid));
        memcpy(pass, s_req.pass, sizeof(pass));
        wipe(&s_req, sizeof(s_req));
    } else if (s_rejoin_left > 0 && s_rejoin_at_ms && !s_wifi.scanning &&
               s_wifi.state != BSP_WIFI_JOINING && esp_timer_get_time() / 1000 >= s_rejoin_at_ms) {
        kind = REQ_JOIN;
        memcpy(ssid, s_wifi.ssid, sizeof(ssid));
        memcpy(pass, s_target_pass, sizeof(pass));
        --s_rejoin_left;
        s_rejoin_at_ms = 0;
        s_wifi.state = BSP_WIFI_JOINING;
    }
    portEXIT_CRITICAL(&s_lock);
    if (persist) {
        bool ok = save_credentials(saved_ssid, saved_pass);
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        portENTER_CRITICAL(&s_lock);
        s_wifi.save_error = ok ? 0 : ESP_FAIL;
        if (ok) {
            memcpy(s_saved_ssid, saved_ssid, sizeof(s_saved_ssid));
            memcpy(s_saved_pass, saved_pass, sizeof(s_saved_pass));
        }
        if (s_wifi.state == BSP_WIFI_ONLINE) s_wifi.rssi = rssi;
        portEXIT_CRITICAL(&s_lock);
    }
    wipe(saved_pass, sizeof(saved_pass));
    if (timeout) (void)esp_wifi_disconnect();
    if (connect) connect_now();
    if (kind == REQ_SCAN) start_scan();
    else if (kind != REQ_NONE) join_now(ssid, pass);
    wipe(pass, sizeof(pass));
}

static void service_worker(void *arg)
{
    (void)arg;
    for (;;) { service_step(); vTaskDelay(pdMS_TO_TICKS(20)); }
}

void bsp_wifi_hosted_service(void)
{
    /* Kept for the existing app loop; the worker above owns the radio now. */
}

void bsp_wifi_hosted_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_started) { portEXIT_CRITICAL(&s_lock); return; }
    s_started = true;
    memset(&s_wifi, 0, sizeof(s_wifi));
    s_wifi.state = BSP_WIFI_STARTING;
    memset(&s_req, 0, sizeof(s_req));
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(service_worker, "wifi_service", 10240, NULL, 2, NULL) != pdPASS) {
        set_state(BSP_WIFI_ERROR, ESP_ERR_NO_MEM);
        return;
    }
    if (xTaskCreate(bringup_worker, "wifi_bringup", 8192, NULL, 2, NULL) != pdPASS)
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
    char encoded[65];
    size_t len = strlen(s.ssid);
    for (size_t n = 0; n < len; ++n) snprintf(encoded+n*2,3,"%02x",(unsigned char)s.ssid[n]);
    encoded[len*2] = 0;
    char line[448];
    snprintf(line, sizeof(line),
             "MEO:WIFI token=%s state=%s stage=%s error=%d en=%d ssid_hex=%s rssi=%d ip=%s aps=%u raw=%u scanning=%u scan_seq=%u scan_done=%u scan_error=%d save_error=%d radio=%u reset=0\n",
             token, state_name(s.state), s_stage, s.error, gpio_get_level(PIN_C5_RESET),
             encoded[0] ? encoded : "-", s.rssi, s.ip[0] ? s.ip : "-", s.ap_count, s.ap_raw,
             s.scanning, (unsigned)s.scan_seq, (unsigned)s.scan_done_seq, s.scan_error, s.save_error, s.radio_ready);
    (void)bsp_usb_cdc_write(line);
}

static void report_scan(const char *token, int index)
{
    char names[WIFI_MAX_AP][33]; int rssi[WIFI_MAX_AP];
    bsp_wifi_status_t s;
    unsigned count = bsp_wifi_snapshot(names, rssi, WIFI_MAX_AP, &s);
    char line[256];
    if (index < 0) {
        snprintf(line, sizeof(line), "MEO:WIFI_SCAN token=%s seq=%u done=%u scanning=%u error=%d count=%u encoding=hex\n",
                 token,(unsigned)s.scan_seq,(unsigned)s.scan_done_seq,s.scanning,s.scan_error,count);
    } else if ((unsigned)index < count) {
        char encoded[65];
        size_t len = strlen(names[index]);
        for (size_t n = 0; n < len; ++n) snprintf(encoded+n*2,3,"%02x",(unsigned char)names[index][n]);
        encoded[len*2] = 0;
        snprintf(line,sizeof(line),"MEO:WIFI_AP token=%s seq=%u index=%d rssi=%d ssid_hex=%s\n",
                 token,(unsigned)s.scan_done_seq,index,rssi[index],encoded);
    } else {
        snprintf(line,sizeof(line),"MEO:WIFI_REJECT token=%s reason=ap_index\n",token);
    }
    /* Each reply fits the real 512-byte CDC admission gate. Never batch six
     * long SSIDs into a silently dropped oversized USB record. */
    (void)bsp_usb_cdc_write(line);
}

bool bsp_wifi_hosted_command(const char *line)
{
    if (!line || strncmp(line, "WIFI ", 5) != 0) return false;
    char verb[12] = {0}, token[17] = {0};
    int used = 0;
    if (sscanf(line, "WIFI %11s %16s%n", verb, token, &used) != 2) return true;
    for (const char *p = token; *p; ++p) if (!isalnum((unsigned char)*p)) return true;
    if (line[used] && !isspace((unsigned char)line[used])) return true;
    const char *rest = line + used;      /* arguments after "<verb> <token> " */
    while (isspace((unsigned char)*rest)) ++rest;
    if ((!strcmp(verb,"STATUS") || !strcmp(verb,"SCAN") || !strcmp(verb,"RETRY") ||
         !strcmp(verb,"APS")) && *rest) return true;
    char reply[128];
    if (strcmp(verb, "STATUS") == 0) {
        report(token);
    } else if (strcmp(verb, "SCAN") == 0) {
        bool queued = bsp_wifi_scan();
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=SCAN queued=%u\n", token, queued);
        (void)bsp_usb_cdc_write(reply);
    } else if (strcmp(verb, "RETRY") == 0) {
        /* Retry a radio scan only; transport faults require explicit diagnosis. */
        bool ok = bsp_wifi_scan(); /* radio retry only; NEVER spawn/reset transport */
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=RETRY queued=%u\n", token, ok);
        (void)bsp_usb_cdc_write(reply);
    } else if (strcmp(verb, "APS") == 0) {
        report_scan(token, -1);
    } else if (strcmp(verb, "AP") == 0) {
        int index; char extra;
        if (sscanf(rest, "%d %c", &index, &extra) == 1 && index >= 0) report_scan(token, index);
    } else if (strcmp(verb, "JOIN") == 0) {
        char ssid[33] = {0};
        char extra;
        bool queued = strcspn(rest," \t\r\n") <= 32 &&
                      sscanf(rest, "%32s %c", ssid, &extra) == 1 && bsp_wifi_join_saved(ssid);
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=JOIN queued=%u\n", token, queued);
        (void)bsp_usb_cdc_write(reply);
    } else if (strcmp(verb, "SET") == 0) {
        char ssid[33] = {0}, pass[65] = {0};
        char extra;
        size_t name_len = strcspn(rest," \t\r\n");
        const char *key = rest + name_len;
        while (isspace((unsigned char)*key)) ++key;
        int fields = sscanf(rest, "%32s %64s %c", ssid, pass, &extra);
        bool queued = name_len<=32 && strcspn(key," \t\r\n")<=64 &&
                      (fields == 1 || fields == 2) && bsp_wifi_set_credentials(ssid, fields == 2 ? pass : "");
        wipe(pass, sizeof(pass));
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s op=SET queued=%u\n", token, queued);
        (void)bsp_usb_cdc_write(reply);
    } else {
        snprintf(reply, sizeof(reply), "MEO:WIFI_REJECT token=%s reason=unknown_op\n", token);
        (void)bsp_usb_cdc_write(reply);
    }
    return true;
}
