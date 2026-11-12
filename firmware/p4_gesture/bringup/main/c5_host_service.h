#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* The UART and product USB transports share the exact OTA/RPC state machine.
 * Called by a single worker, never by a TinyUSB callback. */
typedef bool (*meo_c5_host_output_t)(const char *record);
esp_err_t meo_c5_host_init(void);
void meo_c5_host_execute(char *line, meo_c5_host_output_t output);
