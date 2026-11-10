/* Meoave local, explicit pre-connect SDIO diagnostics. Not an upstream API. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Configure once per boot, before the first card operation. Does not touch
 * GPIOs or reset either processor. CONNECT uses one explicit C5 reset and one
 * card-init attempt, then waits at most 4 seconds for the Hosted init event.
 * Caller must ensure radar is inactive (GPIO54 is shared with radar SOP0). */
esp_err_t esp_hosted_sdio_diagnostic_configure(unsigned width, unsigned khz);
esp_err_t esp_hosted_sdio_diagnostic_configure_ex(unsigned width, unsigned khz,
                                                unsigned poll_ms, unsigned init_ms);
bool esp_hosted_sdio_diagnostic_active(void);
/* Pre-card-init only. ATTACH after OTA must NOT reset a pending candidate. */
esp_err_t esp_hosted_sdio_diagnostic_set_reset(bool enabled);
bool esp_hosted_sdio_diagnostic_reset_enabled(void);
unsigned esp_hosted_sdio_diagnostic_poll_ms(void);
unsigned esp_hosted_sdio_diagnostic_init_ms(void);
void esp_hosted_sdio_diagnostic_snapshot(void);
