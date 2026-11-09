#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
/* Opaque NEW handle API, not legacy sdmmc_host_t/global slot APIs. */
typedef struct sd_host_driver_t *sd_host_ctlr_handle_t;
typedef struct sd_slot_driver_t *sd_host_slot_handle_t;
typedef enum { SD_BUS_WIDTH_1_BIT = 1, SD_BUS_WIDTH_4_BIT = 4,
               SD_BUS_WIDTH_8_BIT = 8 } sd_bus_width_t;
typedef enum { SD_MODE_NORMAL, SD_MODE_UHS1 } sd_mode_t;
