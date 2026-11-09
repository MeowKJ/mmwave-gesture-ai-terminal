#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef struct { unsigned unused; } portMUX_TYPE;
#define pdTRUE 1
#define pdFALSE 0
#define portMUX_INITIALIZER_UNLOCKED {0}
void fake_enter_critical(portMUX_TYPE *lock);
void fake_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) fake_enter_critical(lock)
#define portEXIT_CRITICAL(lock) fake_exit_critical(lock)
