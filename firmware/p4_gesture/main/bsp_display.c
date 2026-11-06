/*
 * 显示 BSP —— 依据 osptek 官方 esp32p4-idf6-co5300-mipi-lvgl-port-lvgl9
 *
 * 与官方例程的差异（都是为了这块板）：
 *  1. 增加 VCI_EN(GPIO20) 屏供电使能 + OLED_RST(GPIO12) 硬件复位
 *  2. 组件 esp_lcd_co5300 的 DCS 读 ID 已本地 patch 跳过（本屏不回 BTA，
 *     官方驱动无超时会卡死并触发 task_wdt）
 *  3. esp_lcd_panel_init() 之后补 set_pattern(BAR_VERTICAL)->(NONE)，
 *     否则 DSI bridge 的 DPI 输出不会真正打开，host_vsync 恒为 0
 *  4. 先跑纯色自检，再交给 LVGL
 */
#include "bsp_display.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_touch.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "board_pins.h"
#include "bsp_common.h"
#include "co5300_init_cmds.h"

static const char *TAG = "BSP_LCD";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static volatile uint32_t s_vsync;
static lv_obj_t *s_status_label;
static lv_display_t *s_disp;

#if LV_COLOR_DEPTH == 24
#define APP_BPP        24
#define APP_DSI_MBPS   500
#define APP_DPI_FMT    LCD_COLOR_FMT_RGB888
#else
#define APP_BPP        16
#define APP_DSI_MBPS   330
#define APP_DPI_FMT    LCD_COLOR_FMT_RGB565
#endif

static bool on_vsync(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    (void)panel; (void)edata; (void)ctx;
    s_vsync++;
    return false;
}

esp_err_t bsp_display_init(void)
{
    /* 屏供电：CN3.24 VCI_EN 高有效，R21 10K 上拉到 3V3 保证 GPIO 高阻时也供电。
       先给电再硬复位，等 20ms 让屏内部 LDO 稳定。 */
    bsp_gpio_out(PIN_LCD_VCI_EN, 1);
    bsp_gpio_in(PIN_LCD_TE, false);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* MIPI DSI PHY 2.5V（P4 内部 LDO3，官方例程同款） */
    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "LDO3 2.5V failed");

    esp_lcd_dsi_bus_handle_t bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = LCD_DSI_LANES,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = APP_DSI_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "new DSI bus failed");

    const esp_lcd_dbi_io_config_t io_cfg = CO5300_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_cfg, &s_io), TAG, "new DBI IO failed");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .in_color_format = APP_DPI_FMT,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_back_porch = 72,
            .hsync_pulse_width = 4,
            .hsync_front_porch = 32,
            .vsync_back_porch = 4,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 6,
        },
    };

    co5300_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = bus,
            .dpi_config = &dpi_cfg,
        },
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = APP_BPP,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel), TAG, "new CO5300 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(s_panel, 100), TAG, "brightness failed");

    const esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_vsync = on_vsync};
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL),
                        TAG, "register vsync cb failed");

    /* 关键：只有 pattern 发生变化才会 update_dpi_config 打开 DPI 输出 */
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_BAR_VERTICAL),
                        TAG, "set BAR pattern failed");
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "DSI color bars, host_vsync=%" PRIu32, s_vsync);
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_NONE),
                        TAG, "set NONE pattern failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "panel ready: %dx%d %dbpp, DSI 1lane %dMbps, DPI %dMHz, vsync=%" PRIu32,
             LCD_H_RES, LCD_V_RES, APP_BPP, APP_DSI_MBPS, LCD_DPI_CLK_MHZ, s_vsync);
    return ESP_OK;
}

static void fill_solid(void *fb, uint8_t r, uint8_t g, uint8_t b)
{
#if APP_BPP == 24
    uint8_t *p = (uint8_t *)fb;
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        p[i * 3 + 0] = r; p[i * 3 + 1] = g; p[i * 3 + 2] = b;
    }
#else
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    uint16_t *p = (uint16_t *)fb;
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) p[i] = c;
#endif
}

void bsp_display_self_test(void)
{
    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || fb == NULL) {
        ESP_LOGW(TAG, "no frame buffer, skip color self test");
        return;
    }

    static const struct { const char *name; uint8_t r, g, b; } colors[] = {
        {"WHITE", 255, 255, 255}, {"RED", 255, 0, 0}, {"GREEN", 0, 255, 0},
        {"BLUE", 0, 0, 255},     {"BLACK", 0, 0, 0},
    };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        fill_solid(fb, colors[i].r, colors[i].g, colors[i].b);
        (void)esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, fb);
        uint32_t before = s_vsync;
        ESP_LOGI(TAG, "SELFTEST %s", colors[i].name);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_COLOR_CYCLE_SEC * 1000));
        ESP_LOGI(TAG, "SELFTEST %s done, vsync_delta=%" PRIu32,
                 colors[i].name, s_vsync - before);
    }

    for (int pct = 0; pct <= 100; pct += 20) {
        bsp_display_set_brightness(pct);
        ESP_LOGI(TAG, "BRIGHTNESS %d%%", pct);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    bsp_display_set_brightness(100);
}

esp_err_t bsp_display_lvgl_start(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
#if LV_COLOR_DEPTH == 24
        .color_format = LV_COLOR_FORMAT_RGB888,
#else
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {
            .buff_dma = false, .buff_spiram = true, .sw_rotate = true,
            .swap_bytes = false, .full_refresh = false, .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {.avoid_tearing = false}};
    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "add DSI display failed");

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL started on DSI display");
    return ESP_OK;
}

bool bsp_display_lvgl_ready(void) { return s_disp != NULL; }

esp_err_t bsp_display_lvgl_add_touch(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_ERR_INVALID_STATE, TAG, "LVGL not started");
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_ARG, TAG, "no touch handle");
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = tp,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "add LVGL touch indev failed");
    ESP_LOGI(TAG, "LVGL touch indev registered (esp_lcd_touch -> LVGL pointer)");
    return ESP_OK;
}

void bsp_display_set_status(const char *text)
{
    if (!s_status_label) return;
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_status_label, text);
        lvgl_port_unlock();
    }
}

uint32_t bsp_display_vsync_count(void) { return s_vsync; }

esp_err_t bsp_display_set_brightness(uint8_t percent)
{
    return esp_lcd_panel_co5300_set_brightness(s_panel, percent);
}
