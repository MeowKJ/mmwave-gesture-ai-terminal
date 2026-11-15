#pragma once
#include "meoave_sentence.h"
#include "esp_err.h"

typedef struct {
    meo_sentence_frame_t frame;
    uint32_t generation,sentence_id,total_frames,sequence,underruns;
    esp_err_t error;
    bool busy,has_frame;
} meo_voice_view_t;

/* Full sentence PCM at 48k/S16/mono. The input is borrowed until busy=false;
 * caller must retain it unchanged. Length is checked against the manifest.
 * Future WSS reception feeds this seam; there is no network client here yet. */
bool meo_voice_start(const meo_sentence_t *sentence,const int16_t *mono,size_t frames);
bool meo_voice_cancel(void);
void meo_voice_view(meo_voice_view_t *out);

/* Native-USB CDC streaming seam.  The transport sends a validated sentence
 * first, then bounded PCM blocks.  The device starts its normal DMA player
 * while blocks are arriving, so the host never has to fit a whole sentence
 * into P4 RAM. */
#define MEO_VOICE_STREAM_BLOCK_BYTES 960u
void meo_voice_stream_init(void);
bool meo_voice_stream_begin_sentence(const meo_sentence_t *sentence);
/* Diagnostic detail for a false begin result. The value is static and only
 * valid until the next begin attempt; it contains no user text or secrets. */
const char *meo_voice_stream_start_error(void);
bool meo_voice_stream_push_pcm(const uint8_t *pcm, size_t bytes);
bool meo_voice_stream_end(void);
bool meo_voice_stream_active(void);
