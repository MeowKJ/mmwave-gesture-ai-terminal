#pragma once
#include <stddef.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
typedef struct { size_t size; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t size);
