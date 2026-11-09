/* Runtime display tests: no clock/timing/initialization command changes,
 * or callback replacement. Hardware bars bypass the LVGL framebuffer path.
 * API success/draw counts are host-side evidence, NOT proof of panel reception.
 */
#include <stdio.h>
#include <string.h>

#include "fls2_display_diag.h"
#include "fls2_display_startup.h"
#include "bsp_usb.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "hal/mipi_dsi_ll.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_lcd_panel_handle_t s_panel;
static uint32_t s_draw_ok, s_draw_errors;
static esp_err_t s_last_draw_error;
static int s_request; /* 0 none, 1 hardware bars, 2 restore normal */
static bool s_bars;
static int64_t s_restore_at_us;
static bool s_hardware_request, s_probe_request, s_colors_request;
static bool s_frame_request, s_redraw_request;
static lv_obj_t *s_test_screen, *s_previous_screen;
static int s_color_step;
static int64_t s_next_color_us;
static bool s_cancel_colors;
static bool s_probe_attempted, s_probe_needs_restart;

bool fls2_display_diag_colors_active(void) { return s_test_screen != NULL; }
bool fls2_display_diag_needs_restart(void) { return s_probe_needs_restart; }

esp_err_t fls2_display_set_brightness(uint8_t percent)
{
    if (percent < 15 || percent > 100) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    esp_lcd_panel_handle_t panel = s_panel;
    portEXIT_CRITICAL(&s_lock);
    return panel ? esp_lcd_panel_co5300_set_brightness(panel, percent) : ESP_ERR_INVALID_STATE;
}

void fls2_display_diag_cancel_colors(void)
{
    portENTER_CRITICAL(&s_lock);
    s_cancel_colors = true;
    portEXIT_CRITICAL(&s_lock);
}

