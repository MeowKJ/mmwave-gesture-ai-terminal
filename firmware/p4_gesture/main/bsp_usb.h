#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 单 USB-C 口复用：CDC(下载/日志) + HID(手势输入)
   CDC 收到 magic 字符串或 esptool 的 DTR/RTS 脉冲都会触发芯片重启进
   ROM 下载模式，全程无需按键。 */
esp_err_t bsp_usb_init(void);
bool bsp_usb_ready(void);
/* 供应用主动请求一次无感下载（例如长按电源键 / 屏幕按钮） */
esp_err_t bsp_usb_request_download(void);
esp_err_t bsp_usb_hid_mouse(int8_t dx, int8_t dy, uint8_t buttons);
esp_err_t bsp_usb_hid_keyboard(uint8_t modifier, const uint8_t keycode[6]);
esp_err_t bsp_usb_hid_gesture(const uint8_t *report, size_t len);

/* CDC 行命令通道：主机发一行文本（\\n 或 \\r 结尾），非下载 magic 的行
   交给应用回调处理，用于 demo 的 PING / STATUS / SCAN / TOUCH 等测试指令。 */
typedef void (*bsp_usb_line_cb_t)(const char *line, void *ctx);
void bsp_usb_set_line_callback(bsp_usb_line_cb_t cb, void *ctx);

/* 主动往 CDC 写一行（不经过 ESP_LOG，主机脚本用它读机器可读的 DEMO: 行） */
/* Nonblocking, newline-terminated record, at most the configured CDC TX size.
 * ESP_OK means the entire record was queued, not delivered. Saturation or
 * concurrent admission refuses the whole record; callers may retry later.
 * Disconnect can still discard or truncate data already in flight. */
esp_err_t bsp_usb_cdc_write(const char *text);
uint16_t bsp_usb_pid(void);
