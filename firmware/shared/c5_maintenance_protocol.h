/* Meoave private maintenance protocol v1, carried by ESP-Hosted custom RPC.
 * Wire layout is little endian (both ESP cores); no pointers or native enums.
 * This is not ROM download/strap control. */
#pragma once
#include <stdint.h>
#define MEO_C5_MAINT_MSG 0x4d354301u
#define MEO_C5_MAINT_MAGIC 0x314d3543u
#define MEO_C5_MAINT_VERSION 1u
enum { MEO_C5_INFO=1, MEO_C5_CONFIRM=2, MEO_C5_PIN=3, MEO_C5_REBOOT=4 };
enum { MEO_C5_HIZ=0, MEO_C5_PULLUP=1, MEO_C5_PULLDOWN=2,
       MEO_C5_DRIVE_LOW=3, MEO_C5_DRIVE_HIGH=4 };
enum { MEO_C5_LOCAL_READY=1, MEO_C5_ROLLBACK=2, MEO_C5_PENDING=4,
       MEO_C5_PIN_HIGHZ=8 };
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version, op;
    uint32_t seq, boot_id, arg0, arg1;
    uint8_t elf_sha[32];
} meo_c5_request_t;
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version, op;
    uint32_t seq, error, boot_id, reset_reason, slot_address, boot_address;
    uint32_t ota_state, flags, pin_mode, lease_ms;
    uint8_t elf_sha[32];
    char project[32], app_version[32];
    uint32_t uptime_ms, pin_level;
} meo_c5_response_t;
_Static_assert(sizeof(meo_c5_request_t)==56, "request wire layout");
_Static_assert(sizeof(meo_c5_response_t)==152, "response wire layout");
