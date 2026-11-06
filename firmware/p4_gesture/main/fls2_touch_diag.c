/* Diagnostic-only link wrappers. Do not modify official CST820 or demo sources.
 * Passive counters observe normal LVGL reads; explicit TOUCH ON probes use the
 * same IO and a mutex, without a second I2C bus or address scanning. Probe reads
 * are counted separately and do not prove that LVGL is processing input.
 */
#include <stdio.h>
#include <string.h>

#include "fls2_touch_diag.h"
#include "bsp_usb.h"
#include "esp_lcd_touch_cst820.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    uint32_t driver_ok, driver_errors, probe_ok, probe_errors;
    uint32_t frames, pressed_frames, press_edges, invalid_frames;
    uint32_t id_ok, id_errors;
    int64_t last_ok_us, last_frame_us, last_driver_ok_us;
    uint16_t x, y;
    uint8_t id, points, raw[15];
    esp_err_t last_error, init_error;
    bool initialized;
} touch_stats_t;

static touch_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_io_lock;
static esp_lcd_panel_io_handle_t s_io;
static bool s_probe_enabled;
static int64_t s_next_probe_us, s_next_id_us;

esp_err_t __real_esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io,
                                          int cmd, void *data, size_t size);
esp_err_t __real_esp_lcd_touch_new_i2c_cst820(esp_lcd_panel_io_handle_t io,
                                            const esp_lcd_touch_config_t *cfg,
                                            esp_lcd_touch_handle_t *tp);

void fls2_touch_diag_init(void)
{
    s_io_lock = xSemaphoreCreateMutex();
    configASSERT(s_io_lock);
}

static void record_read(int cmd, const void *data, size_t size, esp_err_t err, bool probe)
{
    const uint8_t *b = data;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_stats_lock);
    if (err == ESP_OK) {
        if (probe) {
            s_stats.probe_ok++;
        } else {
            s_stats.driver_ok++;
            s_stats.last_driver_ok_us = now;
        }
        s_stats.last_ok_us = now;
    } else {
        if (probe) s_stats.probe_errors++; else s_stats.driver_errors++;
    }
    s_stats.last_error = err;
    if (cmd == 0xA7 && size == 1) {
        if (err == ESP_OK) {
            s_stats.id = b[0];
            s_stats.id_ok++;
        } else {
            s_stats.id_errors++;
        }
    }
    if (cmd == 0x00 && size == sizeof(s_stats.raw) && err == ESP_OK) {
        bool was_pressed = s_stats.points != 0;
        memcpy(s_stats.raw, b, sizeof(s_stats.raw));
        s_stats.points = b[2];
        s_stats.x = ((b[3] & 0x0f) << 8) | b[4];
        s_stats.y = ((b[5] & 0x0f) << 8) | b[6];
        s_stats.frames++;
        s_stats.last_frame_us = now;
        if (s_stats.points) {
            s_stats.pressed_frames++;
            if (!was_pressed) s_stats.press_edges++;
        }
        if (s_stats.points > 1 || (s_stats.points && (s_stats.x >= 460 || s_stats.y >= 460))) {
            s_stats.invalid_frames++;
        }
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

esp_err_t __wrap_esp_lcd_touch_new_i2c_cst820(esp_lcd_panel_io_handle_t io,
                                            const esp_lcd_touch_config_t *cfg,
                                            esp_lcd_touch_handle_t *tp)
{
    /* Install before the real constructor so its chip-ID read is observed. */
    portENTER_CRITICAL(&s_stats_lock);
    s_io = io;
    portEXIT_CRITICAL(&s_stats_lock);
    esp_err_t err = __real_esp_lcd_touch_new_i2c_cst820(io, cfg, tp);
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.init_error = err;
    s_stats.initialized = err == ESP_OK;
    portEXIT_CRITICAL(&s_stats_lock);
    return err;
}

esp_err_t __wrap_esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io,
                                          int cmd, void *data, size_t size)
{
    portENTER_CRITICAL(&s_stats_lock);
    bool touch = io == s_io;
    portEXIT_CRITICAL(&s_stats_lock);
    if (!touch) return __real_esp_lcd_panel_io_rx_param(io, cmd, data, size);
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        record_read(cmd, data, size, ESP_ERR_TIMEOUT, false);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = __real_esp_lcd_panel_io_rx_param(io, cmd, data, size);
    record_read(cmd, data, size, err, false);
    xSemaphoreGive(s_io_lock);
    return err;
}

