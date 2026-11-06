#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_touch.h"

/* CST820：I2C0(IO8/IO9) + RST(IO10) + INT(IO11)。
   没有 ACK 时只报警、不返回错误（部分 FLS2 屏本身不带触摸）。 */
esp_err_t bsp_touch_init(void);
void bsp_touch_poll(void);
bool bsp_touch_present(void);

/* 供 LVGL 注册 indev 用；NULL 表示没有触摸控制器 */
esp_lcd_touch_handle_t bsp_touch_handle(void);

/* 注册 LVGL 后由 lvgl_port 自己周期读触摸，手动 poll 会抢数据 */
void bsp_touch_set_lvgl_owner(bool owned);

/* 重新扫 I2C0 并把 ACK 的地址格式化成 "0x15 0x18 ..."；out 可给 NULL */
esp_err_t bsp_touch_rescan(char *out, size_t out_len, bool *cst820_ack);
const char *bsp_touch_last_scan(void);
