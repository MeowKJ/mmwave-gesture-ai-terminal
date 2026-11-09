/*
 * meoave controller board V1.0 - C5 GPIO2 pad guard.
 *
 * See c5_io2_guard.h for the why. This is the only file in the firmware that is
 * allowed to touch C5 GPIO2, and it only ever configures the pad as an input.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "c5_io2_guard.h"

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "slave_transport_gpio_pin_guard.h"

static const char *TAG = "c5_io2_guard";

static bool s_applied;

#if defined(CONFIG_C5_IO2_GUARD_ENABLE) && defined(CONFIG_C5_IO2_GUARD_ASSERT_SDIO)

/* Compile-time proof that no transport pin landed on the shared audio net.
 * If one of these fires, someone edited the SDIO/SPI/UART pin map into GPIO2 -
 * fix the pin map, never the assertion. */

#if defined(CONFIG_ESP_SDIO_HOST_INTERFACE)
_Static_assert(CONFIG_ESP_SDIO_PIN_CMD != C5_IO2_SHARED_GPIO, "SDIO CMD must not be C5 GPIO2");
_Static_assert(CONFIG_ESP_SDIO_PIN_CLK != C5_IO2_SHARED_GPIO, "SDIO CLK must not be C5 GPIO2");
_Static_assert(CONFIG_ESP_SDIO_PIN_D0  != C5_IO2_SHARED_GPIO, "SDIO D0 must not be C5 GPIO2");
_Static_assert(CONFIG_ESP_SDIO_PIN_D1  != C5_IO2_SHARED_GPIO, "SDIO D1 must not be C5 GPIO2");
_Static_assert(CONFIG_ESP_SDIO_PIN_D2  != C5_IO2_SHARED_GPIO, "SDIO D2 must not be C5 GPIO2");
_Static_assert(CONFIG_ESP_SDIO_PIN_D3  != C5_IO2_SHARED_GPIO, "SDIO D3 must not be C5 GPIO2");
#endif

#if defined(CONFIG_ESP_SPI_HOST_INTERFACE)
#if defined(CONFIG_ESP_SPI_GPIO_MOSI)
_Static_assert(CONFIG_ESP_SPI_GPIO_MOSI != C5_IO2_SHARED_GPIO, "SPI MOSI must not be C5 GPIO2");
#endif
#if defined(CONFIG_ESP_SPI_GPIO_MISO)
_Static_assert(CONFIG_ESP_SPI_GPIO_MISO != C5_IO2_SHARED_GPIO, "SPI MISO must not be C5 GPIO2");
#endif
#if defined(CONFIG_ESP_SPI_GPIO_CLK)
_Static_assert(CONFIG_ESP_SPI_GPIO_CLK != C5_IO2_SHARED_GPIO, "SPI CLK must not be C5 GPIO2");
#endif
#if defined(CONFIG_ESP_SPI_GPIO_CS)
_Static_assert(CONFIG_ESP_SPI_GPIO_CS != C5_IO2_SHARED_GPIO, "SPI CS must not be C5 GPIO2");
#endif
#if defined(CONFIG_ESP_SPI_GPIO_HANDSHAKE)
_Static_assert(CONFIG_ESP_SPI_GPIO_HANDSHAKE != C5_IO2_SHARED_GPIO, "SPI handshake must not be C5 GPIO2");
#endif
#if defined(CONFIG_ESP_SPI_GPIO_DATA_READY)
_Static_assert(CONFIG_ESP_SPI_GPIO_DATA_READY != C5_IO2_SHARED_GPIO, "SPI data-ready must not be C5 GPIO2");
#endif
#endif /* CONFIG_ESP_SPI_HOST_INTERFACE */

#if defined(CONFIG_ESP_UART_HOST_INTERFACE)
#if defined(CONFIG_ESP_UART_PIN_TX)
_Static_assert(CONFIG_ESP_UART_PIN_TX != C5_IO2_SHARED_GPIO, "UART TX must not be C5 GPIO2");
#endif
#if defined(CONFIG_ESP_UART_PIN_RX)
_Static_assert(CONFIG_ESP_UART_PIN_RX != C5_IO2_SHARED_GPIO, "UART RX must not be C5 GPIO2");
#endif
#endif /* CONFIG_ESP_UART_HOST_INTERFACE */

#endif /* CONFIG_C5_IO2_GUARD_ENABLE && CONFIG_C5_IO2_GUARD_ASSERT_SDIO */

esp_err_t c5_io2_guard_apply(void)
{
#ifdef CONFIG_C5_IO2_GUARD_ENABLE
	/* Defensive: if the transport ever claims this pad, touching it would fight
	 * the bus driver. Report and leave the pad alone instead. */
	if (!transport_gpio_pin_guard_is_eligible((gpio_num_t)C5_IO2_SHARED_GPIO)) {
		ESP_LOGE(TAG, "GPIO%d is claimed by the host transport - refusing to reconfigure it",
			 C5_IO2_SHARED_GPIO);
		return ESP_ERR_INVALID_STATE;
	}

	const gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << C5_IO2_SHARED_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};

	esp_err_t err = gpio_config(&cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "gpio_config(GPIO%d) failed: %s", C5_IO2_SHARED_GPIO, esp_err_to_name(err));
		return err;
	}

	/* No gpio_set_level() here on purpose: the output latch is left untouched, so
	 * even a stray set_direction(OUTPUT) elsewhere would not inherit a stale level. */
	const int level = gpio_get_level((gpio_num_t)C5_IO2_SHARED_GPIO);

	if (!s_applied) {
		ESP_LOGI(TAG, "GPIO%d pinned high-Z (input, no pull) - shared with P4 I2S DOUT, level now %d",
			 C5_IO2_SHARED_GPIO, level);
	} else {
		ESP_LOGD(TAG, "GPIO%d guard re-applied, level %d", C5_IO2_SHARED_GPIO, level);
	}
	s_applied = true;
	return ESP_OK;
#else
	ESP_LOGW(TAG, "CONFIG_C5_IO2_GUARD_ENABLE is off - GPIO%d is NOT protected",
		 C5_IO2_SHARED_GPIO);
	return ESP_OK;
#endif
}

bool c5_io2_guard_is_applied(void)
{
	return s_applied;
}
