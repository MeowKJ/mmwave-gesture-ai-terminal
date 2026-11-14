#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Streaming 48 -> 16 kHz decimator. Owned only by the inference worker. */
#define MEO_WAKE_FIR_TAPS 63
typedef struct {
    int16_t history[MEO_WAKE_FIR_TAPS];
    unsigned cursor, phase;
} meo_wake_dsp_t;
void meo_wake_dsp_reset(meo_wake_dsp_t *dsp);
bool meo_wake_dsp_sample(meo_wake_dsp_t *dsp, int16_t sample, int16_t *out);
