#pragma once
#include "meoave_model.h"
#include "lvgl.h"

typedef struct {
    meo_settings_t settings;
    /* Short, nonblocking callbacks. The embedded adapter queues IO/NVS work. */
    void (*settings_changed)(const meo_settings_t *settings);
} meo_ui_config_t;

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
const meo_model_t *meo_ui_model(void);
uint32_t meo_ui_frames(void);
const char *meo_ui_page_name(meo_page_t page);
