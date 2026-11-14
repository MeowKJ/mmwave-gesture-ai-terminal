#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef uint8_t StackType_t;
typedef struct { unsigned unused; } StaticTask_t;
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned stack,
                             void *arg, unsigned priority, StackType_t *buffer, StaticTask_t *control);
void vTaskSuspend(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
