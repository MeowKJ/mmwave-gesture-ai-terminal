#pragma once

#include <stdbool.h>

void fls2_display_diag_request(bool bars);
void fls2_display_diag_service(void);
void fls2_display_diag_report(void);
void fls2_display_diag_request_hardware(void);
void fls2_display_diag_request_probe(void);
void fls2_display_diag_request_colors(void);
void fls2_display_diag_cancel_colors(void);
/* Called only from the same app task that runs service(). */
bool fls2_display_diag_colors_active(void);
bool fls2_display_diag_needs_restart(void);
