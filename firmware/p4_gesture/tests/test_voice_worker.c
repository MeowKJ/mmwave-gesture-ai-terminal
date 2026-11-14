/* Production worker + real PCM ring, simulated DMA only; no physical I2S. */
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define pdMS_TO_TICKS(n) (n)
#include "freertos/task.h"
void vTaskDelay(TickType_t ticks);
#include "../../p4_gesture/main/meoave_voice.c"
#include "meoave_pcm.h"
#include <assert.h>
#include <stdlib.h>

/* The streaming entry points share this translation unit. Provide a bounded
   queue double too; these tests below still exercise the sentence worker. */
struct fake_queue { size_t item_size; unsigned head, count, capacity; uint8_t *data; };
QueueHandle_t xQueueCreate(UBaseType_t n,UBaseType_t size)
{
    QueueHandle_t q=calloc(1,sizeof(*q));assert(q);
    q->data=calloc(n,size);assert(q->data);q->item_size=size;q->capacity=n;return q;
}
BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t wait)
{
    (void)wait;if(q->count==q->capacity)return pdFALSE;
    memcpy(q->data+((q->head+q->count++)%q->capacity)*q->item_size,item,q->item_size);return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *item,TickType_t wait)
{
    (void)wait;if(!q->count)return pdFALSE;
    memcpy(item,q->data+q->head*q->item_size,q->item_size);
    q->head=(q->head+1)%q->capacity;--q->count;return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t q){q->head=q->count=0;return pdTRUE;}

static meo_pcm_t ring;
static int16_t dma[6][480];
static bool leased,pa,fail_task,stop_dma,inject_cancel;
static int64_t fake_us,next_dma;
static unsigned slot,releases;

void fake_enter_critical(portMUX_TYPE *l){(void)l;}
void fake_exit_critical(portMUX_TYPE *l){(void)l;}
int64_t esp_timer_get_time(void){return fake_us;}
void vTaskDelete(TaskHandle_t h){(void)h;}
void vTaskDelay(TickType_t ticks)
{
    fake_us+=(int64_t)ticks*1000;
    if (inject_cancel && fake_us>50000) {assert(meo_voice_cancel());inject_cancel=false;}
    while (!stop_dma && fake_us>=next_dma) {
        meo_pcm_sent(&ring,dma[slot],sizeof(dma[slot]));slot=(slot+1)%6;next_dma+=5000;
    }
}
BaseType_t xTaskCreate(void (*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *h)
{(void)name;(void)stack;(void)priority;(void)h;if(fail_task)return 0;fn(arg);return pdPASS;}
bool bsp_audio_output_try_acquire(void){if(leased)return false;leased=true;return true;}
void bsp_audio_output_release(void){assert(leased);leased=false;++releases;}
esp_err_t bsp_audio_speaker(bool on){pa=on;return ESP_OK;}
int bsp_audio_get_volume(void){return 50;}
esp_err_t bsp_audio_voice_open(uint32_t frames)
{
    assert(leased);assert(meo_pcm_open(&ring,frames));slot=0;next_dma=fake_us+5000;
    for (unsigned i=0;i<6;++i)meo_pcm_sent(&ring,dma[i],sizeof(dma[i]));
    return ESP_OK;
}
bool bsp_audio_voice_push(const int16_t *s,size_t frames){return meo_pcm_push(&ring,s,frames);}
void bsp_audio_voice_pause(bool pause){ring.paused=pause;}
void bsp_audio_voice_status(bsp_audio_voice_status_t *v)
{
    *v=(bsp_audio_voice_status_t){.completed_frames=ring.completed,.submitted_frames=ring.submitted,
        .underruns=ring.underruns,.queued_blocks=ring.count,.envelope=ring.envelope,
        .active=ring.active,.paused=ring.paused,.draining=ring.draining,.fault=ring.fault,.audible=ring.audible};
}
esp_err_t bsp_audio_voice_close(void)
{
    pa=false;meo_pcm_cancel(&ring);
    for (unsigned i=0;i<6;++i)meo_pcm_sent(&ring,dma[i],sizeof(dma[i]));
    return ESP_OK;
}
esp_err_t bsp_usb_cdc_write(const char *s){assert(strchr(s,'\n'));return ESP_OK;}

int main(void)
{
    static int16_t mono[4901];for(unsigned i=0;i<4901;++i)mono[i]=2000;
    meo_sentence_t s={.generation=1,.sentence_id=1,.frames=4901,.text="你好。",
        .cue_count=2,.cues={{0,20,MEO_EYE_HAPPY,.8f},{50,20,MEO_EYE_RELIEVED,.6f}}};
    assert(!meo_voice_start(&s,mono,4900));assert(!leased);
    assert(meo_voice_start(&s,mono,4901));meo_voice_view_t v;meo_voice_view(&v);
    assert(!v.busy && v.error==ESP_OK && v.frame.finished && v.frame.played_frames==4901);
    assert(v.frame.cue_index==1 && !v.underruns && !leased && !pa && releases==1);
    assert(!meo_voice_start(&s,mono,4901));assert(!leased); /* duplicate */
    s.generation=2;fail_task=true;assert(!meo_voice_start(&s,mono,4901));assert(!leased);fail_task=false;
    s.generation=3;inject_cancel=true;fake_us=0;
    assert(meo_voice_start(&s,mono,4901));meo_voice_view(&v);
    assert(!v.busy && v.error==ESP_ERR_INVALID_STATE && !pa && !leased);
    s.generation=4;stop_dma=true;
    assert(meo_voice_start(&s,mono,4901));meo_voice_view(&v);
    assert(v.error==ESP_ERR_TIMEOUT && !v.busy && !leased && !pa);
    puts("PASS: production sentence worker PCM drain/finish, length/duplicate guards, cancel, task failure and clock timeout");
    return 0;
}
