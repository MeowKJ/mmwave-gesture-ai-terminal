#pragma once

#include "driver/gpio.h"

/* 简单的 GPIO 输出/输入配置，供各 BSP 模块共用 */
void bsp_gpio_out(gpio_num_t pin, int level);
void bsp_gpio_in(gpio_num_t pin, bool pull_up);
int  bsp_gpio_read(gpio_num_t pin);
