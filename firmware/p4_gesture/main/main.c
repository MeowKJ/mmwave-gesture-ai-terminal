/*
 * 中控底板 V1.0 —— ESP32P4C5（鱼鹰光电 OspreyPi）DEMO #1
 *
 * 目标：一根 USB-C 线，验证「无感下载 + OLED/LVGL + 触摸」三件事。
 *
 * 顺序（刻意安排）：
 *   0. 电源键开机：PWR_HOLD 拉高锁存 VSYS，释放上次下载留下的 pad hold —— 必须最先做
 *   1. USB CDC+HID 尽早枚举 —— 即使后面显示初始化卡住，也能无感重新烧写
 *   2. 屏：官方 CO5300 组件 + 官方初始化表；纯色自检 -> LVGL
 *   3. 触摸：CST820 I2C 扫描 -> 注册成 LVGL indev -> DEMO UI
 *   4. 音频 / 雷达：仅初始化并轮询，单个失败不影响 DEMO
 */
#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "bsp_common.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_power.h"
#include "bsp_radar.h"
#include "bsp_touch.h"
#include "bsp_usb.h"
#include "demo_ui.h"

static const char *TAG = "GESTURE";
static uint32_t s_fps;
static uint32_t s_last_vsync;
static int64_t s_last_fps_us;

/* bsp_usb.c 的弱符号实现：进 ROM 下载模式前把 3V3 负载压到最低。
   PWR_HOLD 本身由 bsp_power_prepare_download_reset() 用 LP_SYS pad hold 锁存。 */
void bsp_platform_prepare_download(void)
{
    ESP_LOGW(TAG, "prepare download: dropping display/audio/radar load");
    bsp_display_set_brightness(0);
    bsp_gpio_out(PIN_LCD_VCI_EN, 0);
    bsp_gpio_out(PIN_PA_CTRL, 0);
    bsp_gpio_out(PIN_AUDIO_PWR_EN, 0);
    bsp_gpio_out(PIN_RADAR_PWR_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static uint32_t sample_fps(void)
{
    int64_t now = esp_timer_get_time();
    uint32_t v = bsp_display_vsync_count();
    int64_t dt = now - s_last_fps_us;
    if (dt >= 1000000) {
        s_fps = (uint32_t)(((uint64_t)(v - s_last_vsync) * 1000000) / (uint64_t)dt);
        s_last_vsync = v;
        s_last_fps_us = now;
    }
    return s_fps;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== MEOAVE DEMO #1 | IDF %s | chip=%s ===",
             esp_get_idf_version(), CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "reset_reason=%d", esp_reset_reason());

    /* 0. 电源键开机：SW1 按下由硬件上电，这里把 PWR_HOLD 锁存住，
          之后松手不掉电。本版本不做长按动作、不做软件关机。 */
    bsp_power_init();
    ESP_LOGI(TAG, "=== POWER ON: PWR_HOLD latched, SW1 can be released now ===");
    ESP_LOGI(TAG, "battery=%d mV, USB=%d, key=%d, key_at_boot=%d, buck_pg=%d, survived=%d",
             bsp_power_battery_mv(), bsp_power_usb_attached(),
             bsp_power_key_pressed(), bsp_power_key_at_boot(), bsp_power_buck_ok(),
             bsp_power_download_survived());

    /* 1. USB：下载通道 + HID 输入 + 日志，越早越好 */
    esp_err_t err = bsp_usb_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "USB init failed: %s", esp_err_to_name(err));
    bsp_usb_set_line_callback(demo_ui_on_cdc_line, NULL);   /* 无屏也能跑测试指令 */
    demo_ui_emit_boot_report();                             /* 开机快照（主机可能还没打开 CDC） */

    /* 2. 显示屏 */
    err = bsp_display_init();
    if (err == ESP_OK) {
#if CONFIG_APP_SELFTEST_COLORS
        bsp_display_self_test();
#endif
        err = bsp_display_lvgl_start();
        if (err != ESP_OK) ESP_LOGW(TAG, "LVGL start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
    }

    /* 3. 触摸 + LVGL indev */
    bool tp_ok = (bsp_touch_init() == ESP_OK);
    bool indev_ok = false;
    if (tp_ok && bsp_display_lvgl_ready()) {
        indev_ok = (bsp_display_lvgl_add_touch(bsp_touch_handle()) == ESP_OK);
        if (indev_ok) bsp_touch_set_lvgl_owner(true);
    }
    ESP_LOGI(TAG, "touch: present=%d lvgl_indev=%d i2c=[%s]",
             tp_ok, indev_ok, bsp_touch_last_scan());

    /* 4. 其余外设 */
    if (bsp_audio_init() != ESP_OK) ESP_LOGW(TAG, "audio not available");
    if (bsp_radar_init() != ESP_OK) ESP_LOGW(TAG, "radar not available");
#if CONFIG_APP_AUDIO_BEEP
    bsp_audio_beep(200);
#endif

    /* 5. DEMO UI */
    err = demo_ui_init();
    ESP_LOGI(TAG, "demo_ui_init: %s", esp_err_to_name(err));
    demo_ui_emit_boot_report();      /* 完整报告：此时 LVGL / 触摸状态已确定 */
    ESP_LOGI(TAG, "=== DEMO #1 ready: send HELP over USB CDC ===");

    unsigned round = 0;
#if CONFIG_APP_USB_DOWNLOAD_ON_LONG_KEY
    unsigned key_held = 0;
#endif
    /* 电源键只负责开机：本版本不做长按动作，也不做任何软件关机。 */
    while (true) {
        bsp_touch_poll();            /* 注册 LVGL indev 后自动变成 no-op */
        bsp_radar_poll();
        uint32_t fps = sample_fps();

#if CONFIG_APP_USB_DOWNLOAD_ON_LONG_KEY
        /* 可选兜底（默认关闭）：长按电源键 3 秒请求 USB 下载。
           这不是关机，但会 tud_disconnect() + esp_restart()，屏幕熄灭，
           且 R13 下拉使复位窗口存在真掉电风险 —— 所以默认不编进固件。
           PWR_DET 按下为低；手还按着键时正好给 HOLD 电路保电。 */
        if (bsp_power_key_pressed()) {
            if (++key_held >= 15) {
                bsp_usb_request_download();
                key_held = 0;
            }
        } else {
            key_held = 0;
        }
#endif

        if (++round % 5 == 0) {       /* 1 s */
            demo_ui_update(bsp_display_vsync_count(), fps, bsp_usb_ready(),
                           bsp_power_battery_mv());
        }
        if (round % 10 == 0) {       /* 2 s：主机脚本的心跳 / 断言依据 */
            demo_ui_emit_heartbeat(bsp_display_vsync_count(), fps,
                                   bsp_usb_ready(), bsp_power_battery_mv());
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
