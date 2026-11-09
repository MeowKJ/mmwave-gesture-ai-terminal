#include "fls2_display_startup.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_private/periph_ctrl.h"
#include "hal/mipi_dsi_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static fls2_display_startup_state_t s_state;

const fls2_display_startup_state_t *fls2_display_startup_state(void)
{
    return &s_state;
}

esp_err_t fls2_display_prepare_startup(int reset_gpio)
{
    ESP_RETURN_ON_FALSE(!s_state.prepared, ESP_ERR_INVALID_STATE,
                        "FLS2_START", "boot preparation already done");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(reset_gpio), ESP_ERR_INVALID_ARG,
                        "FLS2_START", "invalid reset output");
    /* GPIO12 is the panel's reset, NOT the board's power latch. Preload low
       before enabling output. Keep the receiver in reset while rebaselining
       its host; the existing CO5300 reset/init releases it afterwards. */
    ESP_RETURN_ON_ERROR(gpio_set_level(reset_gpio, 0), "FLS2_START", "reset latch");
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << reset_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), "FLS2_START", "reset output");

    /* IDF 6.1 esp_restart() aborts DW_GDMA before CPU reset, but leaves DSI
       running. new_dsi_bus() resets the BRIDGE registers, while HAL init only
       sets host shutdownz to 1 (no preceding 0). A cold boot and a CPU-only
       warm boot therefore don't have the same host starting state.
       This executes ONLY before any new driver/ISR/DMA exists. Never reset
       shared DMA or change the proven lane/pixel clocks to hide the fault. */
    PERIPH_RCC_ATOMIC() {
        mipi_dsi_ll_enable_bus_clock(0, true);
        mipi_dsi_ll_enable_dpi_clock(0, false);
    }
    dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(0);
    s_state.host_before = host->pwr_up.val;
    s_state.phy_before = host->phy_rstz.val;
    mipi_dsi_host_ll_power_on_off(host, false);
    mipi_dsi_phy_ll_power_on_off(host, false);
    s_state.host_after = host->pwr_up.val;
    s_state.phy_after = host->phy_rstz.val;
    ESP_RETURN_ON_FALSE(!host->pwr_up.shutdownz && !host->phy_rstz.phy_shutdownz,
                        ESP_ERR_INVALID_STATE, "FLS2_START", "host/PHY did not stop");
    vTaskDelay(pdMS_TO_TICKS(10));
    s_state.prepared = true;
    return ESP_OK;
}
