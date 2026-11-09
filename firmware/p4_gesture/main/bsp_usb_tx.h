#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Private CDC admission; shared by telemetry, replies and mirrored logs. */
esp_err_t bsp_usb_tx_record(const char *text, size_t len, bool session_open);
