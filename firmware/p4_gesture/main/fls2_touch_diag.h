#pragma once

#include <stdbool.h>

void fls2_touch_diag_init(void);
void fls2_touch_diag_enable(bool enabled);
void fls2_touch_diag_request_id(void); /* One read-only, serialized 0xA7 read. */
void fls2_touch_diag_service(void);
void fls2_touch_diag_report(void);
void fls2_touch_diag_report_compact(void);
