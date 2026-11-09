/* Product adapter: the audited official pipeline still owns display and touch.
 * Only the UI creation call differs, not its init table or timing. */
#include "meoave_app.h"
#include "meoave_ui.h"
#include "fls2_display_diag.h"
#include "bsp_usb.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdio.h>

static QueueHandle_t s_changes;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready, s_report;
static bool s_storage_ok;
static meo_settings_t s_pending;
static int64_t s_save_at;
static bool s_dirty;
static bool s_save_notice, s_save_ok;

static void changed(const meo_settings_t *settings)
{
    /* Called under LVGL lock: no flash writes, DSI IO, logs or USB here. */
    (void)xQueueOverwrite(s_changes, settings);
}

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
        load_u8(handle, "reduced", &reduced);
        settings.reduced_motion = reduced != 0;
        load_u8(handle, "app_list", &list_view);
        settings.app_list_view = list_view != 0;
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
    meo_ui_config_t config = {.settings = load_settings(), .settings_changed = changed};
    meo_ui_create(&config);
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
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void meoave_app_service(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready, report = s_report;
    if (ready) s_report = false;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) return;
    meo_settings_t latest;
    if (xQueueReceive(s_changes, &latest, 0) == pdTRUE) {
        s_pending = latest;
        esp_err_t err = fls2_display_set_brightness(latest.brightness);
        if (err != ESP_OK) ESP_LOGW("meoave", "brightness: %s", esp_err_to_name(err));
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
        char line[240];
        snprintf(line, sizeof(line), "MEO:UI page=%s frames=%lu rotation=180 brightness=%u "
                 "reduced=%d running=%d mode=%s elapsed_ms=%llu wifi=UNAVAILABLE radar=%d app_view=%s eye=%u eye_auto=%d\n",
                 meo_ui_page_name(m->page), (unsigned long)meo_ui_frames(), m->settings.brightness,
                 m->settings.reduced_motion, m->running, m->mode == MEO_GUIDED ? "GUIDED" : "RADAR",
                 (unsigned long long)m->elapsed_ms, m->respiration_live,m->settings.app_list_view ? "LIST" : "GRID",
                 (unsigned)m->expression, !m->expression_override);
        lvgl_port_unlock();
        (void)bsp_usb_cdc_write(line);
    }
}
