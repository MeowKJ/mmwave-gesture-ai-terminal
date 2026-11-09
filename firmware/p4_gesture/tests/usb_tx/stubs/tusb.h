#pragma once

#include <stdbool.h>
#include <stdint.h>

bool tud_mounted(void);
uint32_t tud_cdc_n_write_available(uint8_t itf);
