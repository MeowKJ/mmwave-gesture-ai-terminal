/*
 * USB BSP —— 单口同时支持「无感下载」与「USB HID 输入」
 *
 * 硬件：ESP32P4C5 模组 48/49 = USB_DN/USB_DP，P4 的 USB 2.0 High-Speed
 *       OTG 专用 PHY 引脚（由 VDD_USBPHY 供电）。USB-Serial/JTAG 的
 *       GPIO24~27 已被 PA_CTRL / AUDIO_PWR_EN / RADAR_PWR_EN 占用，没有引出。
 *       乐鑫硬件设计指南：使用 USB 2.0 高速 OTG 下载固件必须接专用
 *       USB_DM/USB_DP —— 本板正好满足，所以 ROM 下载口与应用口共用一对脚。
 *
 * 两种状态（同一对 D+/D-，分时复用，VID 都是 0x303A）：
 *   - 应用态：TinyUSB HS port，复合 CDC-ACM + HID，PID 0x4005
 *   - 下载态：ROM 的 USB-OTG CDC，PID 0x0012 (= P4 IMAGE_CHIP_ID 18)
 *
 * 无感下载时序：
 *   1. 主机在应用态 CDC 上写 MEOAVE-DOWNLOAD\\n（或发 esptool 的 DTR/RTS 脉冲）
 *   2. 固件 tud_disconnect() 让主机看到设备断开，置
 *      LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT 后 esp_restart()
 *   3. ROM 以 0x303A:0x0012 重新枚举，主机用 --before no-reset 烧写
 *   4. esptool --after hard-reset（USB-OTG 下走 watchdog_reset）回到应用
 *
 * 关键实现点（都是踩过的坑）：
 *   - FORCE_DOWNLOAD_BOOT 必须在 esp_restart() 的 shutdown handler 里最后写，
 *     对齐 IDF esp_usb_cdc_rom_console 的做法；应用启动时主动清掉，防止残留
 *     导致每次复位都进下载模式。
 *   - 复位前必须 tud_disconnect()，否则主机看不到设备变化，ROM 新 PID 不会枚举。
 *   - 日志 vprintf 回调必须先 va_copy 再消费，且要防递归、防 ISR 上下文。
 *   - HS 下 HID interrupt endpoint 的 bInterval 是指数语义，10 = 64ms（太慢），
 *     必须单独给 HS 描述符用 7（= 8ms）。
 *
 * 硬件前置条件：R13 需从 GND 改接 VBUS_ON，保证复位期间 PWR_HOLD 不掉电。
 */
#include "bsp_usb.h"

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#if CONFIG_APP_USB_HID_ENABLE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "soc/soc.h"
#include "soc/lp_system_reg.h"
#include "bsp_power.h"
#include "board_pins.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "BSP_USB";

/* 主机侧 flash_usb.py 发送的下载请求；可打印、不容易被普通日志误触发。
   CDC 按行解析，DL_CMD 之外的行交给应用回调（demo 测试指令）。 */
#define DL_CMD              "MEOAVE-DOWNLOAD"
#define CDC_LINE_MAX        160

#define REPORT_ID_MOUSE     HID_ITF_PROTOCOL_MOUSE
#define REPORT_ID_KEYBOARD  HID_ITF_PROTOCOL_KEYBOARD
#define REPORT_ID_GESTURE   3

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_GENERIC_INOUT(8, HID_REPORT_ID(REPORT_ID_GESTURE)),
};

#define CFG_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

/* CDC = interface 0/1，HID = interface 2 */
static const uint8_t s_fs_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CFG_DESC_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_HID_DESCRIPTOR(2, 5, false, sizeof(s_hid_report_desc), 0x83, 16, 10),
};

#if TUD_OPT_HIGH_SPEED
/* HS interrupt EP bInterval = 2^(n-1) * 125us；7 => 8ms，10 => 64ms */
static const uint8_t s_hs_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CFG_DESC_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 512),
    TUD_HID_DESCRIPTOR(2, 5, false, sizeof(s_hid_report_desc), 0x83, 16, 7),
};
#endif

static const char *s_str_desc[6] = {
    (const char[]){0x09, 0x04},   /* English (0x0409) */
    "Meoave",
    "Gesture Terminal",
    "0001",
    "Gesture CDC",
    "Gesture HID",
};

static volatile bool s_rebooting;
static volatile bool s_cdc_open;
static volatile int  s_in_log;
static bool          s_vprintf_hooked;
static vprintf_like_t s_orig_vprintf;

