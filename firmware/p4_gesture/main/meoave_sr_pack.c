#include "meoave_sr_pack.h"
#include <string.h>

enum { MAX_MODELS = 8, MAX_FILES = 16, NAME_BYTES = 32 };
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool valid_name(const uint8_t *p)
{
    return p[0] && memchr(p, 0, NAME_BYTES) != NULL;
}

meo_sr_pack_result_t meo_sr_pack_validate(meo_sr_pack_reader_t read, void *ctx, size_t size)
{
    uint8_t header[40];
    struct { uint32_t offset, size; } ranges[MAX_MODELS * MAX_FILES];
    char names[MAX_MODELS][NAME_BYTES];
    unsigned count = 0;
    if (!read || size < 4) return MEO_SR_PACK_INVALID;
    if (!read(ctx, 0, header, 4)) return MEO_SR_PACK_IO;
    uint32_t models = le32(header);
    if (!models) return MEO_SR_PACK_EMPTY;
    if (models > MAX_MODELS) return MEO_SR_PACK_INVALID; /* includes erased 0xffffffff */
    size_t cursor = 4;
    for (uint32_t m = 0; m < models; ++m) {
        if (cursor > size || size - cursor < 36) return MEO_SR_PACK_INVALID;
        if (!read(ctx, cursor, header, 36)) return MEO_SR_PACK_IO;
        if (!valid_name(header)) return MEO_SR_PACK_INVALID;
        for (uint32_t n = 0; n < m; ++n)
            if (!strcmp(names[n], (const char *)header)) return MEO_SR_PACK_INVALID;
        memcpy(names[m], header, NAME_BYTES);
        uint32_t files = le32(header + NAME_BYTES);
        if (!files || files > MAX_FILES) return MEO_SR_PACK_INVALID;
        cursor += 36;
        bool metadata = false;
        char file_names[MAX_FILES][NAME_BYTES];
        for (uint32_t f = 0; f < files; ++f) {
            if (cursor > size || size - cursor < 40) return MEO_SR_PACK_INVALID;
            if (!read(ctx, cursor, header, 40)) return MEO_SR_PACK_IO;
            if (!valid_name(header)) return MEO_SR_PACK_INVALID;
            for (uint32_t n = 0; n < f; ++n)
                if (!strcmp(file_names[n], (const char *)header)) return MEO_SR_PACK_INVALID;
            memcpy(file_names[f], header, NAME_BYTES);
            uint32_t offset = le32(header + 32), bytes = le32(header + 36);
            if (!bytes || offset > size || bytes > size - offset) return MEO_SR_PACK_INVALID;
            if (!strcmp((const char *)header, "_MODEL_INFO_")) {
                if (bytes > 1024) return MEO_SR_PACK_INVALID;
                metadata = true;
            }
            ranges[count].offset = offset;
            ranges[count++].size = bytes;
            cursor += 40;
        }
        if (!metadata) return MEO_SR_PACK_INVALID;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (ranges[i].offset < cursor) return MEO_SR_PACK_INVALID;
        for (unsigned j = 0; j < i; ++j)
            if ((uint64_t)ranges[i].offset < (uint64_t)ranges[j].offset + ranges[j].size &&
                (uint64_t)ranges[j].offset < (uint64_t)ranges[i].offset + ranges[i].size)
                return MEO_SR_PACK_INVALID;
    }
    return MEO_SR_PACK_OK;
}
