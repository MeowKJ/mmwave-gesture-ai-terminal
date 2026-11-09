#pragma once
#include <stddef.h>
#include "meoave_ui.h"
#include "esp_err.h"

enum { KEY_BRIGHTNESS, KEY_BPM, KEY_REDUCED, KEY_APP_LIST, KEY_COUNT };
enum { HOOK_NONE, HOOK_SET, HOOK_COMMIT, HOOK_LOCK };
typedef struct {
    uint8_t values[KEY_COUNT];
    bool present[KEY_COUNT];
} fake_image_t;
typedef struct {
    int64_t now_us;
    fake_image_t durable;
    esp_err_t init_error, read_open_error, write_open_error, commit_error;
    esp_err_t brightness_error;
    unsigned read_error_mask;
    esp_err_t read_error_codes[KEY_COUNT];
    bool poison_read_error_output;
    int fail_set_at;
    unsigned lock_failures;
    int hook;
    meo_settings_t hook_settings;
    bool request_status_on_usb;
    const char *image_path;
    bool gui_locked;
    unsigned critical_depth;
    unsigned init_calls, erase_calls, read_open_calls, read_calls;
    unsigned read_key_calls[KEY_COUNT];
    unsigned write_open_calls, set_calls, commit_calls, close_calls;
    unsigned lock_calls, unlock_calls, brightness_calls, usb_calls, log_calls;
    unsigned notice_count, create_calls, overwrite_calls, receive_calls;
    uint8_t last_brightness;
    char trace[64], usb_text[512];
    int set_keys[KEY_COUNT];
    uint8_t set_values[KEY_COUNT];
    bool notices[16];
    meo_settings_t notice_settings[16];
    meo_ui_config_t ui_config;
    meo_model_t ui_model;
} fake_state_t;
extern fake_state_t fake;

/* These assertions stay enabled in Release builds. */
void fake_check(bool condition, const char *expr, const char *file, int line);
#define CHECK(expr) fake_check(!!(expr), #expr, __FILE__, __LINE__)
void fake_init(void);
void fake_clear_observations(void);
void fake_seed(unsigned key, uint8_t value);
void fake_boot(void);
void fake_ui_change(meo_settings_t settings);
void fake_service_at(int64_t now_us);
void fake_read_image(const char *path);
