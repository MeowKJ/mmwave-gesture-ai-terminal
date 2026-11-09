#pragma once
#include <stdbool.h>

/* Explicit C5 CHIP_PU / GPIO54 diagnostic for the OSPTEK P4C5 module.
 *
 * GPIO54 is shared between the C5 CHIP_PU (active low) and the radar SOP0
 * input. The product image never configures this pin, so the C5 reset line is
 * left at its floating default. This module measures that state and offers a
 * HIGH-only release action so the reset hypothesis can be tested.
 *
 * Pulling the line LOW is refused by design: it would assert C5 reset and
 * change radar SOP0 at the same time, and the shared-pin power/reset sequence
 * is not verified yet (see PROJECT_PLAN.md section 3).
 *
 * GPIO6 (audio I2S DOUT) is never touched here.
 */
bool bsp_c5_diag_command(const char *line);
