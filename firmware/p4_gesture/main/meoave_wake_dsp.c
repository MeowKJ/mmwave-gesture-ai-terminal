#include "meoave_wake_dsp.h"
#include <string.h>

/* 63-tap Hamming-windowed sinc, fc=6.5 kHz at 48 kHz; Q15 DC sum=32768.
 * Compute only every third output, retaining history/phase across callbacks. */
static const int16_t taps[MEO_WAKE_FIR_TAPS] = {
    25, 11, -14, -36, -38, -7, 45, 83, 67,
    -16, -122, -170, -92, 94, 273, 292, 76, -276,
    -525, -430, 46, 648, 934, 559, -426, -1473, -1762,
    -650, 1890, 5121, 7822, 8870, 7822, 5121, 1890, -650,
    -1762, -1473, -426, 559, 934, 648, 46, -430, -525,
    -276, 76, 292, 273, 94, -92, -170, -122, -16,
    67, 83, 45, -7, -38, -36, -14, 11, 25,
};
void meo_wake_dsp_reset(meo_wake_dsp_t *dsp)
{
    memset(dsp, 0, sizeof(*dsp));
}
bool meo_wake_dsp_sample(meo_wake_dsp_t *dsp, int16_t sample, int16_t *out)
{
    dsp->history[dsp->cursor] = sample;
    dsp->cursor = (dsp->cursor + 1) % MEO_WAKE_FIR_TAPS;
    if (++dsp->phase < 3) return false;
    dsp->phase = 0;
    int64_t acc = 0;
    unsigned p = dsp->cursor;
    for (unsigned i = 0; i < MEO_WAKE_FIR_TAPS; ++i) {
        p = p ? p - 1 : MEO_WAKE_FIR_TAPS - 1;
        acc += (int32_t)dsp->history[p] * taps[i];
    }
    acc = acc >= 0 ? (acc + 16384) / 32768 : (acc - 16384) / 32768;
    if (acc > INT16_MAX) acc = INT16_MAX;
    if (acc < INT16_MIN) acc = INT16_MIN;
    *out = (int16_t)acc;
    return true;
}