static void diagnostic_line(const char *line)
{
    (void)bsp_usb_cdc_write(line);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void fls2_display_diag_request_hardware(void)
{
    portENTER_CRITICAL(&s_lock);
    s_hardware_request = true;
    portEXIT_CRITICAL(&s_lock);
}

void fls2_display_diag_request_frame(void)
{
    portENTER_CRITICAL(&s_lock); s_frame_request=true; portEXIT_CRITICAL(&s_lock);
}

void fls2_display_diag_request_redraw(void)
{
    portENTER_CRITICAL(&s_lock); s_redraw_request=true; portEXIT_CRITICAL(&s_lock);
}

static void report_framebuffer(esp_lcd_panel_handle_t panel)
{
    if (!panel || !lvgl_port_lock(200)) {
        diagnostic_line("FLS2:FRAME result=UNAVAILABLE reason=PANEL_OR_GUI_BUSY\n");
        return;
    }
    lv_display_t *display=lv_display_get_default();
    void *buffer=NULL;
    esp_err_t err=esp_lcd_dpi_panel_get_frame_buffer(panel,1,&buffer);
    if (err!=ESP_OK || !buffer || !display || lv_display_get_color_format(display)!=LV_COLOR_FORMAT_RGB888) {
        lvgl_port_unlock();
        diagnostic_line("FLS2:FRAME result=UNAVAILABLE reason=NO_RGB888_BUFFER\n");
        return;
    }
    /* This profile has exactly one 460x460 RGB888 DPI framebuffer. Read the
       scanout backing store, not a newly-rendered offscreen screenshot. The
       GUI lock excludes CPU flush/copy writes; DMA is a reader only. Do not
       invalidate CPU caches or write pixels here: diagnostics must be passive. */
    const uint8_t *pixels=buffer;
    uint32_t black=0,white=0,colored=0,yellow=0,hash=2166136261u;
    int x0=460,y0=460,x1=-1,y1=-1;
    for (int y=0;y<460;++y) for (int x=0;x<460;++x) {
        const uint8_t *p=pixels+((size_t)y*460+x)*3;
        unsigned b=p[0],g=p[1],r=p[2];
        for (unsigned i=0;i<3;++i) hash=(hash^p[i])*16777619u;
        if (r<8 && g<8 && b<8) black++;
        else { if (x<x0) x0=x; if (x>x1) x1=x; if (y<y0) y0=y; if (y>y1) y1=y; }
        if (r>247 && g>247 && b>247) white++;
        if (r!=g || g!=b) colored++;
        if (r>64 && g>64 && b<64) yellow++;
    }
    uint32_t bytes=460*460*3;
    lvgl_port_unlock();
    char line[224];
    snprintf(line,sizeof(line),"FLS2:FRAME result=OK bytes=%lu hash=%08lX black=%lu white=%lu colored=%lu yellow=%lu bounds=%d,%d,%d,%d panel_reception=NOT_VERIFIED\n",
        (unsigned long)bytes,(unsigned long)hash,(unsigned long)black,(unsigned long)white,
        (unsigned long)colored,(unsigned long)yellow,x0,y0,x1,y1);
    diagnostic_line(line);
}

static void redraw_frame(void)
{
    if (!lvgl_port_lock(200)) {
        diagnostic_line("FLS2:REDRAW result=BUSY\n"); return;
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(lv_display_get_default());
    lvgl_port_unlock();
    diagnostic_line("FLS2:REDRAW result=SUBMITTED panel_reception=NOT_VERIFIED\n");
}

void fls2_display_diag_request_probe(void)
{
    portENTER_CRITICAL(&s_lock);
    s_probe_request = true;
    portEXIT_CRITICAL(&s_lock);
}

void fls2_display_diag_request_colors(void)
{
    portENTER_CRITICAL(&s_lock);
    s_colors_request = true;
    portEXIT_CRITICAL(&s_lock);
}

static void report_hardware(void)
{
    const fls2_display_startup_state_t *startup = fls2_display_startup_state();
    char startup_line[224];
    snprintf(startup_line, sizeof(startup_line),
             "FLS2:DISPLAY_START prepared=%d host_before=0x%08lX phy_before=0x%08lX "
             "host_off=0x%08lX phy_off=0x%08lX panel_reception=NOT_VERIFIED\n",
             startup->prepared, (unsigned long)startup->host_before,
             (unsigned long)startup->phy_before, (unsigned long)startup->host_after,
             (unsigned long)startup->phy_after);
    diagnostic_line(startup_line);
    dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(0);
    dsi_brg_dev_t *bridge = MIPI_DSI_LL_GET_BRG(0);
    uint32_t out = GPIO.out.val, enable = GPIO.enable.val;
    char line[224];
    snprintf(line, sizeof(line),
             "FLS2:GPIO_OUT vci_gpio20=%lu rst_gpio12=%lu hold_gpio29=%lu "
             "outputs_enabled=%lu,%lu,%lu voltage=NOT_MEASURED\n",
             (unsigned long)((out >> 20) & 1), (unsigned long)((out >> 12) & 1),
             (unsigned long)((out >> 29) & 1), (unsigned long)((enable >> 20) & 1),
             (unsigned long)((enable >> 12) & 1), (unsigned long)((enable >> 29) & 1));
    diagnostic_line(line);
    snprintf(line, sizeof(line),
             "FLS2:DSI_HOST pll_locked=%d lanes=%u command_mode=%u bridge_dpi_en=%u "
             "phy=0x%08lX color=0x%08lX\n",
             mipi_dsi_phy_ll_is_pll_locked(host), (unsigned)host->phy_if_cfg.n_lanes + 1,
             (unsigned)host->mode_cfg.cmd_video_mode, (unsigned)bridge->dpi_misc_config.dpi_en,
             (unsigned long)host->phy_status.val, (unsigned long)host->dpi_color_coding.val);
    diagnostic_line(line);
    snprintf(line, sizeof(line),
             "FLS2:DSI_ERRORS int0=0x%08lX int1=0x%08lX fifo=0x%08lX "
             "reset_reason=%d uptime_ms=%lld\n",
             (unsigned long)host->int_st0.val, (unsigned long)host->int_st1.val,
             (unsigned long)host->cmd_pkt_status.val, (int)esp_reset_reason(),
             (long long)(esp_timer_get_time() / 1000));
    diagnostic_line(line);
}

/* Diagnostic adaptation of the exact packet sequence in IDF 6.1
 * components/esp_hal_lcd/mipi_dsi_hal.c: MRPS, DCS READ, busy wait, RX FIFO.
 * Unlike the stock HAL, every polling loop has a shared 150-ms deadline and
 * yields to FreeRTOS. A timeout is NOT a broken-panel verdict. No library or
 * managed-component source is changed. This uses target-specific LL APIs.
 */
static esp_err_t read_dcs_bounded(dsi_host_dev_t *host, uint8_t cmd,
                                  uint8_t *data, size_t size, const char **stage)
{
    int64_t deadline = esp_timer_get_time() + 150000;
#define WAIT_READY(condition, label) do { \
    *stage = label; \
    while (!(condition)) { \
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT; \
        vTaskDelay(1); \
    } \
} while (0)
    /* Clear any old payload so a previous response cannot masquerade as this
       command's result. Bound draining as well, even with a defective host. */
    *stage = "drain";
    for (int i = 0; !mipi_dsi_host_ll_gen_is_read_fifo_empty(host); i++) {
        if (i >= 64 || esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        (void)mipi_dsi_host_ll_gen_read_payload_fifo(host);
    }
    WAIT_READY(!mipi_dsi_host_ll_gen_is_read_cmd_busy(host), "previous_read_busy");
    WAIT_READY(!mipi_dsi_host_ll_gen_is_cmd_fifo_full(host), "mrps_fifo");
    mipi_dsi_host_ll_gen_set_packet_header(host, 0, MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT,
                                          0, (uint8_t)size);
    WAIT_READY(!mipi_dsi_host_ll_gen_is_cmd_fifo_full(host), "read_fifo");
    mipi_dsi_host_ll_gen_set_packet_header(host, 0, MIPI_DSI_DT_DCS_READ_0, 0, cmd);
    WAIT_READY(!mipi_dsi_host_ll_gen_is_read_cmd_busy(host), "read_busy");
    WAIT_READY(!mipi_dsi_host_ll_gen_is_read_fifo_empty(host), "rx_empty");
    uint32_t word = mipi_dsi_host_ll_gen_read_payload_fifo(host);
    for (size_t i = 0; i < size; i++) data[i] = (word >> (i * 8)) & 0xff;
    *stage = "received";
    return ESP_OK;
#undef WAIT_READY
}

static void probe_registers(void)
{
    if (s_probe_attempted) {
        diagnostic_line("FLS2:DCS_PROBE result=SKIPPED reason=ONCE_PER_BOOT reboot_required=1\n");
        return;
    }
    /* The official profile has exactly one DSI host at bus 0, VC 0 and 8-bit
       DCS commands. Do not run this against arbitrary other board profiles. */
    if (!lvgl_port_lock(200)) {
        diagnostic_line("FLS2:DCS_PROBE result=SKIPPED reason=LVGL_LOCK_TIMEOUT\n");
        return;
    }
    dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(0);
    dsi_brg_dev_t *bridge = MIPI_DSI_LL_GET_BRG(0);
    if (!mipi_dsi_phy_ll_is_pll_locked(host)) {
        lvgl_port_unlock();
        diagnostic_line("FLS2:DCS_PROBE result=SKIPPED reason=PHY_PLL_UNLOCKED\n");
        return;
    }
    if (mipi_dsi_host_ll_gen_is_read_cmd_busy(host)) {
        s_probe_needs_restart = true;
        lvgl_port_unlock();
        diagnostic_line("FLS2:DCS_PROBE result=SKIPPED reason=READ_ENGINE_BUSY reboot_required=1\n");
        return;
    }
    s_probe_attempted = true;
    uint32_t saved_mode = host->mode_cfg.val;
    uint32_t saved_packet = host->pckhdl_cfg.val;
    uint32_t saved_vcid = host->gen_vcid.val;
    bool saved_dpi = bridge->dpi_misc_config.dpi_en;
    mipi_dsi_brg_ll_enable_dpi_output(bridge, false);
    mipi_dsi_brg_ll_update_dpi_config(bridge);
    mipi_dsi_host_ll_enable_video_mode(host, false);
    mipi_dsi_host_ll_enable_bta(host, true);
    mipi_dsi_host_ll_gen_set_rx_vcid(host, 0);
    const struct { uint8_t cmd, size; } reads[] = {
        {0x04, 3}, {0x0A, 1}, {0x0C, 1}, {0x0F, 1}, {0x52, 1},
    };
    for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
        uint8_t data[3] = {0};
        const char *stage = "start";
        int64_t begin = esp_timer_get_time();
        esp_err_t err = read_dcs_bounded(host, reads[i].cmd, data, reads[i].size, &stage);
        char line[224];
        snprintf(line, sizeof(line),
                 "FLS2:DCS_READ cmd=0x%02X bytes=%u result=%s stage=%s "
                 "data=%02X,%02X,%02X elapsed_ms=%lld\n",
                 reads[i].cmd, reads[i].size, esp_err_to_name(err), stage,
                 data[0], data[1], data[2], (long long)((esp_timer_get_time() - begin) / 1000));
        diagnostic_line(line);
        if (mipi_dsi_host_ll_gen_is_read_cmd_busy(host)) {
            diagnostic_line("FLS2:DCS_PROBE remaining=SKIPPED reason=READ_ENGINE_STILL_BUSY\n");
            break;
        }
    }
    host->gen_vcid.val = saved_vcid;
    host->pckhdl_cfg.val = saved_packet;
    host->mode_cfg.val = saved_mode;
    mipi_dsi_brg_ll_enable_dpi_output(bridge, saved_dpi);
    mipi_dsi_brg_ll_update_dpi_config(bridge);
    s_probe_needs_restart = mipi_dsi_host_ll_gen_is_read_cmd_busy(host);
    lvgl_port_unlock();
    char result_line[160];
    snprintf(result_line, sizeof(result_line),
             "FLS2:DCS_PROBE host_config=RESTORED read_engine_idle=%d reboot_required=%d "
             "fault_verdict=NOT_DETERMINED\n", !s_probe_needs_restart, s_probe_needs_restart);
    diagnostic_line(result_line);
}

static bool stop_colors(void)
{
    if (!s_test_screen) return true;
    if (!lvgl_port_lock(200)) return false;
    lv_screen_load(s_previous_screen);
    lv_obj_delete(s_test_screen);
    s_test_screen = s_previous_screen = NULL;
    lvgl_port_unlock();
    diagnostic_line("FLS2:COLOR_TEST result=CANCELLED widgets=RESTORED\n");
    return true;
}

static void service_colors(bool start)
{
    int64_t now = esp_timer_get_time();
    if (!start && (!s_test_screen || now < s_next_color_us)) return;
    if (!lvgl_port_lock(200)) {
        if (start) diagnostic_line("FLS2:COLOR_TEST result=SKIPPED reason=LVGL_LOCK_TIMEOUT\n");
        return;
    }
    if (start && !s_test_screen) {
        s_previous_screen = lv_screen_active();
        s_test_screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(s_test_screen);
        lv_obj_set_size(s_test_screen, 460, 460);
        lv_obj_set_style_bg_opa(s_test_screen, LV_OPA_COVER, 0);
        s_color_step = 0;
    }
    const uint32_t colors[] = {0xffffff, 0xff0000, 0x00ff00, 0x0000ff};
    const char *names[] = {"WHITE", "RED", "GREEN", "BLUE"};
    char line[160];
    if (s_color_step < 4) {
        lv_obj_set_style_bg_color(s_test_screen, lv_color_hex(colors[s_color_step]), 0);
        lv_screen_load(s_test_screen);
        lv_obj_invalidate(s_test_screen);
        snprintf(line, sizeof(line), "FLS2:COLOR_TEST color=%s duration_s=3 visual=NOT_VERIFIED\n",
                 names[s_color_step]);
        s_color_step++;
        s_next_color_us = now + 3000000;
    } else {
        lv_screen_load(s_previous_screen);
        lv_obj_delete(s_test_screen);
        s_test_screen = NULL;
        s_previous_screen = NULL;
        snprintf(line, sizeof(line), "FLS2:COLOR_TEST result=SEQUENCE_DONE widgets=RESTORED\n");
    }
    lvgl_port_unlock();
    diagnostic_line(line);
}

esp_err_t __real_esp_lcd_new_panel_co5300(esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_panel_dev_config_t *cfg,
                                        esp_lcd_panel_handle_t *panel);
esp_err_t __real_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                         int x0, int y0, int x1, int y1,
                                         const void *pixels);

