#pragma once
#include "esp_err.h"

/* Headless maintenance only. Commands contain secrets: never echo the input.
 * Requires a connected, verified Hosted transport. Credentials stay in RAM. */
esp_err_t wifi_diagnostic_command(char *line);
