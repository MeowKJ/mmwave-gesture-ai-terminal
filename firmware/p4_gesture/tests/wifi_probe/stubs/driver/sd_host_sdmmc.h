#pragma once
#include "driver/sd_types.h"
#include "driver/gpio.h"
/* Field types/order and signatures checked against local v6.1 public API. */
typedef struct {
    uint32_t event_queue_items;
    uint32_t dma_desc_num;
} sd_host_sdmmc_cfg_t;
typedef struct {
    sd_bus_width_t width;
    gpio_num_t clk_io, cmd_io, cd_io, wp_io;
    gpio_num_t d0_io, d1_io, d2_io, d3_io, d4_io, d5_io, d6_io, d7_io;
} sd_host_sdmmc_slot_io_cfg_t;
typedef struct {
    int slot_id;
    sd_mode_t sd_mode;
    sd_host_sdmmc_slot_io_cfg_t io_config;
    struct { uint32_t internal_pullup : 1; uint32_t wp_active_high : 1; } slot_flags;
} sd_host_slot_sdmmc_init_cfg_t;
esp_err_t sd_host_create_sdmmc_controller(const sd_host_sdmmc_cfg_t *config,
                                         sd_host_ctlr_handle_t *ret_handle);
esp_err_t sd_host_sdmmc_controller_add_slot(sd_host_ctlr_handle_t ctlr,
                                           const sd_host_slot_sdmmc_init_cfg_t *config,
                                           sd_host_slot_handle_t *ret_handle);
