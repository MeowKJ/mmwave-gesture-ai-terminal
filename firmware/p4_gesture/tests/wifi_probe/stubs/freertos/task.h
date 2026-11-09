#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef struct wifi_test_task *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       configSTACK_DEPTH_TYPE stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *ret_handle);
void vTaskDelete(TaskHandle_t task);
