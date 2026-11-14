/* Product adapter: the audited official pipeline still owns display and touch.
 * Only the UI creation call differs, not its init table or timing. */
#include "meoave_app.h"
#include "meoave_ui.h"
#include "meoave_conversation.h"
#include "meoave_voice.h"
#include "meoave_voice_link.h"
#include "meoave_wake.h"
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
static int s_applied_mic_gain = -1;
static int s_mic_request = -1;
static bool s_music_requested, s_music_busy;
static esp_err_t s_music_error;
static uint32_t s_music_seq;
static bool s_interaction_requested;
static uint32_t s_interaction_requests, s_interaction_played;
static esp_err_t s_interaction_error;

/* Power-key timing. 1.5 s of continuous press arms the countdown, then 3 s
 * until the rail is released; letting go at any point cancels it. */
#define KEY_HOLD_ARM_MS        1500
#define KEY_COUNTDOWN_MS       3000
static struct {
    bool down, counting;
    int64_t down_at, deadline;
    int shown;
    int pending;
    bool home_pending;
} s_key = {.shown = -1, .pending = -2};
static bool s_shutdown;          /* explicit power-off accepted; stop the UI */
static int64_t s_power_poll_at;  /* next battery sample, microseconds */
static int s_battery_percent = -1;
static bool s_charging;
static meo_ui_perf_t perf;
static volatile int s_page_req = -1;   /* pending navigation, applied under lock */
static volatile int s_volume_req = -1;
static volatile int s_shutdown_req = -2; /* -2 idle, -1 hide, n show n seconds */
/* 0 = idle, >0 = start a window of N ms, -1 = report the last window. */
static volatile int s_fps_req;
static uint32_t s_fps_last_seq;
static void interaction_sound_request(void);
#ifdef MEO_WIFI_FEED
static int64_t s_wifi_poll_at;
#endif
static int64_t s_mic_poll_at;
static int64_t s_audio_poll_at;
/* Audio view is pushed only on change; the mic route is persisted when a
   calibration discovers a better input mux. */
static meo_audio_view_t s_last_audio_view;
static bsp_audio_mic_route_t s_last_route;
static bool s_route_dirty;
static int64_t s_route_save_at;
static const char *s_wifi_name = "UNAVAILABLE";
/* The conversation engine is intentionally transport-free for this first
   vertical slice.  The app task owns it; the GUI only receives expression
   changes under the LVGL lock. */
static meo_conversation_t s_conversation;
static meo_conversation_view_t s_ai_view;
static uint32_t s_ai_view_seq;
static uint32_t s_ai_applied_seq;
static volatile int s_ai_request;       /* 0 none, 1 demo, 2 cancel */
static volatile bool s_ai_report_request;
static bool s_voice_demo_request,s_voice_ui_owned;
static uint32_t s_voice_applied_seq;
static uint32_t s_voice_logged_generation,s_voice_logged_sentence;
static unsigned s_voice_logged_cue=UINT32_MAX;
static volatile bool s_wake_text_pending;
static char s_wake_text[48];
static uint32_t s_wake_logged;
static bool s_wake_started;
/* Written by actual page transitions, not inferred from lock availability. */
static bool s_home_listening;

static void wake_audio_frame(const int16_t *stereo, size_t frames, uint64_t now_ms, void *ctx)
{
    /* Apply output inhibition at the input boundary, not just at the slower
       UI tick. Buffered audio from before playback is invalidated by epoch. */
    meoave_espsr_set_output_blocked(bsp_audio_speaker_on() ||
                                   !bsp_audio_mic_capturing() || audio_selftest_busy(), now_ms);
    meo_wake_feed_audio(stereo, frames, now_ms, ctx);
}

