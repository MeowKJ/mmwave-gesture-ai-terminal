/*
 * DEMO #1 —— 一块 460x460 CO5300 AMOLED 上同时验证三件事：
 *   1) 自动下载：屏幕 FLASH 按钮 / CDC "DOWNLOAD" / 主机 magic → 无按键进 ROM
 *   2) OLED + LVGL：官方 esp_lcd_co5300 + esp_lvgl_port，DSI 1lane 输出，
 *      状态栏实时显示 VSYNC 与 FPS（证明 DPI 真的在刷），COLORS 指令换底色
 *   3) 触摸：CST820 注册成 LVGL indev，触点画点 + 坐标显示 + HID 上报；
 *      即使板上没焊触摸芯片，也能用 CDC "TOUCH x y" 注入合成触点测链路
 *
 * 所有机器可读结果都以 "DEMO:" 前缀写到 USB CDC，主机 demo_test.py 直接解析。
 */
#include "demo_ui.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board_pins.h"
#include "bsp_display.h"
#include "bsp_power.h"
#include "bsp_touch.h"
#include "bsp_usb.h"

static const char *TAG = "DEMO1";

/* 460x460 圆屏，内容压在中间 424px 安全区里 */
#define SCR_W       460
#define PAD_X       18
#define PAD_Y       180
#define PAD_W       424
#define PAD_H       196
#define BTN_Y       388
#define BTN_H       44
#define MAX_DOTS    40
#define EMIT_US     80000         /* 触摸事件节流 80ms */

static lv_obj_t *s_status;
static lv_obj_t *s_pad;
static lv_obj_t *s_pad_hint;
static lv_obj_t *s_dots[MAX_DOTS];
static lv_obj_t *s_cross;
static lv_obj_t *s_flash_btn_label;
static lv_obj_t *s_flash_btn;

static int      s_dot_idx;
static uint32_t s_taps;
static int      s_last_x = -1, s_last_y = -1;
static int64_t  s_last_emit_us;
static int64_t  s_flash_armed_us;
static uint32_t s_color_idx;
static uint32_t s_vsync, s_fps;
static bool     s_usb_ready;
static bool     s_ui_ready;      /* LVGL 对象建好之前禁止触碰 lvgl_port_lock */
static int      s_battery_mv;

static const uint32_t s_colors[] = {
    0x0A1018, 0x800000, 0x006000, 0x000080, 0x505050,
};

#if LV_COLOR_DEPTH == 24
#define DEMO_COLOR_MODE "rgb888"
#else
#define DEMO_COLOR_MODE "rgb565"
#endif

static bool eq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }
static bool starts(const char *a, const char *b) { return strncasecmp(a, b, strlen(b)) == 0; }

static void demo_emit(const char *fmt, ...)
{
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = ((size_t)n < sizeof(buf) - 2) ? (size_t)n : sizeof(buf) - 2;
    buf[len++] = '\n';
    buf[len] = '\0';
    if (bsp_usb_cdc_write(buf) != ESP_OK) {
        ESP_LOGI(TAG, "%s", buf);     /* CDC 没枚举时至少留 UART 日志 */
    }
}

/* ---------------- 触摸绘制 ---------------- */

static void dot_at(lv_obj_t *dot, int x, int y)
{
    lv_obj_set_pos(dot, x - 4, y - 4);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
}

void demo_ui_inject_touch(int x, int y, const char *source)
{
    if (!s_ui_ready) {
        demo_emit("DEMO:TOUCH src=%s BUSY (LVGL not ready)", source ? source : "?");
        return;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= SCR_W) x = SCR_W - 1;
    if (y >= SCR_W) y = SCR_W - 1;

    if (lvgl_port_lock(50)) {
        if (s_pad_hint) {
            lv_obj_add_flag(s_pad_hint, LV_OBJ_FLAG_HIDDEN);
            s_pad_hint = NULL;
        }
        if (s_dots[s_dot_idx]) dot_at(s_dots[s_dot_idx], x, y);
        s_dot_idx = (s_dot_idx + 1) % MAX_DOTS;
        if (s_cross) {
            lv_obj_set_pos(s_cross, x - 11, y - 11);
            lv_obj_remove_flag(s_cross, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_cross);
        }
        lvgl_port_unlock();
    }

    /* 每次有效触点顺手发一个 HID 报表，验证复合设备的 HID 通道 */
    uint8_t report[8] = {
        0x01, (uint8_t)(x >> 8), (uint8_t)x,
        (uint8_t)(y >> 8), (uint8_t)y, 0x00, 0x00, 0x00,
    };
    esp_err_t hid = bsp_usb_hid_gesture(report, sizeof(report));
    if (s_last_x >= 0) {
        int dx = x - s_last_x, dy = y - s_last_y;
        if (dx > 127) {
            dx = 127;
        } else if (dx < -128) {
            dx = -128;
        }
        if (dy > 127) {
            dy = 127;
        } else if (dy < -128) {
            dy = -128;
        }
        (void)bsp_usb_hid_mouse((int8_t)dx, (int8_t)dy, 0);
    }
    s_last_x = x; s_last_y = y;

    demo_emit("DEMO:TOUCH src=%s x=%d y=%d taps=%" PRIu32 " hid=%s",
              source ? source : "?", x, y, s_taps,
              hid == ESP_OK ? "OK" : "NO_HOST");
}

