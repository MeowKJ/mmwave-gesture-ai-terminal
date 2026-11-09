/*
 * meoave controller board V1.0 - C5 GPIO2 pad guard.
 *
 * The ESP32-P4C5 module wires C5 GPIO2 to P4 GPIO6 (module user guide, figure
 * "Pin mapping between ESP32C5HF4 and ESP32P4"). On this board the P4 side of
 * that net is ES8389 I2S DOUT, a push-pull output running whenever audio plays.
 * Both pads sit on the same copper, so the C5 must never drive GPIO2.
 *
 * This module pins GPIO2 to a pure high-Z state - input mode, internal pull-up
 * and pull-down both disabled, no interrupt - and keeps it there for the whole
 * lifetime of the firmware. It never writes the output latch.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __C5_IO2_GUARD_H__
#define __C5_IO2_GUARD_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board V1.0: C5 GPIO2 <-> P4 GPIO6 (ES8389 I2S DOUT). */
#define C5_IO2_SHARED_GPIO 2

/**
 * @brief   Force C5 GPIO2 into high-Z and keep it there.
 *
 * Idempotent. Call it as the first statement of app_main() - before NVS, before
 * Wi-Fi, before ESP-Hosted - so the pad is safe even if a later init step
 * fails. Also call it again after ESP-Hosted comes up, so a future transport or
 * feature change cannot quietly re-claim the pad.
 *
 * @return  ESP_OK on success
 *          ESP_ERR_INVALID_STATE if the pin is claimed by the host transport
 *          ESP_ERR_INVALID_ARG / ESP_FAIL from the GPIO driver
 */
esp_err_t c5_io2_guard_apply(void);

/**
 * @brief   True once the guard has been applied successfully at least once.
 */
bool c5_io2_guard_is_applied(void);

#ifdef __cplusplus
}
#endif

#endif /* __C5_IO2_GUARD_H__ */