static void report_voice(const meo_voice_view_t *v)
{
    char line[320];
    snprintf(line,sizeof(line),"MEO:VOICE busy=%d fixture=%d generation=%lu sentence=%lu "
        "played=%lu frames=%lu cue=%u expression=%s intensity=%u underruns=%lu error=%d\n",
        v->busy,meo_voice_demo_available(),(unsigned long)v->generation,(unsigned long)v->sentence_id,
        (unsigned long)(v->has_frame ? v->frame.played_frames : 0),(unsigned long)v->total_frames,
        v->has_frame ? v->frame.cue_index : 0,
        meo_expression_name(v->has_frame ? v->frame.expression : MEO_EYE_CALM),
        (unsigned)(v->has_frame ? v->frame.intensity*100+.5f : 0),
        (unsigned long)v->underruns,v->error);
    (void)bsp_usb_cdc_write(line);
}

static void apply_voice_expression(bool report)
{
    meo_voice_view_t v;meo_voice_view(&v);
    if (v.sequence!=s_voice_applied_seq && lvgl_port_lock(5)) {
        if (v.busy && v.has_frame) {
            meo_ui_set_sentence_pose(v.frame.expression,v.frame.intensity,&v.frame.pose);
            s_voice_ui_owned=true;
        } else if (!v.busy && s_voice_ui_owned) {
            meo_ui_auto_expression();s_voice_ui_owned=false;
        }
        lvgl_port_unlock();s_voice_applied_seq=v.sequence;
    }
    bool cue=v.busy && v.has_frame && (v.generation!=s_voice_logged_generation ||
        v.sentence_id!=s_voice_logged_sentence || v.frame.cue_index!=s_voice_logged_cue);
    if (report || cue) report_voice(&v);
    if (cue) {
        s_voice_logged_generation=v.generation;s_voice_logged_sentence=v.sentence_id;
        s_voice_logged_cue=v.frame.cue_index;
    }
}

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

static void conversation_changed(const meo_conversation_view_t *view, void *ctx)
{
    (void)ctx;
    s_ai_view = *view;
    ++s_ai_view_seq;
    if (s_ai_view_seq == 0) ++s_ai_view_seq;
}

static void report_ai_status(void)
{
    char line[224];
    snprintf(line, sizeof(line),
             "MEO:AI state=%s expression=%s active=%d generation=%lu prompt=%s response=%s\n",
             meo_conversation_state_name(s_ai_view.state),
             meo_conversation_expression_name(s_ai_view.expression),
             s_ai_view.active, (unsigned long)s_ai_view.generation,
             meo_conversation_demo_prompt(), meo_conversation_demo_response());
    (void)bsp_usb_cdc_write(line);
}

static void apply_ai_expression(void)
{
    if (!s_ai_view_seq || s_ai_view_seq == s_ai_applied_seq) return;
    if (!lvgl_port_lock(5)) return;
    /* IDLE returns ownership to the normal home-page mood cycle. Every other
       phase is an explicit hold so the idle slideshow cannot overwrite the
       conversation expression halfway through a turn. */
    if (s_ai_view.state == MEO_CONV_IDLE) {
        meo_ui_auto_expression();
    } else {
        meo_ui_set_expression(s_ai_view.expression, 0);
    }
    lvgl_port_unlock();
    s_ai_applied_seq = s_ai_view_seq;
}

/* Test output from the speaker page. The melody blocks for its whole
 * duration, so it must never run on the LVGL task. */