static void clear_dots(void)
{
    if (!s_ui_ready) return;
    if (!lvgl_port_lock(50)) return;
    for (int i = 0; i < MAX_DOTS; i++) {
        if (s_dots[i]) lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_cross) lv_obj_add_flag(s_cross, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
    s_dot_idx = 0;
    s_taps = 0;
    s_last_x = s_last_y = -1;
    demo_emit("DEMO:CLEAR ok");
}

static void pad_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) s_taps++;

    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;

    int64_t now = esp_timer_get_time();
    if (now - s_last_emit_us < EMIT_US) return;
    s_last_emit_us = now;

    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    demo_ui_inject_touch((int)p.x, (int)p.y, "CST820");
}

static void request_flash(void)
{
    demo_emit("DEMO:DOWNLOAD requesting ROM USB-OTG mode (no buttons)");
    (void)bsp_usb_request_download();
}

static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;
    const char *which = (const char *)lv_event_get_user_data(e);
    if (!which) return;

    if (eq(which, "RESCAN")) {
        char scan[160];
        bool ack = false;
        esp_err_t err = bsp_touch_rescan(scan, sizeof(scan), &ack);
        demo_emit("DEMO:SCAN err=%s cst820_0x15=%s devices=[%s]",
                  esp_err_to_name(err), ack ? "ACK" : "NO_ACK",
                  err == ESP_OK ? scan : "BUS_NOT_READY");
    } else if (eq(which, "CLEAR")) {
        clear_dots();
    } else if (eq(which, "FLASH")) {
        int64_t now = esp_timer_get_time();
        if (now - s_flash_armed_us > 3000000) {
            s_flash_armed_us = now;
            if (s_flash_btn_label) {
                lvgl_port_lock(50);
                lv_label_set_text(s_flash_btn_label, "TAP AGAIN");
                lvgl_port_unlock();
            }
            demo_emit("DEMO:FLASH armed, tap again within 3 s");
        } else {
            s_flash_armed_us = 0;
            request_flash();
        }
    }
}

static lv_obj_t *make_button(int x, const char *text, const char *id)
{
    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_pos(btn, x, BTN_Y);
    lv_obj_set_size(btn, 132, BTN_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xBFE8FF), 0);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void *)id);
    return btn;
}

/* ---------------- 状态显示 ---------------- */

static void refresh_status_locked(void)
{
    if (!s_status) return;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "USB  VID=%04X PID=%04X  %s   VSYNC=%" PRIu32 "  FPS=%" PRIu32 "\n"
             "PWR  HOLD=OK  SURVIVED=%d  VBUS=%d  BAT=%dmV\n"
             "LCD  %dx%d %s  1lane DSI  LVGL=9.4\n"
             "TP   CST820@0x15 %s   I2C[%s]\n"
             "LAST (%d,%d)  TAPS=%" PRIu32,
             USB_ESPRESSIF_VID, bsp_usb_pid(), s_usb_ready ? "READY" : "DOWN",
             s_vsync, s_fps,
             bsp_power_download_survived(), bsp_power_usb_attached(), s_battery_mv,
             LCD_H_RES, LCD_V_RES, DEMO_COLOR_MODE,
             bsp_touch_present() ? "OK" : "NO_ACK", bsp_touch_last_scan(),
             s_last_x, s_last_y, s_taps);
    lv_label_set_text(s_status, buf);
}

