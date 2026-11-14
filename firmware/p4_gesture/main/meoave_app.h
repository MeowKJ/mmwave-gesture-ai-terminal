#pragma once
#include <stdbool.h>

/* Called once after display/touch setup, with the LVGL lock held. */
void meoave_app_create(void);

/* Nonblocking UI work is separate from the proven native USB shell. */
void meoave_app_service(void);
void meoave_app_request_status(void);
/* Queue the same current-volume melody as the speaker page button. */
bool meoave_app_request_preview(void);
/* Queue a short UI confirmation sound. It is dropped if audio is busy. */
bool meoave_app_request_interaction_sound(void);
/* Queued engineering control follows the UI setting and NVS save path. */
void meoave_app_request_volume(int percent);
/* Jump to a page by name from the USB shell ("UI GO mic"); applied on the app
 * task under the GUI lock, so it is safe to call from the USB callback. */
void meoave_app_request_page(const char *name);
/* Show/hide the shutdown countdown overlay from the shell (n<0 hides). */
void meoave_app_request_shutdown(int seconds);
/* Frame pacing probe: >0 starts a measurement window of that many ms,
   0 reports the most recent published window over USB. */
void meoave_app_request_fps(int ms);
/* Offline vertical slice for validating the future voice transport seam. */
bool meoave_app_request_ai_demo(void);
bool meoave_app_request_voice_demo(void);
bool meoave_app_request_ai_cancel(void);
void meoave_app_request_ai_status(void);
/* Queue a recognized phrase from a future C5/server ASR bridge. The current
 * device-side validation accepts the exact configured wake phrase only. */
bool meoave_app_request_wake_text(const char *text);

/* Power key: short press returns to the AI dial; a 1.5 s hold starts the 3 s
 * shutdown countdown. Releasing cancels it. Call every
 * ~20 ms from the app task; it never blocks on the GUI. */
void meoave_app_key_tick(void);
