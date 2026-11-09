#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Complete sdmmc_command_t field list, ordering and flag values checked
 * against local v6.1 components/sdmmc/include/sd_protocol_types.h. This is
 * a host-layout simulation, not an assertion of target ABI compatibility. */
typedef uint32_t sdmmc_response_t[4];
typedef struct {
    uint32_t opcode;
    uint32_t arg;
    sdmmc_response_t response;
    void *data;
    size_t datalen;
    size_t buflen;
    size_t blklen;
    int flags;
    esp_err_t error;
    uint32_t timeout_ms;
    esp_err_t (*volt_switch_cb)(void *, int);
    void *volt_switch_cb_arg;
} sdmmc_command_t;
#define SCF_CMD_BCR 0x0030
#define SCF_RSP_PRESENT 0x1000
#define SCF_RSP_R4 SCF_RSP_PRESENT
