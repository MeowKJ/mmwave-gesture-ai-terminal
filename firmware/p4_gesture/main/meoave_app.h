#pragma once

/* Called once after display/touch setup, with the LVGL lock held. */
void meoave_app_create(void);

/* Nonblocking UI work is separate from the proven native USB shell. */
void meoave_app_service(void);
void meoave_app_request_status(void);
