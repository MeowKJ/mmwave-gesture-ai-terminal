/* Product adapter: the audited official pipeline still owns display and touch.
 * Only the UI creation call differs, not its init table or timing. */
#include "meoave_app.h"
#include "meoave_ui.h"
#include "fls2_display_diag.h"
#include "bsp_power.h"
#include "bsp_audio.h"
#include "audio_selftest.h"
#include "bsp_usb.h"
#ifdef CONFIG_APP_MEOAVE_WIFI_HOSTED
#ifndef CONFIG_APP_C5_NATIVE_MAINTENANCE
#include "bsp_wifi_hosted.h"
#define MEO_WIFI_FEED 1
#endif
#endif
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static QueueHandle_t s_changes;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready, s_report;
static bool s_storage_ok;
static meo_settings_t s_pending;
static int64_t s_save_at;
static bool s_dirty;
static bool s_save_notice, s_save_ok;
/* Last volume written to the codec; -1 forces the first apply. */
static int s_applied_volume = -1;

/* Power-key timing. 1.5 s of continuous press arms the countdown, then 5 s
 * until the rail is released; letting go at any point cancels it. */
#define KEY_HOLD_ARM_MS        1500
#define KEY_COUNTDOWN_MS       5000
#define KEY_COUNTDOWN_SECONDS  (KEY_COUNTDOWN_MS / 1000)
static struct {
    bool down, counting;
    int64_t down_at, deadline;
    int shown;
} s_key = {.shown = -1};
static bool s_shutdown;          /* explicit power-off accepted; stop the UI */
static int64_t s_power_poll_at;  /* next battery sample, microseconds */
static int s_battery_percent = -1;
static bool s_charging;
static meo_ui_perf_t perf;
static volatile int s_page_req = -1;   /* pending navigation, applied under lock */
static volatile int s_shutdown_req = -2; /* -2 idle, -1 hide, n show n seconds */
/* 0 = idle, >0 = start a window of N ms, -1 = report the last window. */
static volatile int s_fps_req;
static uint32_t s_fps_last_seq;
#ifdef MEO_WIFI_FEED
static int64_t s_wifi_poll_at;
#endif
static int64_t s_mic_poll_at;
static int64_t s_audio_poll_at;
/* Audio view is pushed only on change; the mic route is persisted when a
   calibration discovers a better input mux. */
static meo_audio_view_t s_last_audio_view;
static bsp_audio_mic_route_t s_last_route;
static const char *s_wifi_name = "UNAVAILABLE";

/* Page transitions belong to the LVGL task. Running them from app_main() used
 * the main task's stack, and a busy page (five settings rows, the speaker page)
 * overflowed it: the chip panicked inside printf on a smashed format pointer.
 * lv_async_call() hands the work to the task that already owns the LVGL lock
 * and its much larger stack. */
static void nav_async(void *arg)
{
    meo_ui_navigate((meo_page_t)(intptr_t)arg);
}

static void changed(const meo_settings_t *settings)
{
    /* Called under LVGL lock: no flash writes, DSI IO, logs or USB here. */
    (void)xQueueOverwrite(s_changes, settings);
}

/* Test output from the speaker page. bsp_audio_tone() blocks for its whole
 * duration, so it must never run on the LVGL task. */
static void speaker_tone_task(void *arg)
{
    (void)arg;
    (void)bsp_audio_test_tone(1000, 600);   /* quiet 20% test level */
    vTaskDelete(NULL);
}

static void speaker_test_request(void)
{
    if (xTaskCreate(speaker_tone_task, "spk_test", 4096, NULL, 3, NULL) != pdPASS)
        ESP_LOGW("meoave", "speaker test task not created");
}

static void audio_loop_request(void)
{
    /* The self test owns its own task; "ui" is the CDC correlation token. */
    audio_selftest_start("ui", 800);
    s_audio_poll_at = 0;              /* push RUN to the page immediately */
}

/* The crossed mic harness is compensated in software by the discovered ADC
 * mux codes. Persist them so every boot starts from the calibrated route. */
