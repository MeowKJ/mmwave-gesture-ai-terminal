#pragma once

#include <stdint.h>
#include "esp_err.h"

/* DEMO #1：USB 无感下载 + OLED(CO5300/DSI) LVGL + 触摸 三合一自检界面。
   必须在 bsp_display_lvgl_start() 之后调用。 */
esp_err_t demo_ui_init(void);

/* 主循环每 200ms 调一次，刷新状态栏（VSYNC / FPS / USB / 电源 / 触摸） */
void demo_ui_update(uint32_t vsync, uint32_t fps, bool usb_ready, int battery_mv);

/* CDC 行命令入口（由 bsp_usb_set_line_callback 注册） */
void demo_ui_on_cdc_line(const char *line, void *ctx);

/* 无触摸芯片时也能用 CDC 注入合成触点，验证 LVGL 绘制 + HID 上报链路 */
void demo_ui_inject_touch(int x, int y, const char *source);

/* 开机自检标记，供主机脚本判定 PASS/FAIL */
void demo_ui_emit_boot_report(void);

/* 2s 一次的机器可读心跳（DEMO:BEAT vsync=... fps=...），主机用它确认 DPI 在刷 */
void demo_ui_emit_heartbeat(uint32_t vsync, uint32_t fps, bool usb_ready, int battery_mv);
