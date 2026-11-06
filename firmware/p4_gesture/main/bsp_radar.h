#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 毫米波雷达：RADAR_3V3、NRST、UART1 115200、HOST_REQ */
esp_err_t bsp_radar_init(void);
void bsp_radar_poll(void);
esp_err_t bsp_radar_send(const uint8_t *data, size_t len);
