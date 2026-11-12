#pragma once

/* Called once after display/touch setup, with the LVGL lock held. */
void meoave_app_create(void);

/* Nonblocking UI work is separate from the proven native USB shell. */
void meoave_app_service(void);
void meoave_app_request_status(void);
/* Jump to a page by name from the USB shell ("UI GO mic"); applied on the app
 * task under the GUI lock, so it is safe to call from the USB callback. */
void meoave_app_request_page(const char *name);
/* Show/hide the shutdown countdown overlay from the shell (n<0 hides). */
void meoave_app_request_shutdown(int seconds);
/* Frame pacing probe: >0 starts a measurement window of that many ms,
   0 reports the most recent published window over USB. */
void meoave_app_request_fps(int ms);

/* Power key: short press returns to the home menu, a 2 s hold starts the 5 s
 * shutdown countdown, a press during the countdown cancels it. Call every
 * ~20 ms from the app task; it never blocks on the GUI. */
void meoave_app_key_tick(void);
