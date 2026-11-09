#pragma once
#include "esp_err.h"
/* Only the permitted GPIO operation is declared. Other operations must fail
 * compilation/linking, rather than silently becoming successful no-ops. */
typedef enum {
    GPIO_NUM_NC = -1,
    GPIO_NUM_14 = 14,
    GPIO_NUM_18 = 18,
    GPIO_NUM_19 = 19
} gpio_num_t;
esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
