#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Validate the official pack_model.py layout BEFORE the unbounded SR parser.
 * This is structural validation, not signature/authenticity verification. */
typedef bool (*meo_sr_pack_reader_t)(void *ctx, size_t offset, void *out, size_t size);
typedef enum {
    MEO_SR_PACK_OK, MEO_SR_PACK_EMPTY, MEO_SR_PACK_INVALID, MEO_SR_PACK_IO
} meo_sr_pack_result_t;
meo_sr_pack_result_t meo_sr_pack_validate(meo_sr_pack_reader_t read, void *ctx, size_t size);
