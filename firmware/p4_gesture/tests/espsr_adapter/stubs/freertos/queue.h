#pragma once
#include "freertos/FreeRTOS.h"
#ifndef portMAX_DELAY
#define portMAX_DELAY UINT32_MAX
#endif
typedef struct fake_sr_queue *QueueHandle_t;
typedef struct { unsigned unused; } StaticQueue_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t n, UBaseType_t size, uint8_t *storage, StaticQueue_t *control);
void vQueueDelete(QueueHandle_t queue);
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
