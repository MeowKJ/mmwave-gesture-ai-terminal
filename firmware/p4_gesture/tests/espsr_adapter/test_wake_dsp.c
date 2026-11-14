#include "meoave_wake_dsp.h"
#include "meoave_sr_pack.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t pack[256];
static bool read_pack(void *ctx, size_t offset, void *out, size_t size)
{
    (void)ctx;
    assert(offset <= sizeof(pack) && size <= sizeof(pack) - offset);
    memcpy(out, pack + offset, size); return true;
}
static void le32(uint8_t *p, uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i)); }
static void make_pack(void)
{
    memset(pack, 0, sizeof(pack));
    le32(pack, 1); memcpy(pack + 4, "wn9_maomao", 11); le32(pack + 36, 3);
    const char *names[] = {"_MODEL_INFO_", "wn9_index", "wn9_data"};
    unsigned offsets[] = {160,201,209}, sizes[] = {41,8,32};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned p = 40 + i * 40;
        memcpy(pack + p, names[i], strlen(names[i]));
        le32(pack + p + 32, offsets[i]); le32(pack + p + 36, sizes[i]);
    }
}
static void test_pack(void)
{
    make_pack(); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_OK);
    memset(pack, 0xff, sizeof(pack)); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    memset(pack, 0, sizeof(pack)); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_EMPTY);
    make_pack(); assert(meo_sr_pack_validate(read_pack, NULL, 3) == MEO_SR_PACK_INVALID);
    assert(meo_sr_pack_validate(read_pack, NULL, 159) == MEO_SR_PACK_INVALID);
    make_pack(); memset(pack + 4, 'x', 32); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    make_pack(); le32(pack + 36, 0xfffffff0); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    make_pack(); le32(pack + 72, 0xfffffff0); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    make_pack(); le32(pack + 76, 0xffffffff); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    make_pack(); le32(pack + 72, 40); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    make_pack(); le32(pack + 112, 170); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    make_pack(); memcpy(pack + 40, "wrong_name__", 13); assert(meo_sr_pack_validate(read_pack, NULL, sizeof(pack)) == MEO_SR_PACK_INVALID);
    /* Arbitrary corrupted headers must not cause an out-of-bounds read. */
    uint32_t seed = 1;
    for (unsigned n = 0; n < 10000; ++n) {
        make_pack();
        seed = seed * 1664525u + 1013904223u;
        unsigned pos = seed % 160;
        seed = seed * 1664525u + 1013904223u;
        pack[pos] = (uint8_t)(seed >> 24);
        (void)meo_sr_pack_validate(read_pack, NULL, sizeof(pack));
    }
}
static double response(double hz)
{
    meo_wake_dsp_t dsp; meo_wake_dsp_reset(&dsp);
    double sum = 0; unsigned count = 0;
    for (unsigned i = 0; i < 48000; ++i) {
        int16_t out;
        int16_t input = (int16_t)(10000.0 * sin(6.283185307179586 * hz * i / 48000));
        if (meo_wake_dsp_sample(&dsp, input, &out) && i > 300) {
            sum += (double)out * out; ++count;
        }
    }
    return sqrt(sum / count);
}
static void test_dsp(void)
{
    int16_t source[4801], a[1601], b[1601];
    for (unsigned i = 0; i < 4801; ++i) source[i] = (int16_t)((i * 97) % 24000 - 12000);
    meo_wake_dsp_t dsp; meo_wake_dsp_reset(&dsp);
    unsigned na = 0, nb = 0;
    for (unsigned i = 0; i < 4801; ++i) {
        int16_t out; if (meo_wake_dsp_sample(&dsp, source[i], &out)) a[na++] = out;
    }
    meo_wake_dsp_reset(&dsp);
    unsigned cursor = 0;
    const unsigned blocks[] = {1,2,477,13,5};
    for (unsigned n = 0; cursor < 4801; ++n) {
        unsigned block = blocks[n % 5];
        while (block-- && cursor < 4801) {
            int16_t out;
            if (meo_wake_dsp_sample(&dsp, source[cursor++], &out)) b[nb++] = out;
        }
    }
    assert(na == 1600 && nb == na && !memcmp(a, b, na * sizeof(a[0])));
    assert(response(1000) > 6900 && response(1000) < 7200);
    assert(response(12000) < 35); /* strong anti-aliasing, unlike 3-point averaging */
    meo_wake_dsp_reset(&dsp);
    for (unsigned i = 0; i < 300; ++i) {
        int16_t out;
        if (meo_wake_dsp_sample(&dsp, INT16_MAX, &out) && i > 100) assert(out == INT16_MAX);
    }
    meo_wake_dsp_reset(&dsp);
    for (unsigned i = 0; i < 300; ++i) {
        int16_t out;
        if (meo_wake_dsp_sample(&dsp, 0, &out)) assert(out == 0);
    }
}
int main(void)
{
    test_pack(); test_dsp();
    puts("PASS: 48/16 kHz FIR count, irregular boundaries, response, reset; bounded model parser and 10000 corruptions");
    return 0;
}
