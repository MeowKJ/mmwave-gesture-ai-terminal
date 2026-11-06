#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_touch.h"

/* 按 osptek 官方 esp32p4-idf6-co5300 例程初始化显示，适配本板 VCI_EN / OLED_RST */
esp_err_t bsp_display_init(void);

/* 点屏自检：纯色轮播 + 亮度阶梯（在 LVGL 接管 framebuffer 之前调用） */
void bsp_display_self_test(void);

/* 启动 LVGL（官方 esp_lvgl_port + DSI display）并建立状态 UI */
esp_err_t bsp_display_lvgl_start(void);
bool bsp_display_lvgl_ready(void);

/* 把 esp_lcd_touch 注册成 LVGL 指针输入设备（触摸测试的关键一步）。
   注册成功后 LVGL 自己按 30ms 周期读触摸，不要再手动 poll。 */
esp_err_t bsp_display_lvgl_add_touch(esp_lcd_touch_handle_t tp);

/* 在屏幕状态栏显示一行文字（LVGL 启动后可用） */
void bsp_display_set_status(const char *text);

uint32_t bsp_display_vsync_count(void);
esp_err_t bsp_display_set_brightness(uint8_t percent);
