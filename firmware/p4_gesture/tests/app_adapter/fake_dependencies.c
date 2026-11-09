#include "fake_dependencies.h"
#include "meoave_app.h"
#include "fls2_display_diag.h"
#include "bsp_usb.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fake_state_t fake;
struct fake_queue { bool full; meo_settings_t item; };
static struct fake_queue queue;
static bool queue_created, handle_open;
static nvs_open_mode_t handle_mode;
static fake_image_t staged;
static const char *const keys[KEY_COUNT] = {
    "brightness", "guide_bpm", "reduced", "app_list"
};

void fake_check(bool condition, const char *expr, const char *file, int line)
{
    if (!condition) {
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
        exit(EXIT_FAILURE);
    }
}

void fake_init(void)
{
    memset(&fake, 0, sizeof(fake));
    memset(&queue, 0, sizeof(queue));
    queue_created = handle_open = false;
}

void fake_clear_observations(void)
{
    CHECK(!fake.gui_locked && fake.critical_depth == 0 && !handle_open);
    fake.init_calls = fake.erase_calls = fake.read_open_calls = fake.read_calls = 0;
    fake.write_open_calls = fake.set_calls = fake.commit_calls = fake.close_calls = 0;
    fake.lock_calls = fake.unlock_calls = fake.brightness_calls = fake.usb_calls = 0;
    fake.notice_count = fake.log_calls = fake.overwrite_calls = fake.receive_calls = 0;
    memset(fake.read_key_calls, 0, sizeof(fake.read_key_calls));
    memset(fake.trace, 0, sizeof(fake.trace));
    memset(fake.usb_text, 0, sizeof(fake.usb_text));
    memset(fake.set_keys, 0, sizeof(fake.set_keys));
    memset(fake.set_values, 0, sizeof(fake.set_values));
    memset(fake.notices, 0, sizeof(fake.notices));
    memset(fake.notice_settings, 0, sizeof(fake.notice_settings));
}

void fake_seed(unsigned key, uint8_t value)
{
    CHECK(key < KEY_COUNT);
    fake.durable.values[key] = value;
    fake.durable.present[key] = true;
}

void fake_boot(void)
{
    CHECK(fake.create_calls == 0);
    fake.gui_locked = true; /* Official caller's documented LVGL lock. */
    meoave_app_create();
    fake.gui_locked = false;
    CHECK(fake.create_calls == 1 && fake.ui_config.settings_changed != NULL);
}

void fake_ui_change(meo_settings_t settings)
{
    CHECK(fake.ui_config.settings_changed != NULL && fake.critical_depth == 0);
    bool was_locked = fake.gui_locked;
    fake.gui_locked = true;
    fake.ui_model.settings = settings;
    fake.ui_config.settings_changed(&fake.ui_model.settings);
    fake.gui_locked = was_locked;
}

void fake_service_at(int64_t now_us)
{
    CHECK(now_us >= fake.now_us && !fake.gui_locked);
    fake.now_us = now_us;
    meoave_app_service();
    CHECK(!fake.gui_locked && fake.critical_depth == 0 && !handle_open);
    CHECK(fake.erase_calls == 0);
}

static void trace(char c)
{
    size_t n = strlen(fake.trace);
    CHECK(n + 1 < sizeof(fake.trace));
    fake.trace[n] = c;
    fake.trace[n + 1] = '\0';
}

static void background_io(void)
{
    CHECK(!fake.gui_locked && fake.critical_depth == 0);
}

static void inject(int hook)
{
    if (fake.hook == hook) {
        fake.hook = HOOK_NONE; /* One deterministic UI/task interleaving. */
        fake_ui_change(fake.hook_settings);
    }
}

static int key_index(const char *key)
{
    for (int i = 0; i < KEY_COUNT; ++i) {
        if (strcmp(key, keys[i]) == 0) return i;
    }
    CHECK(false);
    return 0;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    CHECK(!queue_created && length == 1 && item_size == sizeof(meo_settings_t));
    queue_created = true;
    return &queue;
}

BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item)
{
    CHECK(q == &queue && queue_created && item != NULL && fake.gui_locked);
    ++fake.overwrite_calls;
    memcpy(&queue.item, item, sizeof(queue.item));
    queue.full = true;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    CHECK(q == &queue && queue_created && wait == 0);
    background_io();
    ++fake.receive_calls;
    if (!queue.full) return pdFALSE;
    memcpy(item, &queue.item, sizeof(queue.item));
    queue.full = false;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    CHECK(q == &queue && fake.gui_locked);
    return queue.full ? 1U : 0U;
}

void fake_enter_critical(portMUX_TYPE *lock)
{
    CHECK(lock != NULL && fake.critical_depth == 0);
    ++fake.critical_depth;
}

void fake_exit_critical(portMUX_TYPE *lock)
{
    CHECK(lock != NULL && fake.critical_depth == 1);
    --fake.critical_depth;
}

int64_t esp_timer_get_time(void) { return fake.now_us; }

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "FAKE_ESP_ERROR";
}

void fake_error_check(esp_err_t err, const char *file, int line)
{
    fake_check(err == ESP_OK, "ESP_ERROR_CHECK", file, line);
}

void fake_log(const char *level, const char *tag, const char *format, ...)
{
    CHECK(level != NULL && tag != NULL && format != NULL);
    ++fake.log_calls;
}

esp_err_t nvs_flash_init(void)
{
    ++fake.init_calls;
    return fake.init_error;
}

esp_err_t nvs_flash_erase(void)
{
    ++fake.erase_calls;
    CHECK(false); /* Fail immediately, even before any service call. */
    return ESP_FAIL;
}