esp_err_t __wrap_esp_lcd_new_panel_co5300(esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_panel_dev_config_t *cfg,
                                        esp_lcd_panel_handle_t *panel)
{
    esp_err_t err = __real_esp_lcd_new_panel_co5300(io, cfg, panel);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_panel = *panel;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t __wrap_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                         int x0, int y0, int x1, int y1,
                                         const void *pixels)
{
    esp_err_t err = __real_esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, pixels);
    portENTER_CRITICAL(&s_lock);
    if (panel == s_panel) {
        if (err == ESP_OK) s_draw_ok++; else s_draw_errors++;
        s_last_draw_error = err;
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}

void fls2_display_diag_request(bool bars)
{
    portENTER_CRITICAL(&s_lock);
    s_request = bars ? 1 : 2;
    portEXIT_CRITICAL(&s_lock);
}

void fls2_display_diag_report(void)
{
    portENTER_CRITICAL(&s_lock);
    bool panel = s_panel != NULL, bars = s_bars;
    uint32_t ok = s_draw_ok, errors = s_draw_errors;
    esp_err_t last = s_last_draw_error;
    portEXIT_CRITICAL(&s_lock);
    char line[224];
    snprintf(line, sizeof(line),
             "FLS2:DISPLAY panel_created=%d pattern=%s draw_ok=%lu draw_err=%lu "
             "last=%s panel_reception=NOT_VERIFIED\n",
             panel, bars ? "HARDWARE_BARS" : "NORMAL",
             (unsigned long)ok, (unsigned long)errors, esp_err_to_name(last));
    (void)bsp_usb_cdc_write(line);
}

