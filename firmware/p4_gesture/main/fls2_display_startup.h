#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t host_before, phy_before, host_after, phy_after;
    bool prepared;
} fls2_display_startup_state_t;

/* Boot-only, after PHY LDO acquisition and before creating ANY DSI/DPI object.
 * Not a runtime recovery API: it must never touch an active driver's DMA. */
esp_err_t fls2_display_prepare_startup(int reset_gpio);
const fls2_display_startup_state_t *fls2_display_startup_state(void);
