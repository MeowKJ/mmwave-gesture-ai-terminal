/* P4 ESP-SR 2.6.2 compatibility: vector buffers must not land in LP/SPM RAM.
 * Keep the public dl_lib allocator/free ABI; do not wrap the system malloc.
 * The linked archive and its prefix ABI are pinned in SrSimdGuard.cmake. */
#include "sdkconfig.h"

#if defined(MEOAVE_ESPSR_LINKED) && MEOAVE_ESPSR_LINKED && CONFIG_IDF_TARGET_ESP32P4
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "dl_lib.h"

static void *sr_calloc(int count, int size, int alignment, bool prefer_psram)
{
    if (count <= 0 || size <= 0 || alignment < 0) return NULL;
    size_t align = alignment < 16 ? 16u : (size_t)alignment;
    if ((alignment && (alignment & (alignment - 1))) ||
        (size_t)count > SIZE_MAX / (size_t)size) return NULL;
    size_t bytes = (size_t)count * (size_t)size;
    size_t padding = sizeof(void *) + align - 1;
    if (bytes > SIZE_MAX - padding) return NULL;
    size_t total = bytes + padding;
    unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_SIMD;
    unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_SIMD;
    /* Match IDF's default small/large allocation preference, but never fall
       back to a region incapable of 128-bit loads/stores. */
    prefer_psram |= total > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
    void *raw = heap_caps_calloc(1, total, prefer_psram ? external : internal);
    if (!raw) raw = heap_caps_calloc(1, total, prefer_psram ? internal : external);
    if (!raw) return NULL;
    uintptr_t aligned = ((uintptr_t)raw + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    /* Official dl_lib_free reads the original allocation one pointer before
       the aligned result. Preserve this ABI, including zero-filled payload. */
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

void *__wrap_dl_lib_calloc(int count, int size, int alignment)
{
    return sr_calloc(count, size, alignment, false);
}

void *__wrap_dl_lib_calloc_psram(int count, int size, int alignment)
{
    return sr_calloc(count, size, alignment, true);
}
#endif
