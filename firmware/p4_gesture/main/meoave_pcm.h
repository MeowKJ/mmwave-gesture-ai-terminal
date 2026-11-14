#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define MEO_PCM_ISR IRAM_ATTR
#else
#define MEO_PCM_ISR
#endif

/* Matches the EXISTING I2S defaults, not a change to pins/clocks/DMA sizing. */
#define MEO_PCM_BLOCK_FRAMES 240u
#define MEO_PCM_DMA_SLOTS 6u
#define MEO_PCM_QUEUE_BLOCKS 12u

typedef struct {
    int16_t pcm[MEO_PCM_BLOCK_FRAMES*2];
    uint16_t frames,envelope;
} meo_pcm_block_t;
typedef struct {
    meo_pcm_block_t queue[MEO_PCM_QUEUE_BLOCKS];
    struct {void *buffer;uint16_t frames,envelope;} dma[MEO_PCM_DMA_SLOTS];
    unsigned head,count;
    uint32_t total,submitted,completed,underruns;
    uint16_t envelope;
    bool active,paused,draining,fault,audible,started;
} meo_pcm_t;

/* Caller serializes producer/ISR access. Fixed internal SRAM only. */
bool meo_pcm_open(meo_pcm_t *p,uint32_t total_frames);
bool meo_pcm_push(meo_pcm_t *p,const int16_t *stereo,size_t frames);
void meo_pcm_cancel(meo_pcm_t *p);
/* On a completed DMA descriptor: account its OLD payload, then fill its next
 * lap. Never count enqueued/write-return frames as played. ISR: no FPU/alloc/IO. */
void meo_pcm_sent(meo_pcm_t *p,void *dma,size_t bytes);
