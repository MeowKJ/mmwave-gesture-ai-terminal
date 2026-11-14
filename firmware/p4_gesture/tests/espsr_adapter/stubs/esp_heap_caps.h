#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_INTERNAL 4u
void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