/* CDC 按行缓冲 + 应用回调 */
static char s_line[CDC_LINE_MAX];
static size_t s_line_len;
static bsp_usb_line_cb_t s_line_cb;
static void *s_line_ctx;

/* ---------- TinyUSB HID 回调 ---------- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                              hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

/* ---------- 进入 ROM 下载模式 ---------- */
static inline void set_force_download_boot(void)
{
    /* LP_SYS 属于 always-on 域，软件复位不清；ROM 读该位决定 boot:0x107 */
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
}

static inline void clear_force_download_boot(void)
{
    REG_CLR_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
}

/* esp_restart() 在真正复位前调用所有 shutdown handler；在这里最后写一次，
   避免 esp_restart() 内部清理把标志抹掉。只有本次是下载请求时才写。 */
static void usb_shutdown_handler(void)
{
    if (s_rebooting) set_force_download_boot();
}

/* 应用在 main.c 里实现：复位前卸掉屏/音频/雷达等大电流负载，
   让 3V3 电容在 GPIO29 变高阻的复位窗口内尽量撑得久一点。
   R13 未改板时这是唯一能提高下载成功率的软件手段（不能根治）。 */
void __attribute__((weak)) bsp_platform_prepare_download(void) {}

static void reboot_to_download_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(120));       /* 让 magic 发完、CDC 日志刷出 */
    ESP_LOGW(TAG, "USB flash request -> reboot into ROM USB-OTG download mode");

    bsp_platform_prepare_download();      /* 卸掉屏/音频/雷达等大电流负载 */
    /* 主机必须看到断开，ROM 才会以 PID 0x0012 重新枚举 */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));

    /* 在真正复位前把 PWR_HOLD 锁存到 always-on pad hold 寄存器，
       让 3V3 跨过复位窗口；同时写 RTC_NOINIT 标记做掉电体检。 */
    bsp_power_prepare_download_reset();

    set_force_download_boot();            /* 双保险；handler 里还会再写一次 */
    esp_restart();
    vTaskDelete(NULL);
}

static void request_download(const char *why)
{
    if (s_rebooting) return;
    s_rebooting = true;
    ESP_LOGW(TAG, "download requested via %s", why);
    xTaskCreate(reboot_to_download_task, "usb_dl", 3072, NULL, 5, NULL);
}

/* CDC 行分发：下载 magic 自己处理，其它整行交给应用回调。
   按行解析比逐字节匹配 magic 更稳，不会被主机打开串口时的杂散字节打乱，
   也顺带提供了一条无需额外 USB 端点的测试命令通道。 */
static void dispatch_line(const char *line)
{
    if (strcmp(line, DL_CMD) == 0 || strcmp(line, "DOWNLOAD") == 0) {
        request_download("CDC magic");
        return;
    }
    if (s_line_cb) s_line_cb(line, s_line_ctx);
}

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)itf; (void)event;
    uint8_t buf[64];
    size_t n;
    while (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &n) == ESP_OK && n > 0) {
        for (size_t i = 0; i < n && !s_rebooting; i++) {
            uint8_t c = buf[i];
            if (c == '\n' || c == '\r') {
                if (s_line_len) {
                    s_line[s_line_len] = '\0';
                    size_t len = s_line_len;
                    s_line_len = 0;
                    /* 去掉行尾空格，方便 Windows 串口助手 */
                    while (len && (s_line[len - 1] == ' ' || s_line[len - 1] == '\t')) {
                        s_line[--len] = '\0';
                    }
                    if (len) dispatch_line(s_line);
                }
            } else if (c >= 0x20 && c < 0x7F) {
                if (s_line_len < CDC_LINE_MAX - 1) {
                    s_line[s_line_len++] = (char)c;
                } else {
                    s_line_len = 0;       /* 超长行丢弃，避免粘死 */
                }
            }
        }
    }
}

/* 兼容 esptool / VSCode Flash 按钮发出的 DTR/RTS 复位脉冲。
   ClassicReset 与 USBJTAGSerialReset 都会走过 (!dtr && rts) -> (dtr && !rts)。*/
static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    static bool armed;
    static bool was_open;
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    s_cdc_open = dtr;

    if (!dtr && rts) {
        /* A normal host open may briefly expose this state.  Only arm the
           compatibility pulse after the CDC session was already open. */
        armed = was_open;
        was_open = false;
    } else if (dtr && !rts && armed) {
        armed = false;
        was_open = true;
        request_download("DTR/RTS pulse");
    } else if (dtr == rts) {
        armed = false;
        if (dtr) was_open = true;
    }
}

/* 把 ESP_LOG 同步转发到 CDC，一根 USB 线就能看日志。
   注意：va_list 只能消费一次，且不能在 ISR 里调 TinyUSB，更不能递归。 */
