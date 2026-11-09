#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

/* Deliberately use non-1ms ticks to catch vTaskDelay(10) without conversion. */
#define configTICK_RATE_HZ 500U
#define pdMS_TO_TICKS(ms) \
    ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000U))