static void speaker_tone_task(void *arg)
{
    (void)arg;
    esp_err_t err = bsp_audio_preview_music();
    bsp_audio_output_release();
    ESP_LOGI("meoave", "speaker preview: %s", esp_err_to_name(err));
    portENTER_CRITICAL(&s_lock);
    s_music_error = err;
    s_music_busy = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static bool speaker_test_request(void)
{
    if (!bsp_audio_output_try_acquire()) return false;
    portENTER_CRITICAL(&s_lock);
    bool accepted = !s_music_busy;
    if (accepted) {
        s_music_requested = s_music_busy = true;
        s_music_error = ESP_OK;
        ++s_music_seq;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!accepted) bsp_audio_output_release();
    return accepted;
}

bool meoave_app_request_preview(void)
{
    return s_ready && !s_shutdown && speaker_test_request();
}

bool meoave_app_request_interaction_sound(void)
{
    if (!s_ready || s_shutdown) return false;
    interaction_sound_request();
    return true;
}

static void mic_test_request(bool on)
{
    /* GUI callback only queues intent. I2S starts/stops on the app task. */
    portENTER_CRITICAL(&s_lock);
    s_mic_request = on;
    portEXIT_CRITICAL(&s_lock);
}

static void ai_mic_test_request(bool on)
{
    portENTER_CRITICAL(&s_lock);
    s_home_listening = on;
    s_mic_request = on;
    /* Leaving the dial invalidates in-flight inference immediately. */
    if (!on) meoave_espsr_set_armed(false);
    portEXIT_CRITICAL(&s_lock);
}

static void audio_loop_request(void)
{
    /* The self test owns its own task; "ui" is the CDC correlation token. */
    audio_selftest_start("ui", 800);
    s_audio_poll_at = 0;              /* push RUN to the page immediately */
}

static void interaction_sound_request(void)
{
    /* UI callbacks never touch the codec. The app task owns the short cue and
       may drop it when a longer preview/self-test already owns the output. */
    portENTER_CRITICAL(&s_lock);
    s_interaction_requested = true;
    ++s_interaction_requests;
    portEXIT_CRITICAL(&s_lock);
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
        uint8_t reduced = 0, list_view = 0;
        load_u8(handle, "brightness", &settings.brightness);
        load_u8(handle, "guide_bpm", &settings.guide_bpm);
        load_u8(handle, "volume", &settings.volume);
        load_u8(handle, "reduced", &reduced);
        settings.reduced_motion = reduced != 0;
        load_u8(handle, "app_list", &list_view);
        settings.app_list_view = list_view != 0;
        load_u8(handle, "mic_g", &settings.mic_gain);
        uint8_t interaction = 1;
        load_u8(handle, "touch_snd", &interaction);
        settings.interaction_sound = interaction != 0;
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
                              .mic_test = mic_test_request,
                              .ai_mic_test = ai_mic_test_request,
                              .audio_loop = audio_loop_request,
                              .interaction_sound = interaction_sound_request};
    s_pending=config.settings;
#ifdef MEO_WIFI_FEED
    config.wifi_scan = wifi_scan_request;
    config.wifi_join = wifi_join_request;
    config.wifi_connect = wifi_connect_request;
#endif
    meo_ui_create(&config);
    meo_conversation_init(&s_conversation, (uint64_t)(esp_timer_get_time() / 1000),
                          conversation_changed, NULL);
    /* The ESP-SR tap consumes the existing mic worker's blocks.  It never
       opens I2S RX itself, so UI metering and wake detection share one reader
       without racing the IDF channel lock. */
    (void)bsp_audio_mic_set_frame_callback(wake_audio_frame, NULL);
    meo_voice_link_init();
    /* Apply the saved speaker level now that the codec is up. */
    (void)bsp_audio_set_volume(config.settings.volume);
    /* Restore the calibrated mic mux (software compensation for the crossed
       P/N harness); a fresh AUDIO CALIBRATE can replace it at runtime. */
    bsp_audio_mic_route_t route;
    load_route(&route);
    route.gain = config.settings.mic_gain;
    (void)bsp_audio_mic_route_set(&route);
    s_last_route = route;
    if (!s_storage_ok) meo_ui_settings_saved(false);
    (void)xQueueOverwrite(s_changes, &config.settings);
    portENTER_CRITICAL(&s_lock);
    s_ready = true;
    portEXIT_CRITICAL(&s_lock);
    /* The AI dial is the default listening surface. Starting capture here
       keeps the home indicator honest from the first service tick. */
    ai_mic_test_request(true);
}

bool meoave_app_request_wake_text(const char *text)
{
    if (!s_ready || s_shutdown || !text || !*text) return false;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_wake_text,sizeof(s_wake_text),"%s",text);
    s_wake_text_pending=true;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool meoave_app_request_ai_demo(void)
{
    if (!s_ready || s_shutdown) return false;
    meo_voice_view_t voice;meo_voice_view(&voice);
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_ai_request == 0 && !s_voice_demo_request && !voice.busy && !meo_conversation_active(&s_conversation);
    if (accepted) s_ai_request = 1;
    portEXIT_CRITICAL(&s_lock);
    return accepted;
}

