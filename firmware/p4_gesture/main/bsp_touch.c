#include "bsp_touch.h"

#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst820.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "bsp_common.h"

static const char *TAG = "BSP_TP";
static i2c_master_bus_handle_t s_bus;
static esp_lcd_touch_handle_t s_tp;
static bool s_present;
static bool s_lvgl_owner;
static char s_last_scan[160];

/* 短超时全地址扫描；0x08..0x77 共 112 个地址，5ms 超时上限约 560ms */
esp_err_t bsp_touch_rescan(char *out, size_t out_len, bool *cst820_ack)
{
    if (!s_bus) {
        if (out && out_len) snprintf(out, out_len, "BUS_NOT_READY");
        return ESP_ERR_INVALID_STATE;
    }
    char *p = s_last_scan;
    size_t left = sizeof(s_last_scan);
    unsigned found = 0;
    bool ack15 = false;

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 5) == ESP_OK) {
            if (addr == TP_I2C_ADDR) ack15 = true;
            int n = snprintf(p, left, "%s0x%02X", found ? " " : "", addr);
            if (n < 0 || (size_t)n >= left) break;
            p += n; left -= (size_t)n;
            found++;
            ESP_LOGI(TAG, "  I2C ACK 0x%02X", addr);
        }
    }
    if (!found) snprintf(s_last_scan, sizeof(s_last_scan), "NONE");
    if (cst820_ack) *cst820_ack = ack15;
    if (out && out_len) snprintf(out, out_len, "%s", s_last_scan);
    ESP_LOGI(TAG, "touch scan: %u device(s) [%s], CST820@0x%02X=%s",
             found, s_last_scan, TP_I2C_ADDR, ack15 ? "ACK" : "NO_ACK");
    return ESP_OK;
}

const char *bsp_touch_last_scan(void) { return s_last_scan; }

esp_err_t bsp_touch_init(void)
{
    /* 硬件复位：CST820 要求释放后 >=100ms 才响应 */
    bsp_gpio_out(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    bsp_gpio_out(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    bsp_gpio_in(PIN_TP_INT, true);

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TP_I2C_PORT,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    bool ack = false;
    bsp_touch_rescan(NULL, 0, &ack);
    if (!ack) {
        ESP_LOGW(TAG, "no touch controller; skip driver (this panel variant may have no CTP)");
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    esp_err_t err = esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &tp_io);
    if (err != ESP_OK) return err;

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,   /* 已在上面手动复位 */
        .int_gpio_num = GPIO_NUM_NC,   /* 轮询方式，不依赖中断 */
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    err = esp_lcd_touch_new_i2c_cst820(tp_io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST820 driver init failed: %s (I2C ACK ok, check ID regs)", esp_err_to_name(err));
        return err;
    }
    s_present = true;
    return ESP_OK;
}

void bsp_touch_poll(void)
{
    if (!s_present || s_lvgl_owner) return;
    esp_lcd_touch_point_data_t points[5] = {0};
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_data(s_tp, points, &cnt, 5) == ESP_OK && cnt > 0) {
        for (uint8_t i = 0; i < cnt; i++) {
            ESP_LOGI(TAG, "touch[%u] x=%u y=%u strength=%u", i,
                     points[i].x, points[i].y, points[i].strength);
        }
    }
}

bool bsp_touch_present(void) { return s_present; }
esp_lcd_touch_handle_t bsp_touch_handle(void) { return s_present ? s_tp : NULL; }
void bsp_touch_set_lvgl_owner(bool owned) { s_lvgl_owner = owned; }
