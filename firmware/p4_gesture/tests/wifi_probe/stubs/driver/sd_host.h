#pragma once
#include "driver/sd_types.h"
#include "sd_protocol_types.h"
esp_err_t sd_host_slot_do_transaction(sd_host_slot_handle_t slot,
                                     sdmmc_command_t *cmdinfo);
esp_err_t sd_host_remove_slot(sd_host_slot_handle_t slot);
esp_err_t sd_host_del_controller(sd_host_ctlr_handle_t ctlr);