static void load_route(bsp_audio_mic_route_t *route)
{
    bsp_audio_mic_route_default(route);
    if (!s_storage_ok) return;
    nvs_handle_t handle;
    if (nvs_open("meoave", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(handle, "mic_l", &v) == ESP_OK && v <= 7) route->left_sel = v;
    if (nvs_get_u8(handle, "mic_r", &v) == ESP_OK && v <= 7) route->right_sel = v;
    if (nvs_get_u8(handle, "mic_g", &v) == ESP_OK && v <= 0x0E) route->gain = v;
    if (nvs_get_u8(handle, "mic_sw", &v) == ESP_OK) route->swap = v != 0;
    nvs_close(handle);
}

static esp_err_t save_route(const bsp_audio_mic_route_t *route)
{
    if (!s_storage_ok || !route) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("meoave", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "mic_l", route->left_sel);
    if (err == ESP_OK) err = nvs_set_u8(handle, "mic_r", route->right_sel);
    if (err == ESP_OK) err = nvs_set_u8(handle, "mic_g", route->gain);
    if (err == ESP_OK) err = nvs_set_u8(handle, "mic_sw", route->swap ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

#ifdef MEO_WIFI_FEED
/* Both only queue work for the radio task; the UI never blocks on Wi-Fi. */
static bool wifi_scan_request(void) { return bsp_wifi_scan(); }
static bool wifi_join_request(const char *ssid) { return bsp_wifi_join_saved(ssid); }
/* The settings switch owns the microphone: nothing is captured until it is on. */
static void mic_test_request(bool on)
{
    if (on) (void)bsp_audio_mic_start();
    else bsp_audio_mic_stop();
}

static bool wifi_connect_request(const char *ssid, const char *pass)
{
    return bsp_wifi_set_credentials(ssid, pass);
}
#endif

static void load_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    uint8_t loaded;
    esp_err_t err = nvs_get_u8(handle, key, &loaded);
    if (err == ESP_OK) {
        *value = loaded;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        /* A missing legacy key can use its default. An IO/type error cannot:
         * never persist fallback values over settings we failed to read.
         * Keep this boot read-only until a clean reload on the next boot. */
        s_storage_ok = false;
        ESP_LOGE("meoave", "NVS read %s: %s; writes disabled", key, esp_err_to_name(err));
    }
}

static meo_settings_t load_settings(void)
{
    meo_settings_t settings = meo_default_settings();
    /* Never erase NVS automatically: other namespaces may hold user data. */
    esp_err_t err = nvs_flash_init();
    s_storage_ok = err == ESP_OK;
    if (!s_storage_ok) { ESP_LOGE("meoave", "NVS unavailable: %s", esp_err_to_name(err)); return settings; }
    nvs_handle_t handle;
    err = nvs_open("meoave", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t reduced = 0, list_view = 0, mic_test = 0;
        load_u8(handle, "brightness", &settings.brightness);
        load_u8(handle, "guide_bpm", &settings.guide_bpm);
        load_u8(handle, "volume", &settings.volume);
        load_u8(handle, "reduced", &reduced);
        settings.reduced_motion = reduced != 0;
        load_u8(handle, "app_list", &list_view);
        settings.app_list_view = list_view != 0;
        load_u8(handle, "mic_test", &mic_test);
        settings.mic_test = mic_test != 0;
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        s_storage_ok = false;
        ESP_LOGE("meoave", "NVS open: %s; writes disabled", esp_err_to_name(err));
    }
    meo_validate_settings(&settings);
    return settings;
}

void meoave_app_create(void)
{
    /* Official caller already holds the LVGL lock. */
    s_changes = xQueueCreate(1, sizeof(meo_settings_t));
    ESP_ERROR_CHECK(s_changes ? ESP_OK : ESP_ERR_NO_MEM);
    meo_ui_config_t config = {.settings = load_settings(), .settings_changed = changed,
                              .speaker_test = speaker_test_request,
                              .audio_loop = audio_loop_request};
#ifdef MEO_WIFI_FEED
    config.wifi_scan = wifi_scan_request;
    config.wifi_join = wifi_join_request;
    config.wifi_connect = wifi_connect_request;
    config.mic_test = mic_test_request;
#endif
    meo_ui_create(&config);
    /* Restore the switch state saved in NVS; default is off. */
    if (config.settings.mic_test) (void)bsp_audio_mic_start();
    /* Apply the saved speaker level now that the codec is up. */
    (void)bsp_audio_set_volume(config.settings.volume);
    /* Restore the calibrated mic mux (software compensation for the crossed
       P/N harness); a fresh AUDIO CALIBRATE can replace it at runtime. */
    bsp_audio_mic_route_t route;
    load_route(&route);
    (void)bsp_audio_mic_route_set(&route);
    s_last_route = route;
    if (!s_storage_ok) meo_ui_settings_saved(false);
    (void)xQueueOverwrite(s_changes, &config.settings);
    portENTER_CRITICAL(&s_lock);
    s_ready = true;
    portEXIT_CRITICAL(&s_lock);
}

void meoave_app_request_status(void)
{
    portENTER_CRITICAL(&s_lock);
    s_report = true;
    portEXIT_CRITICAL(&s_lock);
}

static bool save_settings(void)
{
    if (!s_storage_ok) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("meoave", NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(handle, "brightness", s_pending.brightness);
    if (err == ESP_OK) err = nvs_set_u8(handle, "guide_bpm", s_pending.guide_bpm);
    if (err == ESP_OK) err = nvs_set_u8(handle, "reduced", s_pending.reduced_motion);
    if (err == ESP_OK) err = nvs_set_u8(handle, "app_list", s_pending.app_list_view);
    if (err == ESP_OK) err = nvs_set_u8(handle, "mic_test", s_pending.mic_test);
    if (err == ESP_OK) err = nvs_set_u8(handle, "volume", s_pending.volume);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static void power_off_now(void)
{
    s_key.counting = false;
    portENTER_CRITICAL(&s_lock);
    s_shutdown = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW("meoave", "shutdown countdown finished: releasing PWR_HOLD");
    esp_err_t err = bsp_power_off();
    if (err != ESP_OK) ESP_LOGE("meoave", "power off: %s", esp_err_to_name(err));
    /* If VBUS keeps 3V3 alive the MCU is still running: go dark and stay off.
       USB stays enumerated so the host can still recover the board later. */
    if (lvgl_port_lock(20)) {
        meo_ui_set_shutdown(0);
        lvgl_port_unlock();
    }
    if (fls2_display_set_brightness(0) != ESP_OK) ESP_LOGW("meoave", "panel off on shutdown failed");
}

void meoave_app_key_tick(void)
{
    /* Runs on the app task, never on the LVGL task. The GUI lock is tried
       without waiting: a busy GUI simply delays the label by one tick. */
    if (!s_ready || s_shutdown) return;
    int64_t now = esp_timer_get_time() / 1000;
    bool pressed = bsp_power_key_pressed();
    bool home = false;
    int countdown = -2;                 /* -2: unchanged, -1: hide, >=0: show */
    if (pressed && !s_key.down) {
        s_key.down = true;
        s_key.down_at = now;
        if (s_key.counting) {           /* defensive: never keep a stale countdown */
            s_key.counting = false;
            countdown = -1;
        }
    } else if (pressed && s_key.down && !s_key.counting) {
        if (now - s_key.down_at >= KEY_HOLD_ARM_MS) {
            s_key.counting = true;
            s_key.deadline = now + KEY_COUNTDOWN_MS;
            s_key.shown = -1;
        }
    } else if (!pressed && s_key.down) {
        s_key.down = false;
        if (s_key.counting) {
            /* Release restores the UI immediately: the countdown only exists
               while the key is held, so a slip of the finger cannot power off. */
            s_key.counting = false;
            countdown = -1;
        } else if (now - s_key.down_at < KEY_HOLD_ARM_MS) {
            home = true;
        }
    }
    bool poweroff = false;
    if (s_key.counting) {
        int64_t left = s_key.deadline - now;
        int seconds = left > 0 ? (int)((left + 999) / 1000) : 0;
        if (seconds != s_key.shown) countdown = seconds;
        poweroff = seconds == 0;
    }
    if (home || countdown != -2) {
        if (!lvgl_port_lock(0)) return; /* retried next tick; no state latched */
        if (home) meo_ui_navigate(MEO_HOME);
        if (countdown != -2) {
            meo_ui_set_shutdown(countdown);
            s_key.shown = countdown;
        }
        lvgl_port_unlock();
    }
    if (poweroff) power_off_now();
}

void meoave_app_request_page(const char *name)
{
    static const struct { const char *name; meo_page_t page; } map[] = {
        {"eyes", MEO_HOME}, {"apps", MEO_APPS}, {"settings", MEO_SETTINGS},
        {"wifi", MEO_WIFI}, {"breathe", MEO_BREATHE}, {"display", MEO_DISPLAY},
        {"accessibility", MEO_ACCESSIBILITY}, {"password", MEO_PASSWORD},
        {"mic", MEO_MIC}, {"speaker", MEO_SPEAKER},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (name && strcmp(name, map[i].name) == 0) { s_page_req = (int)map[i].page; return; }
}

void meoave_app_request_shutdown(int seconds)
{
    s_shutdown_req = seconds < 0 ? -1 : seconds;
}

void meoave_app_request_fps(int ms)
{
    s_fps_req = ms > 0 ? ms : -1;
}

void meoave_app_service(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready, report = s_report;
    if (ready) s_report = false;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) return;
    if (s_shutdown) return;           /* rail released: keep the UI frozen */
    if (s_shutdown_req != -2 && lvgl_port_lock(5)) {
        meo_ui_set_shutdown(s_shutdown_req < 0 ? -1 : s_shutdown_req);
        s_shutdown_req = -2;
        lvgl_port_unlock();
    }
    if (s_page_req >= 0 && lvgl_port_lock(5)) {
        /* Shell-driven navigation (UI GO) for tests and bring-up. */
        meo_page_t page = (meo_page_t)s_page_req;
        s_page_req = -1;
        lv_async_call(nav_async, (void *)(intptr_t)page);
        lvgl_port_unlock();
    }
    int fps_req = s_fps_req;
    if (fps_req && lvgl_port_lock(5)) {
        s_fps_req = 0;
        if (fps_req > 0) {
            meo_ui_fps_start((uint32_t)fps_req, "manual");
        } else {
            meo_ui_fps_t f;
            meo_ui_fps_read(&f);
            char line[160];
            snprintf(line, sizeof(line),
                     "MEO:UIFPS_READ done=%lu tag=%s frames=%lu fps=%lu.%lu "
                     "span_ms=%lu max_ms=%lu jank=%lu render_max=%lu flush_max=%lu\n",
                     (unsigned long)f.done, f.tag, (unsigned long)f.frames,
                     (unsigned long)(f.fps_x10 / 10), (unsigned long)(f.fps_x10 % 10),
                     (unsigned long)f.span_ms, (unsigned long)f.max_frame_ms,
                     (unsigned long)f.jank, (unsigned long)f.max_render_us,
                     (unsigned long)f.max_flush_us);
            (void)bsp_usb_cdc_write(line);
        }
        lvgl_port_unlock();
    }
    /* Publish every finished frame-pacing window from the app task. Doing the
       CDC write on the GUI task used to stall the frames being measured.
       Poll at 10 Hz, not every service tick: taking the GUI mutex 50 times a
       second is itself a source of frame jitter. */
    static int64_t s_fps_poll_at;
    int64_t fps_now = esp_timer_get_time();
    if (fps_now >= s_fps_poll_at) {
        s_fps_poll_at = fps_now + 100000;
        meo_ui_fps_t f;
        if (lvgl_port_lock(0)) {
            meo_ui_fps_read(&f);
            lvgl_port_unlock();
            if (f.seq && f.seq != s_fps_last_seq) {
                s_fps_last_seq = f.seq;
                char line[160];
                snprintf(line, sizeof(line),
                         "MEO:UIFPS tag=%s frames=%lu fps=%lu.%lu span_ms=%lu "
                         "max_ms=%lu jank=%lu render_max=%lu flush_max=%lu "
                         "slow_render=%lu slow_gap=%lu\n",
                         f.tag, (unsigned long)f.frames,
                         (unsigned long)(f.fps_x10 / 10), (unsigned long)(f.fps_x10 % 10),
                         (unsigned long)f.span_ms, (unsigned long)f.max_frame_ms,
                         (unsigned long)f.jank, (unsigned long)f.max_render_us,
                         (unsigned long)f.max_flush_us,
                         (unsigned long)f.slow_render, (unsigned long)f.slow_gap);
                (void)bsp_usb_cdc_write(line);
            }
        }
    }
    int64_t now_us = esp_timer_get_time();
    if (now_us >= s_power_poll_at) {
        /* Once every 2 s: 5 ADC reads plus the VBUS divider. Never inside the
           LVGL task and never while the GUI lock is already held. */
        s_power_poll_at = now_us + 2000000;
        s_battery_percent = bsp_power_battery_percent();
        s_charging = bsp_power_charging();
        if (lvgl_port_lock(5)) {
            meo_ui_set_power(s_battery_percent, s_charging);
            lvgl_port_unlock();
        }
    }
#ifdef MEO_WIFI_FEED
    if (now_us >= s_wifi_poll_at) {
        /* Once every second: read the cached transport state only. The Hosted
           worker owns the radio; this task never scans or joins on its own. */
        s_wifi_poll_at = now_us + 1000000;
        bsp_wifi_status_t wifi = bsp_wifi_hosted_status();
        meo_wifi_state_t state;
        switch (wifi.state) {
        case BSP_WIFI_ONLINE:     state = MEO_WIFI_ONLINE; s_wifi_name = "ONLINE"; break;
        case BSP_WIFI_SCANNING:
        case BSP_WIFI_JOINING:
        case BSP_WIFI_STARTING:   state = MEO_WIFI_CONNECTING; s_wifi_name = "CONNECTING"; break;
        case BSP_WIFI_ERROR:      state = MEO_WIFI_ERROR; s_wifi_name = "ERROR"; break;
        default:                  state = MEO_WIFI_OFF; s_wifi_name = "OFF"; break;
        }
        char ap_ssid[6][33];
        int ap_rssi[6];
        unsigned ap_count = bsp_wifi_ap_list(ap_ssid, ap_rssi, 6);
        if (lvgl_port_lock(5)) {
            meo_ui_set_wifi(state, wifi.ssid, wifi.rssi);
            meo_ui_set_scan_list((const char (*)[33])ap_ssid, ap_rssi, ap_count);
            lvgl_port_unlock();
        }
    }
#endif
    if (now_us >= s_mic_poll_at) {
        /* 10 Hz: enough for a responsive meter, cheap enough for the app task.
           Amplitude is mapped on a dB scale, otherwise normal speech only
           lights the first two bars. */
        s_mic_poll_at = now_us + 100000;
        int rms[2] = {0, 0}, peak[2] = {0, 0};
        bsp_audio_mic_level(rms, peak);
        float level[2];
        for (int ch = 0; ch < 2; ++ch) {
            float db = rms[ch] > 0 ? 20.0f * log10f((float)rms[ch] / 32768.0f) : -96.0f;
            float v = (db + 60.0f) / 54.0f;      /* -60 dB .. -6 dB -> 0..1 */
            level[ch] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
        if (lvgl_port_lock(5)) {
            meo_ui_set_mic(level, bsp_audio_mic_active());
            lvgl_port_unlock();
        }
    }
    /* 5 Hz audio snapshot: only changed values reach LVGL, and a discovered
       mic route is written to NVS here (never from inside the GUI lock). */
    if (now_us >= s_audio_poll_at) {
        s_audio_poll_at = now_us + 200000;
        audio_loop_result_t loop;
        bsp_audio_status_t status;
        audio_selftest_result(&loop);
        bsp_audio_status(&status);
        meo_audio_view_t view = {
            .volume = status.volume,
            .mclk_duty = status.mclk_duty,
            .loop_state = (int)loop.state,
            .loop_score = loop.score,
            .route_l = status.route.left_sel,
            .route_r = status.route.right_sel,
            .busy = audio_selftest_busy(),
            .mic_running = status.mic_running,
        };
        if (memcmp(&view, &s_last_audio_view, sizeof(view)) != 0) {
            s_last_audio_view = view;
            if (lvgl_port_lock(5)) { meo_ui_set_audio(&view); lvgl_port_unlock(); }
        }
        if (memcmp(&status.route, &s_last_route, sizeof(status.route)) != 0) {
            s_last_route = status.route;
            (void)save_route(&status.route);
        }
    }
    meo_settings_t latest;
    if (xQueueReceive(s_changes, &latest, 0) == pdTRUE) {
        s_pending = latest;
        /* The boot warm-up keeps the backlight at 0 while it rasterises every
           page; warmup_abort() restores the real level afterwards. */
        if (!meo_ui_warmup_active()) {
            esp_err_t err = fls2_display_set_brightness(latest.brightness);
            if (err != ESP_OK) ESP_LOGW("meoave", "brightness: %s", esp_err_to_name(err));
        }
        /* Dragging the slider fires many updates; only touch I2C when the
           level actually changed, and never from the LVGL task. */
        if (latest.volume != s_applied_volume) {
            s_applied_volume = latest.volume;
            esp_err_t verr = bsp_audio_set_volume(latest.volume);
            if (verr != ESP_OK) ESP_LOGW("meoave", "volume: %s", esp_err_to_name(verr));
        }
        s_save_at = esp_timer_get_time() + 600000;
        s_dirty = true;
        s_save_notice = false;
    }
    if (s_dirty && esp_timer_get_time() >= s_save_at) {
        s_save_ok = save_settings();
        s_dirty = false; /* GUI lock contention must never repeat flash IO. */
        s_save_notice = true;
    }
    if (s_save_notice && lvgl_port_lock(5)) {
        /* Do not acknowledge an older save over a newer slider change. */
        if (uxQueueMessagesWaiting(s_changes) == 0) meo_ui_settings_saved(s_save_ok);
        lvgl_port_unlock();
        s_save_notice = false;
    }
    if (report && !lvgl_port_lock(5)) {
        meoave_app_request_status(); /* Retry later; USB never waits on GUI. */
    } else if (report) {
        const meo_model_t *m = meo_ui_model();
        meo_ui_perf(&perf);
        char line[320];
        snprintf(line, sizeof(line), "MEO:UI page=%s frames=%lu rotation=180 brightness=%u "
                 "reduced=%d running=%d mode=%s elapsed_ms=%llu wifi=%s radar=%d app_view=%s eye=%u eye_auto=%d "
                 "battery=%d charging=%d cjk=%d warm=%d frame_ms=%lu busy_us=%lu render_us=%lu flush_us=%lu refreshes=%lu px_s=%lu flushes=%lu max_px=%lu area=%ld,%ld,%ld,%ld\n",
                 meo_ui_page_name(m->page), (unsigned long)meo_ui_frames(), m->settings.brightness,
                 m->settings.reduced_motion, m->running, m->mode == MEO_GUIDED ? "GUIDED" : "RADAR",
                 (unsigned long long)m->elapsed_ms, s_wifi_name,
                 m->respiration_live,m->settings.app_list_view ? "LIST" : "GRID",
                 (unsigned)m->expression, !m->expression_override,
                 s_battery_percent, s_charging,
                 meo_ui_font_has("\xe8\xbf\x9e\xe6\x8e\xa5\xe4\xb8\xad"),  /* 连接中 */
                 meo_ui_warmup_count(),
                 (unsigned long)perf.period_ms, (unsigned long)perf.busy_us,
                 (unsigned long)perf.render_us, (unsigned long)perf.flush_us,
                 (unsigned long)perf.refreshes, (unsigned long)perf.px_per_s,
                 (unsigned long)perf.flushes, (unsigned long)perf.max_px,
                 (long)perf.max_area[0], (long)perf.max_area[1],
                 (long)perf.max_area[2], (long)perf.max_area[3]);
        lvgl_port_unlock();
        (void)bsp_usb_cdc_write(line);
    }
}
