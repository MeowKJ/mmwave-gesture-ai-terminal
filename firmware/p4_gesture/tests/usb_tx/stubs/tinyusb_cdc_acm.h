#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Signatures match managed_components/espressif__esp_tinyusb/include/
 * tinyusb_cdc_acm.h. Queue success is a size_t byte count, not esp_err_t. */
typedef enum {
    TINYUSB_CDC_ACM_0 = 0,
    TINYUSB_CDC_ACM_1,
    TINYUSB_CDC_ACM_MAX
} tinyusb_cdcacm_itf_t;

size_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t itf,
                                const uint8_t *in_buf, size_t in_size);
esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t itf,
                                  uint32_t timeout_ticks);