void demo_ui_update(uint32_t vsync, uint32_t fps, bool usb_ready, int battery_mv)
{
    s_vsync = vsync;
    s_fps = fps;
    s_usb_ready = usb_ready;
    s_battery_mv = battery_mv;
    if (!s_ui_ready) return;
    if (lvgl_port_lock(0)) {          /* 非阻塞：LVGL 忙就下一轮再刷 */
        refresh_status_locked();
        lvgl_port_unlock();
    }
}

void demo_ui_emit_heartbeat(uint32_t vsync, uint32_t fps, bool usb_ready, int battery_mv)
{
    demo_emit("DEMO:BEAT vsync=%" PRIu32 " fps=%" PRIu32
              " usb=%d tp=%d batt=%dmV ui=%d",
              vsync, fps, usb_ready, bsp_touch_present(), battery_mv, s_ui_ready);
}

void demo_ui_emit_boot_report(void)
{
    demo_emit("DEMO:BOOT target=esp32p4 module=ESP32P4C5 demo=1");
    demo_emit("DEMO:USB vid=%04X pid=%04X rom_pid=%04X ready=%d",
              USB_ESPRESSIF_VID, bsp_usb_pid(), USB_ROM_PID, bsp_usb_ready());
    demo_emit("DEMO:DISPLAY %dx%d %s dsi1lane lvgl=%s vsync=%" PRIu32 " fps=%" PRIu32,
              LCD_H_RES, LCD_V_RES, DEMO_COLOR_MODE,
              bsp_display_lvgl_ready() ? "OK" : "FAIL",
              bsp_display_vsync_count(), s_fps);
    demo_emit("DEMO:TOUCH cst820_0x15=%s i2c=[%s] lvgl_indev=%d",
              bsp_touch_present() ? "OK" : "NO_ACK", bsp_touch_last_scan(),
              bsp_touch_present());
    demo_emit("DEMO:PWR hold=OK key_at_boot=%d survived=%d vbus=%d batt=%dmV pad_hold=%d",
              bsp_power_key_at_boot(), bsp_power_download_survived(),
              bsp_power_usb_attached(), bsp_power_battery_mv(),
              bsp_power_pad_hold_active());
    demo_emit("DEMO:RESULT AUTOFLASH=READY LVGL=%s TOUCH=%s HID=READY",
              bsp_display_lvgl_ready() ? "OK" : "FAIL",
              bsp_touch_present() ? "OK" : "NO_CTP");
}

/* ---------------- CDC 测试指令 ---------------- */

static void send_hid_test(void)
{
    static const uint8_t key_a[6] = {0x04, 0, 0, 0, 0, 0};   /* HID usage 'a' */
    static const uint8_t key_none[6] = {0};
    esp_err_t m = bsp_usb_hid_mouse(8, 8, 0);
    esp_err_t k = bsp_usb_hid_keyboard(0, key_a);
    (void)bsp_usb_hid_keyboard(0, key_none);
    uint8_t rep[8] = {0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0};
    esp_err_t g = bsp_usb_hid_gesture(rep, sizeof(rep));
    demo_emit("DEMO:HID mouse=%s keyboard=%s gesture=%s",
              m == ESP_OK ? "OK" : "NO_HOST",
              k == ESP_OK ? "OK" : "NO_HOST",
              g == ESP_OK ? "OK" : "NO_HOST");
}

