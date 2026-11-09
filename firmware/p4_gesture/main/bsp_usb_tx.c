#include "bsp_usb_tx.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#if CONFIG_APP_USB_HID_ENABLE
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

static volatile int s_in_tx;
static bool s_needs_boundary;

/* The consumer only frees capacity. With all producers behind this gate,
 * a record admitted in full cannot be displaced by another producer.
 * No mutex wait or blocking flush is allowed from TinyUSB callbacks. */
esp_err_t bsp_usb_tx_record(const char *text, size_t len, bool session_open)
{
    if (!text || len == 0 || len > CONFIG_TINYUSB_CDC_TX_BUFSIZE || text[len - 1] != '\n') {
        return ESP_ERR_INVALID_ARG;
    }
    if (xPortInIsrContext() || !session_open || !tud_mounted()) return ESP_ERR_INVALID_STATE;
    if (__sync_lock_test_and_set(&s_in_tx, 1) != 0) return ESP_ERR_NOT_FINISHED;
    esp_err_t result = ESP_ERR_NO_MEM;
    /* Disconnect can invalidate admission mid-write. Terminate an unexpected
     * short record before admitting any subsequent machine-readable record. */
    if (s_needs_boundary) {
        if (tud_cdc_n_write_available(TINYUSB_CDC_ACM_0) < 1 ||
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)"\n", 1) != 1) {
            goto done;
        }
        s_needs_boundary = false;
    }
    if (tud_cdc_n_write_available(TINYUSB_CDC_ACM_0) >= len) {
        size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)text, len);
        s_needs_boundary = queued != 0 && queued != len;
        result = queued == len ? ESP_OK : ESP_FAIL;
    }
done:
    /* write_queue returns a byte count, not esp_err_t. Always drain previously
     * admitted records, even if no new record fits. ESP_OK means queued only. */
    (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    __sync_lock_release(&s_in_tx);
    return result;
}
#else
esp_err_t bsp_usb_tx_record(const char *text, size_t len, bool session_open)
{
    (void)text; (void)len; (void)session_open;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
