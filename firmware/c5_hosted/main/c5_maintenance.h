#pragma once
#include "esp_err.h"
/* Called before Hosted starts; may deliberately reset a pending fault-test image. */
void c5_maintenance_pre_init(void);
esp_err_t c5_maintenance_init(void);