void demo_ui_on_cdc_line(const char *line, void *ctx)
{
    (void)ctx;
    if (!line || !*line) return;

    if (eq(line, "PING")) {
        demo_emit("DEMO:PONG uptime_us=%lld", (long long)esp_timer_get_time());
    } else if (eq(line, "STATUS") || eq(line, "HELP")) {
        demo_ui_emit_boot_report();
        if (eq(line, "HELP")) {
            demo_emit("DEMO:CMDS PING STATUS SCAN TOUCH_x_y COLORS CLEAR HID DOWNLOAD");
        }
    } else if (eq(line, "SCAN")) {
        char scan[160];
        bool ack = false;
        esp_err_t err = bsp_touch_rescan(scan, sizeof(scan), &ack);
        demo_emit("DEMO:SCAN err=%s cst820_0x15=%s devices=[%s]",
                  esp_err_to_name(err), ack ? "ACK" : "NO_ACK",
                  err == ESP_OK ? scan : "BUS_NOT_READY");
    } else if (starts(line, "TOUCH ")) {
        int x = -1, y = -1;
        if (sscanf(line, "TOUCH %d %d", &x, &y) == 2) {
            s_taps++;
            demo_ui_inject_touch(x, y, "CDC");
        } else {
            demo_emit("DEMO:ERR usage: TOUCH <x> <y>");
        }
    } else if (eq(line, "COLORS")) {
        s_color_idx = (s_color_idx + 1) % (sizeof(s_colors) / sizeof(s_colors[0]));
        if (s_ui_ready && lvgl_port_lock(50)) {
            lv_obj_set_style_bg_color(lv_screen_active(),
                                      lv_color_hex(s_colors[s_color_idx]), 0);
            lvgl_port_unlock();
        }
        demo_emit("DEMO:COLORS index=%" PRIu32 " color=0x%06" PRIX32,
                  s_color_idx, s_colors[s_color_idx]);
    } else if (eq(line, "CLEAR")) {
        clear_dots();
    } else if (eq(line, "HID")) {
        send_hid_test();
    } else if (eq(line, "DOWNLOAD")) {
        request_flash();
    } else {
        demo_emit("DEMO:ERR unknown cmd '%s' (send HELP)", line);
    }
}

/* ---------------- 初始化 ---------------- */

esp_err_t demo_ui_init(void)
{
    if (!bsp_display_lvgl_ready()) return ESP_ERR_INVALID_STATE;
    if (!lvgl_port_lock(200)) return ESP_ERR_TIMEOUT;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(s_colors[0]), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MEOAVE  DEMO #1");
    lv_obj_set_style_text_color(title, lv_color_hex(0x66DDFF), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, 12);
    lv_obj_set_width(title, SCR_W);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "USB auto-flash  /  CO5300 LVGL  /  CST820 touch");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7E9AB5), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sub, 0, 40);
    lv_obj_set_width(sub, SCR_W);

    s_status = lv_label_create(scr);
    lv_obj_set_pos(s_status, PAD_X, 66);
    lv_obj_set_width(s_status, PAD_W);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xC8D6E5), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_LEFT, 0);
    refresh_status_locked();

    s_pad = lv_obj_create(scr);
    lv_obj_set_pos(s_pad, PAD_X, PAD_Y);
    lv_obj_set_size(s_pad, PAD_W, PAD_H);
    lv_obj_set_style_bg_color(s_pad, lv_color_hex(0x101C28), 0);
    lv_obj_set_style_bg_opa(s_pad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_pad, lv_color_hex(0x2A5278), 0);
    lv_obj_set_style_border_width(s_pad, 2, 0);
    lv_obj_set_style_radius(s_pad, 12, 0);
    lv_obj_set_style_pad_all(s_pad, 0, 0);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pad, pad_event_cb, LV_EVENT_ALL, NULL);

    s_pad_hint = lv_label_create(s_pad);
    lv_label_set_text(s_pad_hint, "TOUCH HERE   (or send  TOUCH x y  over USB CDC)");
    lv_obj_set_style_text_color(s_pad_hint, lv_color_hex(0x4E7698), 0);
    lv_obj_center(s_pad_hint);

    for (int i = 0; i < MAX_DOTS; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        s_dots[i] = d;
    }
    s_cross = lv_obj_create(scr);
    lv_obj_set_size(s_cross, 22, 22);
    lv_obj_set_style_radius(s_cross, 11, 0);
    lv_obj_set_style_bg_opa(s_cross, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_cross, lv_color_hex(0xFF5470), 0);
    lv_obj_set_style_border_width(s_cross, 3, 0);
    lv_obj_set_style_pad_all(s_cross, 0, 0);
    lv_obj_remove_flag(s_cross, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cross, LV_OBJ_FLAG_HIDDEN);

    (void)make_button(PAD_X, "RESCAN I2C", "RESCAN");
    s_flash_btn = make_button(164, "USB FLASH", "FLASH");
    s_flash_btn_label = lv_obj_get_child(s_flash_btn, 0);
    (void)make_button(310, "CLEAR", "CLEAR");

    lvgl_port_unlock();

    s_ui_ready = true;
    demo_emit("DEMO:UI_READY buttons=RESCAN/FLASH/CLEAR dots=%d", MAX_DOTS);
    return ESP_OK;
}
