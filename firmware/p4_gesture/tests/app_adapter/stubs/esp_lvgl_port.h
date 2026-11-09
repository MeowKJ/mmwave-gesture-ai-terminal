#pragma once
#include <stdbool.h>
#include <stdint.h>
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
