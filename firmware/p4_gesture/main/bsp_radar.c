#include "bsp_radar.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "bsp_common.h"

static const char *TAG = "BSP_RADAR";

esp_err_t bsp_radar_init(void)
{
    /* 上电前先固定 SOP 启动模式，再释放复位 */
    bsp_gpio_out(PIN_RADAR_SOP0, 0);
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
