#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Application task only, after display readiness; never changes panel timing. */
esp_err_t fls2_display_set_brightness(uint8_t percent);

void fls2_display_diag_request(bool bars);
void fls2_display_diag_service(void);
void fls2_display_diag_report(void);
void fls2_display_diag_request_hardware(void);
/* Read the actual DPI framebuffer; no DCS reads or test-pattern changes. */
void fls2_display_diag_request_frame(void);
/* Explicit diagnostic redraw only; does not reset/reconfigure the panel. */
void fls2_display_diag_request_redraw(void);
void fls2_display_diag_request_probe(void);
void fls2_display_diag_request_colors(void);
void fls2_display_diag_cancel_colors(void);
/* Called only from the same app task that runs service(). */
bool fls2_display_diag_colors_active(void);
bool fls2_display_diag_needs_restart(void);
