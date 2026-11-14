#pragma once
#include <stddef.h>
#include <stdint.h>

/* Soft original confirmation sound: low descending body, quiet overtone,
 * 2 ms attack and quadratic release reaching exactly zero. Audible midrange
 * harmonics suit the small speaker without raising the system volume. Shares the codec
 * sample rate and volume; this is not a separate loudness setting. */
#define MEO_CLICK_FRAMES (120u * 48u)
typedef struct { size_t position; uint32_t phase; } meo_click_t;
static inline size_t meo_click_render(meo_click_t *state, int16_t *stereo,
                                      size_t frames, const int16_t sine[512])
{
    if (!state || !stereo || !sine || state->position>=MEO_CLICK_FRAMES) return 0;
    size_t left=MEO_CLICK_FRAMES-state->position;
    if (frames>left) frames=left;
    for (size_t i=0;i<frames;++i) {
        unsigned n=(unsigned)state->position++;
        unsigned tail=MEO_CLICK_FRAMES-1-n;
        int attack=n<96 ? (int)n*1024/96 : 1024;
        int release=(int)((uint64_t)tail*tail*1024/((uint64_t)(MEO_CLICK_FRAMES-1)*(MEO_CLICK_FRAMES-1)));
        int amp=6200*attack/1024*release/1024;
        unsigned index=(state->phase>>7)&511u;
        int carrier=(8*(int)sine[index]+3*(int)sine[(index*2)&511u]+(int)sine[(index*3)&511u])/12;
        int sample=(int)((int64_t)carrier*amp/32768);
        stereo[2*i]=stereo[2*i+1]=(int16_t)sample;
        unsigned hz=500-200*n/MEO_CLICK_FRAMES;
        state->phase+=(uint32_t)((uint64_t)hz*65536/48000);
    }
    return frames;
}
