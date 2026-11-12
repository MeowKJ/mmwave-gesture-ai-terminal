#pragma once
#include "meoave_model.h"
#include "lvgl.h"

typedef struct {
    meo_settings_t settings;
    /* Short, nonblocking callbacks. The embedded adapter queues IO/NVS work. */
    void (*settings_changed)(const meo_settings_t *settings);
    /* Wi-Fi page actions. Both must return immediately: they only queue a
       request for the radio task, they never scan or connect inline. */
    bool (*wifi_scan)(void);
    bool (*wifi_join)(const char *ssid);
    /* Store the typed password and start connecting; false when the radio
       cannot accept it yet (the page keeps the text so nothing is lost). */
    bool (*wifi_connect)(const char *ssid, const char *pass);
    /* Start/stop microphone capture; the settings switch is the only owner. */
    void (*mic_test)(bool on);
    /* Play one short test tone at the current speaker volume. Must return
       immediately (the adapter queues it); the UI never blocks on audio. */
    void (*speaker_test)(void);
    /* Run the physical acoustic loopback self test. Non-blocking. */
    void (*audio_loop)(void);
} meo_ui_config_t;

/* Audio snapshot for the speaker page. The adapter pushes it only when a
 * field changes, so this costs no LVGL work during steady state. */
typedef struct {
    int      volume;
    int      mclk_duty;
    int      loop_state;      /* 0 idle, 1 running, 2 pass, 3 fail */
    int      loop_score;
    uint8_t  route_l, route_r;
    bool     busy;
    bool     mic_running;
} meo_audio_view_t;

/* Wi-Fi summary the home indicator and the network page can show without
 * owning any network state themselves. */
typedef enum { MEO_WIFI_OFF = 0, MEO_WIFI_CONNECTING, MEO_WIFI_ONLINE, MEO_WIFI_ERROR } meo_wifi_state_t;

/* Call with the LVGL lock held; the existing port rotates pixels and LVGL
 * rotates raw pointer coordinates. Never additionally mirror CST820 input. */
void meo_ui_create(const meo_ui_config_t *config);
void meo_ui_navigate(meo_page_t page);
void meo_ui_feed_gaze(meo_gaze_t sample);
void meo_ui_feed_respiration(meo_resp_t sample);
/* Compatibility hook: the product page accepts MEO_RADAR only. Feed normalized
 * waveform [-1,1], a real rate and monotonic timestamp through the API above;
 * missing/stale/invalid samples show a static flower and no rate. */
void meo_ui_set_breath_mode(meo_breath_mode_t mode);
/* Input timestamps may be raw 32-bit LVGL ticks or the model's full 64-bit
 * monotonic time; the UI boundary normalizes raw ticks across wrap. */
void meo_ui_set_expression(meo_expression_t expression, uint32_t hold_ms);
void meo_ui_auto_expression(void);
void meo_ui_settings_saved(bool success);
/* Status inputs. Call with the LVGL lock held; they only touch cached labels
 * and never read hardware, block or allocate. percent < 0 hides the gauge. */
void meo_ui_set_power(int percent, bool charging);
void meo_ui_set_wifi(meo_wifi_state_t state, const char *ssid, int rssi);
/* Scannable networks for the network page; count is clamped to 3 rows. */
void meo_ui_set_scan_list(const char ssid[][33], const int rssi[], unsigned count);
/* Open the on-screen password page for a network that has no saved key. */
void meo_ui_password_open(const char *ssid);
/* True when every code point of this UTF-8 string exists in the shipped font.
 * Used by the USB health line so a stale font shows up as a number, not a bug
 * report about "a weird character". */
bool meo_ui_font_has(const char *utf8);
/* 0..5 shows the shutdown countdown, negative hides it. 0 means "powering off". */
void meo_ui_set_shutdown(int seconds_left);
/* Per-channel microphone level in 0..1 (0 = silence), [0] = left, [1] = right.
 * Driven from the app task; the page only animates the bars, never I2S. */
void meo_ui_set_mic(const float level[2], bool running);
/* Push a fresh audio snapshot; called with the LVGL lock held. */
void meo_ui_set_audio(const meo_audio_view_t *view);
/* Frame pacing probe. A window starts automatically on every page change and
 * can be started on demand for any other animation. The GUI task timestamps
 * each completed LVGL refresh, so this measures real delivered frames rather
 * than the nominal 15 ms refresh timer. fps_x10 is computed over the span in
 * which frames were actually produced (static pages legitimately render 0
 * frames), so an idle tail after an animation cannot dilute the number. */
typedef struct {
    uint32_t done;             /* 1 once a window has been published */
    uint32_t seq;              /* increments on every published window */
    uint32_t frames;           /* completed refreshes inside the motion span */
    uint32_t fps_x10;          /* frames/second over that span, x10 */
    uint32_t span_ms;
    uint32_t max_frame_ms;     /* worst inter-frame gap */
    uint32_t jank;             /* refreshes slower than 33.3 ms */
    uint32_t slow_render;      /* those caused by a long rasterisation */
    uint32_t slow_gap;         /* those caused by the GUI task being stalled */
    uint32_t max_render_us;
    uint32_t max_flush_us;
    char     tag[16];
} meo_ui_fps_t;
/* window_ms is clamped to 50..10000. Safe to call with the LVGL lock held. */
void meo_ui_fps_start(uint32_t window_ms, const char *tag);
void meo_ui_fps_read(meo_ui_fps_t *out);
/* Number of pages the boot warm-up has rendered (0..MEO_PAGE_COUNT). */
int meo_ui_warmup_count(void);
/* True while the backlight is intentionally off for the boot warm-up. */
bool meo_ui_warmup_active(void);

/* Cost probe, updated from the GUI task. period_ms is the real interval the
 * LVGL timer achieved; render_us/flush_us are the average rasterisation and
 * panel-transfer time per refresh. */
typedef struct {
    uint32_t period_ms;
    uint32_t busy_us;
    uint32_t render_us;
    uint32_t flush_us;
    uint32_t refreshes;
    uint32_t px_per_s;      /* panel pixels actually transferred per second */
    uint32_t flushes;
    uint32_t max_px;        /* largest single invalidated area in the window */
    int32_t  max_area[4];   /* its x1,y1,x2,y2 */
} meo_ui_perf_t;
void meo_ui_perf(meo_ui_perf_t *out);
const meo_model_t *meo_ui_model(void);
uint32_t meo_ui_frames(void);
const char *meo_ui_page_name(meo_page_t page);
