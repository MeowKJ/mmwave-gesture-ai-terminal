#include "bsp_radar.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "bsp_common.h"

static const char *TAG = "BSP_RADAR";

/* 雷达是 IWRL6432（SWRS298 Table 9-3，AOP 版 SWRS323 Table 8-3）：
 *   SOP1=0 SOP0=1 -> Functional（从 QSPI flash 加载应用）
 *   SOP1=0 SOP0=0 -> Flashing（bootloader 停在循环里等 UniFlash 写 QSPI）
 *   SOP1=1 SOP0=1 -> Debug（绕过 bootloader，M4F 停住）
 * 参考设计默认 SOP0 上拉、SOP1 下拉，即默认功能模式；把 SOP0 接地才进刷写模式。
 *
 * 但这块板的 SOP0 与 C5 的 CHIP_PU 是同一根线（GPIO54），而 CHIP_PU 低有效：
 * 拉低 SOP0 等于在让雷达进刷写模式的同时把 C5 按在复位里。所以正常运行必须
 * 保持 SOP0 高——这既是雷达的功能模式，也是 C5 的 CHIP_PU 释放。只有要刷雷达
 * 固件时才允许拉低，代价是 C5 同时被复位、必须重新走一次连接流程。 */
_Static_assert(PIN_RADAR_SOP0 == PIN_C5_RESET,
               "SOP0 is shared with C5 CHIP_PU; keep both definitions in sync");

esp_err_t bsp_radar_init(void)
{
    /* 上电前先固定 SOP 启动模式，再释放复位 */
    bsp_gpio_out(PIN_RADAR_SOP0, 1);
    bsp_gpio_out(PIN_RADAR_SOP1, 0);
    bsp_gpio_out(PIN_HOST_REQ, 0);
    bsp_gpio_out(PIN_RADAR_NRST, 0);
    bsp_gpio_out(PIN_RADAR_PWR_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_gpio_out(PIN_RADAR_NRST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    const uart_config_t cfg = {
        .baud_rate = RADAR_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* 轮询周期 200ms @115200 ≈ 2304B，RX buffer 必须大于它，否则丢帧 */
    esp_err_t err = uart_driver_install(RADAR_UART_PORT, 4096, 256, 0, NULL, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(RADAR_UART_PORT, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(RADAR_UART_PORT, PIN_RADAR_UART_TX, PIN_RADAR_UART_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "UART%d TX=IO%d RX=IO%d @%d ready (%s)", RADAR_UART_PORT,
             PIN_RADAR_UART_TX, PIN_RADAR_UART_RX, RADAR_UART_BAUD, esp_err_to_name(err));
    return err;
}

void bsp_radar_poll(void)
{
    uint8_t buf[128];
    int n = uart_read_bytes(RADAR_UART_PORT, buf, sizeof(buf), 0);
    if (n > 0) {
        ESP_LOGI(TAG, "radar rx %d bytes", n);
#if CONFIG_APP_RADAR_HEXDUMP
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_INFO);
#endif
    }
}

esp_err_t bsp_radar_send(const uint8_t *data, size_t len)
{
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    int n = uart_write_bytes(RADAR_UART_PORT, data, len);
    return n == (int)len ? ESP_OK : ESP_FAIL;
}
