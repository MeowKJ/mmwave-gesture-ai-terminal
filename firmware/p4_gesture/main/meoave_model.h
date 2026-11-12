#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Pure, platform-independent state. All timestamps are monotonic milliseconds.
 * Calls must be serialized by the UI thread (or its platform LVGL lock). */
typedef enum { MEO_HOME, MEO_APPS, MEO_SETTINGS, MEO_WIFI, MEO_BREATHE,
               MEO_DISPLAY, MEO_ACCESSIBILITY, MEO_PASSWORD, MEO_MIC, MEO_SPEAKER,
               MEO_PAGE_COUNT } meo_page_t;
typedef enum { MEO_GUIDED, MEO_RADAR } meo_breath_mode_t;
typedef enum { MEO_EYE_CALM, MEO_EYE_HAPPY, MEO_EYE_CURIOUS, MEO_EYE_SLEEPY,
               MEO_EYE_SURPRISED, MEO_EYE_WINK, MEO_EYE_SAD, MEO_EYE_COUNT } meo_expression_t;

/* A single rounded white stroke per eye, not an eyeball/pupil hierarchy.
 * Normalized path t=-1..1: x=half_x*t, y=half_y*t+arch*(t*t-.5)+y. */
typedef struct { float half_x, half_y, arch, width, y; } meo_eye_shape_t;
typedef struct {
    uint8_t brightness, guide_bpm;
    /* Speaker output level 0..100 (0 = mute, 100 = 0 dB). Persisted so the
       next boot restores the same loudness. */
    uint8_t volume;
    bool reduced_motion, app_list_view;
    /* Microphone test switch: capture only runs while this is on, so the radio
       and the meter stay idle (and silent) until someone asks for them. */
    bool mic_test;
} meo_settings_t;
typedef struct {
    float x, y;
    uint64_t at_ms;
    bool valid;
} meo_gaze_t;
typedef struct {
    float waveform, rate_bpm;
    uint64_t at_ms;
    bool valid, rate_valid;
} meo_resp_t;
typedef struct {
    meo_page_t page;
    meo_breath_mode_t mode;
    meo_settings_t settings;
    meo_gaze_t gaze;
    meo_resp_t respiration;
    uint64_t now_ms, last_ms, elapsed_ms, next_blink_ms, blink_start_ms;
    float eye_x, eye_y, openness, breath_level, guide_phase;
    /* 60000 units per cycle; advances by guide_bpm per active millisecond. */
    uint32_t guide_phase_units;
    uint32_t rng;
    meo_expression_t expression;
    meo_eye_shape_t eye_shape[2];
    uint64_t expression_clock_ms, expression_at_ms;
    uint32_t expression_hold_ms;
    bool expression_override;
    bool running, complete, gaze_live, respiration_live, rate_live, inhaling;
} meo_model_t;

meo_settings_t meo_default_settings(void);
void meo_validate_settings(meo_settings_t *settings);
void meo_model_init(meo_model_t *m, uint64_t now, const meo_settings_t *settings);
void meo_model_tick(meo_model_t *m, uint64_t now);
void meo_model_gaze(meo_model_t *m, meo_gaze_t sample);
void meo_model_respiration(meo_model_t *m, meo_resp_t sample);
void meo_model_toggle_session(meo_model_t *m);
void meo_model_set_mode(meo_model_t *m, meo_breath_mode_t mode);
void meo_model_set_rate(meo_model_t *m, int rate);
/* UI thread/LVGL lock, like gaze. hold_ms=0 holds until explicitly released.
 * Invalid expressions are ignored. Timed holds expire even off the home page. */
void meo_model_set_expression(meo_model_t *m, meo_expression_t expression, uint32_t hold_ms);
void meo_model_auto_expression(meo_model_t *m);
meo_page_t meo_page_parent(meo_page_t page);
void meo_rotate_touch_180(int raw_x, int raw_y, int *x, int *y);
