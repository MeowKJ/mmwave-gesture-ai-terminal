#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Only the fields used by the production helper are represented. These are
 * host-memory bitfields, not a description of a physical peripheral layout. */
typedef union {
    uint32_t val;
    struct {
        uint32_t shutdownz : 1;
        uint32_t reserved : 31;
    };
} mock_dsi_power_reg_t;

typedef union {
    uint32_t val;
    struct {
        uint32_t phy_shutdownz : 1;
        uint32_t reserved : 31;
    };
} mock_dsi_phy_reg_t;

typedef struct {
    mock_dsi_power_reg_t pwr_up;
    mock_dsi_phy_reg_t phy_rstz;
    /* Unrelated host-memory canaries, deliberately not hardware registers. */
    uint32_t unrelated_canaries[4];
} dsi_host_dev_t;

dsi_host_dev_t *mock_mipi_dsi_get_host(int id);
#define MIPI_DSI_LL_GET_HOST(id) mock_mipi_dsi_get_host(id)

void mipi_dsi_ll_enable_bus_clock(int id, bool enable);
void mipi_dsi_ll_enable_dpi_clock(int id, bool enable);
void mipi_dsi_host_ll_power_on_off(dsi_host_dev_t *host, bool on);
void mipi_dsi_phy_ll_power_on_off(dsi_host_dev_t *host, bool on);

/* No VCI, board power, DMA, reset-peripheral, or other driver APIs are exposed.
 * Unexpected external calls must fail compilation/linking, never become no-ops. */
