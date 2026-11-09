#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

/* Synthetic validity policy, not a claim about physical GPIO availability.
 * It rejects out-of-range shifts and one configurable in-range output. */
bool mock_gpio_is_valid_output(int gpio);
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio) mock_gpio_is_valid_output(gpio)

esp_err_t gpio_set_level(int gpio, uint32_t level);
esp_err_t gpio_config(const gpio_config_t *config);
