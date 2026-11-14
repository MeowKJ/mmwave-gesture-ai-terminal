#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
#define pdPASS 1
BaseType_t xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t handle);