void fls2_touch_diag_enable(bool enabled)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_probe_enabled = enabled;
    portEXIT_CRITICAL(&s_stats_lock);
}

void fls2_touch_diag_service(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    bool enabled = s_probe_enabled && s_stats.initialized;
    esp_lcd_panel_io_handle_t io = s_io;
    portEXIT_CRITICAL(&s_stats_lock);
    int64_t now = esp_timer_get_time();
    if (!enabled || now < s_next_probe_us) return;
    s_next_probe_us = now + 100000;
    /* Never wait on an LVGL lock. Serialize only touch IO transactions. */
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        record_read(0, NULL, 0, ESP_ERR_TIMEOUT, true);
        return;
    }
    uint8_t raw[15] = {0};
    esp_err_t err = __real_esp_lcd_panel_io_rx_param(io, 0x00, raw, sizeof(raw));
    record_read(0x00, raw, sizeof(raw), err, true);
    if (now >= s_next_id_us) {
        s_next_id_us = now + 1000000;
        uint8_t id = 0;
        err = __real_esp_lcd_panel_io_rx_param(io, 0xA7, &id, 1);
        record_read(0xA7, &id, 1, err, true);
    }
    xSemaphoreGive(s_io_lock);
}

static void write_line(const char *line)
{
    (void)bsp_usb_cdc_write(line);
    /* Called only by app_main, not a USB/LVGL callback. Let the 512-byte CDC
       TX queue drain before the next line instead of silently losing data. */
    vTaskDelay(pdMS_TO_TICKS(10));
}

void fls2_touch_diag_report_compact(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    touch_stats_t st = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    char line[256];
    snprintf(line, sizeof(line),
             "FLS2:TOUCH_LIVE n=%u x=%u y=%u id=0x%02X driver_ok=%lu driver_err=%lu "
             "pressed=%lu edges=%lu invalid=%lu age_ms=%lld\n",
             st.points, st.x, st.y, st.id,
             (unsigned long)st.driver_ok, (unsigned long)st.driver_errors,
             (unsigned long)st.pressed_frames, (unsigned long)st.press_edges,
             (unsigned long)st.invalid_frames,
             st.last_frame_us ? (long long)((esp_timer_get_time() - st.last_frame_us) / 1000) : -1LL);
    (void)bsp_usb_cdc_write(line);
}

void fls2_touch_diag_report(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    touch_stats_t st = s_stats;
    bool enabled = s_probe_enabled;
    portEXIT_CRITICAL(&s_stats_lock);
    int64_t now = esp_timer_get_time();
    char line[240];
    snprintf(line, sizeof(line),
             "FLS2:TOUCH init=%d addr=0x15 sda=8 scl=9 hz=400000 probe=%d "
             "id=0x%02X id_ok=%lu id_err=%lu\n",
             st.initialized, enabled, st.id,
             (unsigned long)st.id_ok, (unsigned long)st.id_errors);
    write_line(line);
    snprintf(line, sizeof(line),
             "FLS2:TOUCH_IO driver_ok=%lu driver_err=%lu probe_ok=%lu probe_err=%lu "
             "last=%s age_ms=%lld driver_age_ms=%lld\n",
             (unsigned long)st.driver_ok, (unsigned long)st.driver_errors,
             (unsigned long)st.probe_ok, (unsigned long)st.probe_errors,
             esp_err_to_name(st.last_error),
             st.last_ok_us ? (long long)((now - st.last_ok_us) / 1000) : -1LL,
             st.last_driver_ok_us ? (long long)((now - st.last_driver_ok_us) / 1000) : -1LL);
    write_line(line);
    snprintf(line, sizeof(line),
             "FLS2:TOUCH_XY n=%u x=%u y=%u frames=%lu pressed=%lu edges=%lu "
             "invalid=%lu age_ms=%lld\n",
             st.points, st.x, st.y, (unsigned long)st.frames,
             (unsigned long)st.pressed_frames, (unsigned long)st.press_edges,
             (unsigned long)st.invalid_frames,
             st.last_frame_us ? (long long)((now - st.last_frame_us) / 1000) : -1LL);
    write_line(line);
    /* Keep each USB write short; the existing CDC queue is bounded. */
    char *out = line;
    out += snprintf(out, sizeof(line), "FLS2:TOUCH_RAW");
    for (size_t i = 0; i < sizeof(st.raw); i++) out += sprintf(out, " %02X", st.raw[i]);
    sprintf(out, "\n");
    write_line(line);
}
