#pragma once

#include "esp_err.h"

/* Evaluate each expression once and preserve the original error. */
#define ESP_RETURN_ON_ERROR(expression, tag, ...) \
    do { \
        const esp_err_t mock_error = (expression); \
        if (mock_error != ESP_OK) { \
            return mock_error; \
        } \
    } while (0)

#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) \
    do { \
        if (!(condition)) { \
            return (error); \
        } \
    } while (0)
