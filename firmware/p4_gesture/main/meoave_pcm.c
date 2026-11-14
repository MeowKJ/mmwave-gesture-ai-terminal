#include "meoave_pcm.h"
#include "meoave_sentence.h"
#include <string.h>

bool meo_pcm_open(meo_pcm_t *p,uint32_t frames)
{
    if (!p || p->active || p->draining || !frames || frames>MEO_SENTENCE_MAX_FRAMES) return false;
    memset(p,0,sizeof(*p));p->active=true;p->paused=true;p->total=frames;
    return true;
}

bool meo_pcm_push(meo_pcm_t *p,const int16_t *stereo,size_t frames)
{
    if (!p || !stereo || !p->active || p->fault || p->count==MEO_PCM_QUEUE_BLOCKS ||
        !frames || frames>MEO_PCM_BLOCK_FRAMES || frames>p->total-p->submitted ||
        (frames<MEO_PCM_BLOCK_FRAMES && frames!=p->total-p->submitted)) return false;
    meo_pcm_block_t *b=&p->queue[(p->head+p->count)%MEO_PCM_QUEUE_BLOCKS];
    memcpy(b->pcm,stereo,frames*4);b->frames=(uint16_t)frames;
    uint32_t absolute=0;
    for (size_t i=0;i<frames;++i) {
        int n=stereo[i*2];absolute+=(uint32_t)(n<0 ? -n : n);
    }
    unsigned level=(unsigned)(absolute/frames);
    /* Mean absolute PCM envelope, normalized at -15 dBFS. A silence gate
       avoids a trembling mouth on converter/codec-floor noise. */
    b->envelope=(uint16_t)(level<100 ? 0 : level>=6000 ? 1000 : level*1000/6000);
    p->submitted+=(uint32_t)frames;++p->count;
    return true;
}

void meo_pcm_cancel(meo_pcm_t *p)
{
    if (!p) return;
    p->active=false;p->paused=true;p->draining=true;p->count=0;p->audible=false;p->envelope=0;
    /* PA must be disabled by the worker BEFORE cancellation. Scheduled data
       is scrubbed over the next DMA lap; its stale completion is not counted. */
    for (unsigned i=0;i<MEO_PCM_DMA_SLOTS;++i) p->dma[i].frames=0;
}

void MEO_PCM_ISR meo_pcm_sent(meo_pcm_t *p,void *buffer,size_t bytes)
{
    if (!p->active && !p->draining) return;
    if (!buffer || bytes!=MEO_PCM_BLOCK_FRAMES*4) {
        p->fault=true;p->active=false;p->audible=false;return;
    }
    unsigned slot=MEO_PCM_DMA_SLOTS;
    for (unsigned i=0;i<MEO_PCM_DMA_SLOTS;++i) if (p->dma[i].buffer==buffer) {slot=i;break;}
    if (slot==MEO_PCM_DMA_SLOTS) {
        for (unsigned i=0;i<MEO_PCM_DMA_SLOTS;++i) if (!p->dma[i].buffer) {slot=i;break;}
        if (slot==MEO_PCM_DMA_SLOTS) {p->fault=true;p->active=false;return;}
        p->dma[slot].buffer=buffer;
    }
    unsigned finished=p->dma[slot].frames;
    p->audible=p->active && finished>0;
    p->envelope=p->audible ? p->dma[slot].envelope : 0;
    if (p->active && finished) {p->completed+=finished;p->started=true;}
    else if (p->active && p->started && !p->paused && p->completed<p->total) ++p->underruns;
    p->dma[slot].frames=0;
    memset(buffer,0,bytes);
    if (p->active && !p->paused && p->count) {
        meo_pcm_block_t *b=&p->queue[p->head];
        memcpy(buffer,b->pcm,b->frames*4);
        p->dma[slot].frames=b->frames;p->dma[slot].envelope=b->envelope;
        p->head=(p->head+1)%MEO_PCM_QUEUE_BLOCKS;--p->count;
    }
    if (p->active && p->completed==p->total) {
        p->active=false;p->draining=true;p->audible=false;p->envelope=0;
    }
    if (p->draining) {
        /* During cancel every descriptor must be observed at least once
           AFTER cancel, even though the logical metadata was invalidated. */
        p->dma[slot].envelope=UINT16_MAX;
        bool clean=true;
        for (unsigned i=0;i<MEO_PCM_DMA_SLOTS;++i)
            if (p->dma[i].envelope!=UINT16_MAX || p->dma[i].frames) clean=false;
        if (clean) p->draining=false;
    }
}