bool meoave_app_request_voice_demo(void)
{
    if (!s_ready || s_shutdown || !meo_voice_demo_available()) return false;
    meo_voice_view_t voice;meo_voice_view(&voice);
    portENTER_CRITICAL(&s_lock);
    bool ok=!s_voice_demo_request && !voice.busy && !s_ai_request && !meo_conversation_active(&s_conversation);
    if (ok) s_voice_demo_request=true;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

bool meoave_app_request_ai_cancel(void)
{
    if (!s_ready || s_shutdown) return false;
    meo_voice_view_t voice;meo_voice_view(&voice);
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_ai_request == 0 && (meo_conversation_active(&s_conversation) || voice.busy || s_voice_demo_request);
    if (accepted) s_ai_request = 2;
    portEXIT_CRITICAL(&s_lock);
    return accepted;
}

void meoave_app_request_ai_status(void)
{
    portENTER_CRITICAL(&s_lock);
    s_ai_report_request = true;
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
    if (err == ESP_OK) err = nvs_set_u8(handle, "mic_g", s_pending.mic_gain);
    if (err == ESP_OK) err = nvs_set_u8(handle, "volume", s_pending.volume);
    if (err == ESP_OK) err = nvs_set_u8(handle, "touch_snd", s_pending.interaction_sound ? 1 : 0);
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
    /* Release is an edge, not a continuously available value. Keep the cancel
       request until the GUI lock succeeds, otherwise a busy frame strands the
       shutdown screen forever after the key has already been released. */
    if (home) s_key.home_pending=true;
    if (countdown!=-2) s_key.pending=countdown;
    if (s_key.home_pending || s_key.pending != -2) {
        if (!lvgl_port_lock(0)) return;
        if (s_key.pending != -2) {
            meo_ui_set_shutdown(s_key.pending);
            s_key.shown = s_key.pending;
            s_key.pending=-2;
        }
        if (s_key.home_pending) {meo_ui_navigate(MEO_HOME);s_key.home_pending=false;}
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
        {"mic", MEO_MIC}, {"speaker", MEO_SPEAKER}, {"expressions", MEO_EXPRESSIONS},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (name && strcmp(name, map[i].name) == 0) { s_page_req = (int)map[i].page; return; }
}

void meoave_app_request_volume(int percent)
{
    if (percent >= 0 && percent <= 100) s_volume_req = percent;
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
    int ai_request;
    bool ai_report;
    bool voice_demo;
    portENTER_CRITICAL(&s_lock);
    ai_request = s_ai_request;
    s_ai_request = 0;
    ai_report = s_ai_report_request;
    s_ai_report_request = false;
    voice_demo=s_voice_demo_request;s_voice_demo_request=false;
    portEXIT_CRITICAL(&s_lock);
    uint64_t ai_now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (ai_request == 1) (void)meo_conversation_start(&s_conversation, ai_now_ms);
    else if (ai_request == 2) {
        (void)meo_conversation_cancel(&s_conversation, ai_now_ms);
        (void)meo_voice_cancel();voice_demo=false;
    }
    if (voice_demo) {
        bool ok=meo_voice_demo();
        (void)bsp_usb_cdc_write(ok ? "MEO:VOICE_START accepted=1 offline=1\n" :
            "MEO:VOICE_START accepted=0 reason=BUSY_OR_FIXTURE_INVALID\n");
    }
    meo_conversation_tick(&s_conversation, ai_now_ms);
    apply_ai_expression();
    if (ai_report) report_ai_status();
    apply_voice_expression(ai_report);
    if (s_volume_req >= 0 && lvgl_port_lock(5)) {
        int percent = s_volume_req;
        s_volume_req = -1;
        meo_ui_set_volume(percent);
        lvgl_port_unlock();
    }
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
    uint64_t now_ms=(uint64_t)(now_us/1000);
    bool wake_text_pending;
    char wake_text[sizeof(s_wake_text)];
    portENTER_CRITICAL(&s_lock);
    wake_text_pending=s_wake_text_pending;
    if (wake_text_pending) {
        snprintf(wake_text,sizeof(wake_text),"%s",s_wake_text);
        s_wake_text_pending=false;
    }
    portEXIT_CRITICAL(&s_lock);
    meo_voice_view_t wake_voice;
    meo_voice_view(&wake_voice);
    bool home, wake_warmup_done = false;
    if (!s_wake_started && lvgl_port_lock(5)) {
        wake_warmup_done = !meo_ui_warmup_active();
        lvgl_port_unlock();
    }
    if (!s_wake_started && wake_warmup_done) {
        meo_wake_init(now_ms);
        s_wake_started = true;
    }
    /* A busy renderer is not a page change: repeatedly disarming here used
       to destroy speech context every time the 5 ms GUI lock timed out. */
    meoave_espsr_set_output_blocked(bsp_audio_speaker_on() ||
                                   !bsp_audio_mic_capturing() || audio_selftest_busy(), now_ms);
    portENTER_CRITICAL(&s_lock);
    home = s_home_listening;
    meo_wake_set_armed(s_wake_started && home && !s_shutdown && !wake_voice.busy &&
                       !meo_conversation_active(&s_conversation) && bsp_audio_mic_capturing(), now_ms);
    portEXIT_CRITICAL(&s_lock);
    if (wake_text_pending) (void)meo_wake_submit_text(wake_text, now_ms);
    meo_wake_service(now_ms);
    meo_wake_tick(now_ms);
    meo_wake_view_t wake; meo_wake_view(&wake);
    bool wake_fresh = wake.detections != s_wake_logged;
    if (wake_fresh) {
        s_wake_logged = wake.detections;
        char line[192];
        snprintf(line, sizeof(line), "MEO:WAKE keyword=%s backend=%s source=%s detected=1 count=%lu\n",
                 MEO_WAKE_KEYWORD, wake.backend, wake.event_source, (unsigned long)wake.detections);
        (void)bsp_usb_cdc_write(line);
    }
    if (lvgl_port_lock(5)) {
        if (wake_fresh && home) meo_ui_set_expression(MEO_EYE_CURIOUS, 1200);
        meo_ui_set_wake_state(wake.detected);
        lvgl_port_unlock();
    }
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
        bsp_wifi_status_t wifi;
        char ap_ssid[6][33];
        int ap_rssi[6];
        unsigned ap_count = bsp_wifi_snapshot(ap_ssid, ap_rssi, 6, &wifi);
        meo_wifi_state_t state;
        switch (wifi.state) {
        case BSP_WIFI_ONLINE:     state = MEO_WIFI_ONLINE; s_wifi_name = "ONLINE"; break;
        case BSP_WIFI_JOINING:
                                  state = MEO_WIFI_CONNECTING; s_wifi_name = "CONNECTING"; break;
        case BSP_WIFI_STARTING:   state = MEO_WIFI_STARTING; s_wifi_name = "STARTING"; break;
        case BSP_WIFI_SCANNING:   state = MEO_WIFI_READY; s_wifi_name = "SCANNING"; break;
        case BSP_WIFI_READY:      state = MEO_WIFI_READY; s_wifi_name = "READY"; break;
        case BSP_WIFI_ERROR:      state = wifi.radio_ready ? MEO_WIFI_ERROR : MEO_WIFI_UNAVAILABLE;
                                  s_wifi_name = wifi.radio_ready ? "ERROR" : "UNAVAILABLE"; break;
        default:                  state = MEO_WIFI_OFF; s_wifi_name = "OFF"; break;
        }
        if (lvgl_port_lock(5)) {
            meo_ui_set_wifi(state, wifi.ssid, wifi.rssi);
            meo_ui_set_scan_list((const char (*)[33])ap_ssid, ap_rssi, ap_count);
            meo_ui_set_wifi_scan(wifi.scanning,wifi.scan_error,wifi.scan_done_seq,wifi.save_error);
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
            meo_ui_set_mic(level, bsp_audio_mic_capturing());
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
        portENTER_CRITICAL(&s_lock);
        view.playback_busy = s_music_busy;
        view.playback_error = s_music_error;
        view.playback_seq = s_music_seq;
        portEXIT_CRITICAL(&s_lock);
        if (memcmp(&view, &s_last_audio_view, sizeof(view)) != 0) {
            if (lvgl_port_lock(5)) {
                /* A new GUI request may have arrived between snapshot and
                   lock acquisition. Never acknowledge an older idle state. */
                portENTER_CRITICAL(&s_lock);
                view.playback_busy=s_music_busy;
                view.playback_error=s_music_error;
                view.playback_seq=s_music_seq;
                portEXIT_CRITICAL(&s_lock);
                meo_ui_set_audio(&view); s_last_audio_view=view; lvgl_port_unlock();
            }
        }
        if (memcmp(&status.route, &s_last_route, sizeof(status.route)) != 0) {
            bool mux_changed=status.route.left_sel!=s_last_route.left_sel ||
                             status.route.right_sel!=s_last_route.right_sel ||
                             status.route.swap!=s_last_route.swap;
            s_last_route = status.route;
            /* Slider gain is saved by the settings debounce below, never by
               this 5 Hz poll. Calibration mux changes settle before saving. */
            if (mux_changed) {s_route_dirty=true;s_route_save_at=now_us+600000;}
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
            esp_err_t verr = bsp_audio_set_volume(latest.volume);
            if (verr == ESP_OK) s_applied_volume = latest.volume;
            if (verr != ESP_OK) ESP_LOGW("meoave", "volume: %s", esp_err_to_name(verr));
        }
        if (latest.mic_gain != s_applied_mic_gain) {
            bsp_audio_mic_route_t route;
            bsp_audio_mic_route_get(&route);
            route.gain = latest.mic_gain;
            esp_err_t err = bsp_audio_mic_route_set(&route);
            if (err == ESP_OK) s_applied_mic_gain = latest.mic_gain;
            else ESP_LOGW("meoave", "microphone gain: %s", esp_err_to_name(err));
        }
        s_save_at = esp_timer_get_time() + 600000;
        s_dirty = true;
        s_save_notice = false;
    }
    portENTER_CRITICAL(&s_lock);
    int mic_request = s_mic_request;
    s_mic_request = -1;
    bool music_request = s_music_requested;
    s_music_requested = false;
    bool interaction_request = s_interaction_requested;
    s_interaction_requested = false;
    portEXIT_CRITICAL(&s_lock);
    if (mic_request == 1) (void)bsp_audio_mic_start();
    else if (mic_request == 0) bsp_audio_mic_stop();
    /* Recheck the latest setting after draining GUI changes. A cue queued
       just before disabling sound must not play after the switch turns off.
       Mute never powers the amplifier for a silent cue; failed volume writes
       must not play it at an older, possibly louder level. */
    if (interaction_request && s_pending.interaction_sound && s_pending.volume>0 &&
        s_applied_volume==s_pending.volume) {
        s_interaction_error=bsp_audio_click();
        if (s_interaction_error==ESP_OK) ++s_interaction_played;
    }
    /* Apply queued volume before starting playback, including a same-frame tap. */
    if (music_request) {
        /* Never play at an older loudness if the requested codec write failed.
           The driver de-duplicates an unchanged value, so this is cheap. */
        esp_err_t err=bsp_audio_set_volume(s_pending.volume);
        if (err==ESP_OK && xTaskCreate(speaker_tone_task,"spk_music",6144,NULL,3,NULL)!=pdPASS)
            err=ESP_ERR_NO_MEM;
        if (err!=ESP_OK) {
            ESP_LOGW("meoave", "speaker preview setup: %s", esp_err_to_name(err));
            bsp_audio_output_release();
            portENTER_CRITICAL(&s_lock);
            s_music_error=err;s_music_busy=false;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    if (s_dirty && esp_timer_get_time() >= s_save_at) {
        s_save_ok = save_settings();
        s_dirty = false; /* GUI lock contention must never repeat flash IO. */
        s_save_notice = true;
    }
    if (s_route_dirty && !s_dirty && !audio_selftest_busy() &&
        uxQueueMessagesWaiting(s_changes)==0 && esp_timer_get_time()>=s_route_save_at) {
        esp_err_t err=save_route(&s_last_route);
        s_route_dirty=false;
        if (err!=ESP_OK) {
            ESP_LOGW("meoave","microphone calibration save: %s",esp_err_to_name(err));
            s_save_ok=false;s_save_notice=true;
        }
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
        /* CDC admits whole newline-terminated records. The old 320-byte
           buffer silently lost its newline on long pages/counters, causing
           valid UI navigation to look like a timeout. Bound the full record
           and never submit a truncated status as if it were complete. */
        char line[512];
        int line_length=snprintf(line, sizeof(line), "MEO:UI page=%s frames=%lu rotation=180 brightness=%u "
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
        meo_wake_view_t wake_view;
        meo_wake_view(&wake_view);
        meo_ui_capture_view_t capture;
        meo_ui_capture_view(&capture);
        char capture_line[192];
        snprintf(capture_line, sizeof(capture_line),
                 "MEO:UI_CAPTURE visible=%d ack=%d wave_seq=%lu min_h=%u max_h=%u ink=%06lx\n",
                 capture.visible, capture.ack, (unsigned long)capture.wave_seq,
                 capture.min_height, capture.max_height, (unsigned long)capture.ink);
        char audio[320];
        snprintf(audio, sizeof(audio),
                 "MEO:UI_AUDIO volume=%u mic_gain=%u preview_busy=%d preview_error=%d preview_seq=%lu "
                 "mic_running=%d wake_armed=%d wake_detected=%d wake_detections=%lu wake_backend=%s cue_enabled=%d "
                 "cue_requests=%lu cue_played=%lu cue_error=%d\n",
                 m->settings.volume, m->settings.mic_gain,
                 s_last_audio_view.playback_busy, s_last_audio_view.playback_error,
                 (unsigned long)s_last_audio_view.playback_seq,
                 s_last_audio_view.mic_running,
                 wake_view.armed,
                 wake_view.detected,
                 (unsigned long)wake_view.detections,
                 wake_view.backend,
                 m->settings.interaction_sound,(unsigned long)s_interaction_requests,
                 (unsigned long)s_interaction_played,s_interaction_error);
        lvgl_port_unlock();
        if (line_length>0 && (size_t)line_length<sizeof(line))
            (void)bsp_usb_cdc_write(line);
        else
            (void)bsp_usb_cdc_write("MEO:UI_ERROR reason=STATUS_OVERFLOW\n");
        (void)bsp_usb_cdc_write(audio);
        (void)bsp_usb_cdc_write(capture_line);
        meoave_espsr_view_t sr;
        meoave_espsr_view(&sr);
        char sr_line[384];
        snprintf(sr_line, sizeof(sr_line), "MEO:WAKE_STATUS linked=%d loaded=%d keyword_ready=%d "
                 "requested=%d armed=%d blocked=%d model=%s keyword=%s error=%s queued=%lu drops=%lu chunks=%lu "
                 "acoustic=%lu text_tests=%lu source=%s guarded=%d crash_stage=%lu stack_free=%lu resets=%lu\n", sr.component_linked, sr.model_loaded,
                 sr.keyword_ready, sr.requested, sr.armed, sr.output_blocked, sr.model, MEO_WAKE_KEYWORD,
                 sr.error[0] ? sr.error : "NONE", (unsigned long)sr.queued_frames,
                 (unsigned long)sr.queue_drops, (unsigned long)sr.inferred_chunks,
                 (unsigned long)wake_view.acoustic_detections, (unsigned long)wake_view.text_test_detections,
                 wake_view.event_source[0] ? wake_view.event_source : "NONE", sr.crash_guarded,
                 (unsigned long)sr.crash_stage, (unsigned long)sr.worker_stack_free,
                 (unsigned long)sr.stream_resets);
        (void)bsp_usb_cdc_write(sr_line);
        report_ai_status();
    }
}
