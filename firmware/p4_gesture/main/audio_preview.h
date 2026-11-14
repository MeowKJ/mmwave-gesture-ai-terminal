#pragma once
#include <stddef.h>
#include <stdint.h>
#include "assets/audio/preview_pcm.inc"

/* Real CC0 pizzicato music; source, license and hashes are vendored alongside
 * the PCM. Linear 24 -> 48 kHz interpolation, mono-compatible stereo, no heap
 * allocation. Asset peak is 22000 (~3.5 dB sample headroom), not the old
 * overly conservative 7000. The user's codec level owns volume and mute. */
typedef struct { size_t position; } meo_preview_t;
#define MEO_PREVIEW_MUSIC_FRAMES (MEO_PREVIEW_PCM_FRAMES * 2u)
#define MEO_PREVIEW_TOTAL_FRAMES (MEO_PREVIEW_MUSIC_FRAMES + 4800u)

static inline size_t meo_preview_render(meo_preview_t *state, int16_t *stereo, size_t frames)
{
    if (!state || !stereo || state->position>=MEO_PREVIEW_TOTAL_FRAMES) return 0;
    size_t left=MEO_PREVIEW_TOTAL_FRAMES-state->position;
    if (frames>left) frames=left;
    for (size_t i=0;i<frames;++i) {
        size_t n=state->position++;
        int sample=0;
        if (n<MEO_PREVIEW_MUSIC_FRAMES) {
            size_t at=n/2;
            sample=meo_preview_pcm[at];
            if (n & 1u) {
                int next=at+1<MEO_PREVIEW_PCM_FRAMES ? meo_preview_pcm[at+1] : 0;
                sample=(sample+next)/2;
            }
        }
        stereo[2*i]=stereo[2*i+1]=(int16_t)sample;
    }
    return frames;
}