static int usb_log_vprintf(const char *fmt, va_list args)
{
    va_list a, b;
    va_copy(a, args);
    va_copy(b, args);
    int ret = s_orig_vprintf ? s_orig_vprintf(fmt, a) : vprintf(fmt, a);
    va_end(a);

    if (!xPortInIsrContext() && s_cdc_open && tud_mounted() &&
        __sync_lock_test_and_set(&s_in_log, 1) == 0) {
        char buf[256];
        int len = vsnprintf(buf, sizeof(buf), fmt, b);
        if (len > 0) {
            size_t n = ((size_t)len < sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)buf, n);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
        __sync_lock_release(&s_in_log);
    }
    va_end(b);
    return ret;
}

esp_err_t bsp_usb_init(void)
{
    /* ROM 可能把 FORCE_DOWNLOAD_BOOT 留着；应用一启动就清，
       否则烧完 hard-reset 会再次掉进下载模式形成死循环。 */
    clear_force_download_boot();
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(usb_shutdown_handler),
                        TAG, "register shutdown handler failed");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL;    /* 默认描述符：VID 0x303A / PID 0x4005 */
    tusb_cfg.descriptor.string = s_str_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
    tusb_cfg.descriptor.full_speed_config = s_fs_cfg_desc;
#if TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.high_speed_config = s_hs_cfg_desc;
#endif
    tusb_cfg.phy.self_powered = false;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install failed");

    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_cb,
        .callback_line_state_changed = cdc_line_state_cb,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&cdc_cfg), TAG, "CDC ACM init failed");

    /* 只允许安装一次：重复安装会让 s_orig_vprintf 指向自己，造成无限递归。 */
    if (!s_vprintf_hooked) {
        s_orig_vprintf = esp_log_set_vprintf(usb_log_vprintf);
        s_vprintf_hooked = true;
    }
    ESP_LOGI(TAG, "USB composite CDC+HID ready (HS OTG, VID=0x%04X PID=0x%04X)",
             USB_ESPRESSIF_VID, USB_APP_PID);
    return ESP_OK;
}

bool bsp_usb_ready(void) { return tud_mounted(); }

esp_err_t bsp_usb_request_download(void)
{
    request_download("application call");
    return ESP_OK;
}

esp_err_t bsp_usb_hid_mouse(int8_t dx, int8_t dy, uint8_t buttons)
{
    if (!tud_mounted() || !tud_hid_ready()) return ESP_ERR_INVALID_STATE;
    return tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, 0, 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_usb_hid_keyboard(uint8_t modifier, const uint8_t keycode[6])
{
    if (!tud_mounted() || !tud_hid_ready()) return ESP_ERR_INVALID_STATE;
    return tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keycode) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_usb_hid_gesture(const uint8_t *report, size_t len)
{
    if (!tud_mounted() || !tud_hid_ready() || !report || !len) return ESP_ERR_INVALID_STATE;
    return tud_hid_report(REPORT_ID_GESTURE, report, len) ? ESP_OK : ESP_FAIL;
}

void bsp_usb_set_line_callback(bsp_usb_line_cb_t cb, void *ctx)
{
    s_line_cb = cb;
    s_line_ctx = ctx;
}

esp_err_t bsp_usb_cdc_write(const char *text)
{
    if (!text || !tud_mounted()) return ESP_ERR_INVALID_STATE;
    size_t len = strlen(text);
    if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)text, len) != ESP_OK) {
        return ESP_FAIL;
    }
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

uint16_t bsp_usb_pid(void) { return USB_APP_PID; }

#else  /* !CONFIG_APP_USB_HID_ENABLE */

esp_err_t bsp_usb_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool bsp_usb_ready(void) { return false; }
esp_err_t bsp_usb_request_download(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bsp_usb_hid_mouse(int8_t dx, int8_t dy, uint8_t buttons)
{ (void)dx; (void)dy; (void)buttons; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bsp_usb_hid_keyboard(uint8_t modifier, const uint8_t keycode[6])
{ (void)modifier; (void)keycode; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bsp_usb_hid_gesture(const uint8_t *report, size_t len)
{ (void)report; (void)len; return ESP_ERR_NOT_SUPPORTED; }
void bsp_usb_set_line_callback(bsp_usb_line_cb_t cb, void *ctx)
{ (void)cb; (void)ctx; }
esp_err_t bsp_usb_cdc_write(const char *text)
{ (void)text; return ESP_ERR_NOT_SUPPORTED; }
uint16_t bsp_usb_pid(void) { return 0; }

#endif