void fls2_display_diag_service(void)
{
    portENTER_CRITICAL(&s_lock);
    bool cancel = s_cancel_colors;
    portEXIT_CRITICAL(&s_lock);
    /* Keep pending commands until bounded cleanup succeeds. Never leave a
       hidden temporary LVGL screen alive when stopping/resuming the loop. */
    if (cancel) {
        if (!stop_colors()) return;
        portENTER_CRITICAL(&s_lock);
        s_cancel_colors = false;
        portEXIT_CRITICAL(&s_lock);
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    int req = s_request;
    s_request = 0;
    esp_lcd_panel_handle_t panel = s_panel;
    bool expired = s_bars && now >= s_restore_at_us;
    bool hardware = s_hardware_request, probe = s_probe_request, colors = s_colors_request;
    bool frame=s_frame_request, redraw=s_redraw_request;
    s_frame_request=s_redraw_request=false;
    s_hardware_request = s_probe_request = s_colors_request = false;
    portEXIT_CRITICAL(&s_lock);
    if (hardware) report_hardware();
    if (redraw) redraw_frame();
    if (frame) report_framebuffer(panel);
    if (probe) probe_registers();
    service_colors(colors);
    if (!req && !expired) return;
    bool bars = req == 1;
    esp_err_t err = panel ? esp_lcd_dpi_panel_set_pattern(
        panel, bars ? MIPI_DSI_PATTERN_BAR_VERTICAL : MIPI_DSI_PATTERN_NONE)
        : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_bars = bars;
        s_restore_at_us = bars ? now + 20000000 : 0;
        portEXIT_CRITICAL(&s_lock);
    }
    char line[192];
    snprintf(line, sizeof(line),
             "FLS2:DISPLAY_TEST requested=%s api=%s auto_restore_s=%d "
             "panel_reception=NOT_VERIFIED\n",
             bars ? "HARDWARE_BARS" : "NORMAL", esp_err_to_name(err), bars ? 20 : 0);
    (void)bsp_usb_cdc_write(line);
}
