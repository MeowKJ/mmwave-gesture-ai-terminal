#pragma once
#include "FreeRTOS.h"
typedef struct fake_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