esp_err_t nvs_flash_erase_partition(const char *partition_label)
{
    (void)partition_label;
    return nvs_flash_erase();
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    (void)handle;
    return nvs_flash_erase();
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    (void)key;
    return nvs_flash_erase();
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    CHECK(strcmp(name, "meoave") == 0 && handle != NULL && !handle_open);
    esp_err_t error;
    if (mode == NVS_READONLY) {
        ++fake.read_open_calls;
        error = fake.read_open_error;
    } else {
        CHECK(mode == NVS_READWRITE);
        background_io();
        ++fake.write_open_calls;
        trace('O');
        error = fake.write_open_error;
    }
    if (error != ESP_OK) return error;
    handle_open = true;
    handle_mode = mode;
    staged = fake.durable;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    CHECK(handle == 1 && handle_open && handle_mode == NVS_READONLY && value);
    int i = key_index(key);
    ++fake.read_calls;
    ++fake.read_key_calls[i];
    esp_err_t error = fake.read_error_codes[i];
    if (error == ESP_OK && (fake.read_error_mask & (1U << i))) error = ESP_FAIL;
    if (error != ESP_OK) {
        /* Deliberately taint the output on error to test load_u8's isolation. */
        static const uint8_t error_outputs[KEY_COUNT] = {55, 8, 1, 1};
        if (fake.poison_read_error_output) *value = error_outputs[i];
        return error;
    }
    if (!fake.durable.present[i]) return ESP_ERR_NVS_NOT_FOUND;
    *value = fake.durable.values[i];
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    background_io();
    CHECK(handle == 1 && handle_open && handle_mode == NVS_READWRITE);
    CHECK(fake.set_calls < KEY_COUNT);
    int i = key_index(key);
    fake.set_keys[fake.set_calls] = i;
    fake.set_values[fake.set_calls] = value;
    ++fake.set_calls;
    trace('S');
    inject(HOOK_SET);
    if ((int)fake.set_calls == fake.fail_set_at) return ESP_FAIL;
    staged.values[i] = value;
    staged.present[i] = true;
    return ESP_OK;
}

static void write_image(void)
{
    if (!fake.image_path) return;
    FILE *file = fopen(fake.image_path, "wb");
    CHECK(file != NULL);
    /* Explicit bytes; no structure padding, pointers or production state. */
    CHECK(fwrite("MEO1", 1, 4, file) == 4);
    for (int i = 0; i < KEY_COUNT; ++i) {
        CHECK(fputc(fake.durable.present[i] ? 1 : 0, file) != EOF);
        CHECK(fputc(fake.durable.values[i], file) != EOF);
    }
    CHECK(fclose(file) == 0);
}

void fake_read_image(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    char magic[4];
    CHECK(fread(magic, 1, sizeof(magic), file) == sizeof(magic));
    CHECK(memcmp(magic, "MEO1", 4) == 0);
    for (int i = 0; i < KEY_COUNT; ++i) {
        int present = fgetc(file), value = fgetc(file);
        CHECK((present == 0 || present == 1) && value != EOF);
        fake.durable.present[i] = present != 0;
        fake.durable.values[i] = (uint8_t)value;
    }
    CHECK(fgetc(file) == EOF && !ferror(file));
    CHECK(fclose(file) == 0);
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    background_io();
    CHECK(handle == 1 && handle_open && handle_mode == NVS_READWRITE);
    ++fake.commit_calls;
    trace('C');
    inject(HOOK_COMMIT);
    if (fake.commit_error != ESP_OK) return fake.commit_error;
    fake.durable = staged;
    write_image();
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    CHECK(handle == 1 && handle_open);
    if (handle_mode == NVS_READWRITE) {
        background_io();
        trace('X');
    }
    ++fake.close_calls;
    handle_open = false;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    CHECK(timeout_ms == 5 && !fake.gui_locked && fake.critical_depth == 0);
    ++fake.lock_calls;
    inject(HOOK_LOCK);
    if (fake.lock_failures) {
        --fake.lock_failures;
        return false;
    }
    fake.gui_locked = true;
    return true;
}

void lvgl_port_unlock(void)
{
    CHECK(fake.gui_locked);
    ++fake.unlock_calls;
    fake.gui_locked = false;
}

esp_err_t fls2_display_set_brightness(uint8_t percent)
{
    background_io();
    ++fake.brightness_calls;
    fake.last_brightness = percent;
    return fake.brightness_error;
}

void meo_ui_create(const meo_ui_config_t *config)
{
    CHECK(fake.gui_locked && config != NULL);
    ++fake.create_calls;
    fake.ui_config = *config;
    meo_model_init(&fake.ui_model, 0, &config->settings);
}

void meo_ui_settings_saved(bool success)
{
    CHECK(fake.gui_locked && fake.notice_count < 16);
    fake.notices[fake.notice_count] = success;
    fake.notice_settings[fake.notice_count] = fake.ui_model.settings;
    ++fake.notice_count;
}

const meo_model_t *meo_ui_model(void)
{
    CHECK(fake.gui_locked);
    return &fake.ui_model;
}

uint32_t meo_ui_frames(void)
{
    CHECK(fake.gui_locked);
    return 123;
}

const char *meo_ui_page_name(meo_page_t page)
{
    CHECK(fake.gui_locked && page == MEO_HOME);
    return "HOME";
}

esp_err_t bsp_usb_cdc_write(const char *text)
{
    background_io(); /* Captured in memory: never opens a serial device. */
    CHECK(text != NULL && strlen(text) < sizeof(fake.usb_text));
    ++fake.usb_calls;
    strcpy(fake.usb_text, text);
    if (fake.request_status_on_usb) {
        fake.request_status_on_usb = false;
        meoave_app_request_status();
    }
    return ESP_OK;
}
