#include "meoave_voice.h"
#include "bsp_audio.h"
#include "bsp_usb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static meo_voice_view_t view;
static meo_sentence_player_t player;
static const int16_t *source;
static bool cancel_requested,demo;

typedef struct {
    uint16_t bytes;
    uint8_t data[MEO_VOICE_STREAM_BLOCK_BYTES];
} stream_block_t;

#define STREAM_QUEUE_DEPTH 64u
static QueueHandle_t stream_queue;
static bool stream_mode;
static volatile bool stream_end;
static uint32_t stream_received_frames;
#ifdef MEO_VOICE_FIXTURE
static uint32_t demo_generation;
#include "voice_fixture.h"
extern const uint8_t fixture_start[] asm("_binary_voice_fixture_pcm_start");
#endif

static void publish(const meo_sentence_frame_t *frame,const bsp_audio_voice_status_t *audio)
{
    portENTER_CRITICAL(&lock);
    if (frame) {view.frame=*frame;view.has_frame=true;}
    if (audio) view.underruns=audio->underruns;
    ++view.sequence;
    portEXIT_CRITICAL(&lock);
}

static bool canceled(void)
{
    portENTER_CRITICAL(&lock);bool result=cancel_requested;portEXIT_CRITICAL(&lock);
    return result;
}

static bool stream_canceled(void)
{
    return canceled();
}

static esp_err_t play_sentence(void)
{
    esp_err_t err=bsp_audio_voice_open(player.sentence.frames);
    if (err!=ESP_OK) return err;
    /* The newly owned DMA buffers are scrubbed while PA starts (150 ms in
       the verified BSP). No PCM is consumed until resume below. */
    err=bsp_audio_speaker(true);
    uint32_t submitted=0,last=UINT32_MAX;
    int64_t progress_at=esp_timer_get_time();
    bool resumed=false;
    while (err==ESP_OK && player.active && !canceled()) {
        int16_t stereo[240*2];
        while (submitted<player.sentence.frames) {
            size_t count=player.sentence.frames-submitted;
            if (count>240) count=240;
            for (size_t i=0;i<count;++i) stereo[2*i]=stereo[2*i+1]=source[submitted+i];
            if (!bsp_audio_voice_push(stereo,count)) break;
            submitted+=(uint32_t)count;
        }
        if (!resumed) {bsp_audio_voice_pause(false);resumed=true;}
        bsp_audio_voice_status_t audio;bsp_audio_voice_status(&audio);
        if (audio.fault) {err=ESP_FAIL;break;}
        if (audio.completed_frames!=last) {
            last=audio.completed_frames;progress_at=esp_timer_get_time();
        } else if (esp_timer_get_time()-progress_at>1000000) {err=ESP_ERR_TIMEOUT;break;}
        if (audio.completed_frames) {
            meo_sentence_frame_t frame;
            if (!meo_sentence_sample(&player,audio.completed_frames,
                    audio.audible && bsp_audio_get_volume()>0,audio.envelope/1000.f,false,&frame)) {
                err=ESP_FAIL;break;
            }
            publish(&frame,&audio);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (canceled()) err=ESP_ERR_INVALID_STATE;
    esp_err_t close=bsp_audio_voice_close();
    if (err==ESP_OK) err=close;
    char line[192];
    snprintf(line,sizeof(line),"MEO:VOICE_SENTENCE_DONE generation=%lu sentence=%lu played=%lu frames=%lu error=%d\n",
        (unsigned long)player.generation,(unsigned long)player.sentence.sentence_id,
        (unsigned long)player.played_frames,(unsigned long)player.sentence.frames,err);
    (void)bsp_usb_cdc_write(line);
    return err;
}

#ifdef MEO_VOICE_FIXTURE
static bool fixture_sentence(unsigned id,meo_sentence_t *s)
{
    /* Deliberately exercise the SAME bounded JSON parser as a future gateway. */
    const char *text=id==1 ? "你好，我在这里。" : "今天也陪着你。";
    unsigned frames=id==1 ? MEO_FIXTURE_FRAMES_1 : MEO_FIXTURE_FRAMES_2;
    char json[1024];
    int n=snprintf(json,sizeof(json),
        "{\"v\":1,\"generation\":%lu,\"sentence_id\":%u,\"sample_rate\":48000,\"channels\":1,\"format\":\"pcm_s16le\",\"frames\":%u,\"text\":\"%s\",\"expressions\":["
        "{\"at_ms\":0,\"expression\":\"%s\",\"intensity\":0.6,\"transition_ms\":160},"
        "{\"at_ms\":600,\"expression\":\"%s\",\"intensity\":0.85,\"transition_ms\":180},"
        "{\"at_ms\":1400,\"expression\":\"%s\",\"intensity\":0.65,\"transition_ms\":200}]}",
        (unsigned long)demo_generation,id,frames,text,id==1 ? "CURIOUS" : "LOVE",
        id==1 ? "HAPPY" : "WINK",id==1 ? "EXCITED" : "RELIEVED");
    return n>0 && (size_t)n<sizeof(json) && meo_sentence_parse(json,(size_t)n,s);
}
#endif

static void voice_task(void *arg)
{
    (void)arg;
    esp_err_t error=play_sentence();
#ifdef MEO_VOICE_FIXTURE
    if (error==ESP_OK && demo && !canceled()) {
        meo_sentence_t second;
        if (!fixture_sentence(2,&second) || !meo_sentence_begin(&player,&second)) error=ESP_FAIL;
        else {
            source=(const int16_t*)fixture_start+MEO_FIXTURE_FRAMES_1;
            portENTER_CRITICAL(&lock);
            view.sentence_id=2;view.total_frames=second.frames;view.has_frame=false;++view.sequence;
            portEXIT_CRITICAL(&lock);
            error=play_sentence();
        }
    }
#endif
    if (error!=ESP_OK) meo_sentence_cancel(&player);
    bsp_audio_output_release();
    portENTER_CRITICAL(&lock);
    view.error=error;view.busy=false;
    if (error!=ESP_OK) view.has_frame=false;
    ++view.sequence;
    demo=false;
    portEXIT_CRITICAL(&lock);
    vTaskDelete(NULL);
}

static esp_err_t play_stream_sentence(void)
{
    esp_err_t err=bsp_audio_voice_open(player.sentence.frames);
    if (err!=ESP_OK) return err;
    err=bsp_audio_speaker(true);
    uint32_t submitted=0,last=UINT32_MAX;
    int64_t wait_started=esp_timer_get_time(),progress_at=wait_started;
    bool resumed=false;
    while (err==ESP_OK && submitted<player.sentence.frames && !stream_canceled()) {
        stream_block_t block;
        if (xQueueReceive(stream_queue,&block,pdMS_TO_TICKS(100)) != pdTRUE) {
            portENTER_CRITICAL(&lock);
            uint32_t received=stream_received_frames;
            bool ended=stream_end;
            portEXIT_CRITICAL(&lock);
            if (ended && received<player.sentence.frames &&
                esp_timer_get_time()-wait_started>10000000LL) err=ESP_ERR_TIMEOUT;
            continue;
        }
        wait_started=esp_timer_get_time();
        if (!block.bytes || (block.bytes&1u)) { err=ESP_ERR_INVALID_SIZE; break; }
        size_t frames=block.bytes/2u, offset=0;
        while (offset<frames && !stream_canceled()) {
            size_t count=frames-offset;
            if (count>240) count=240;
            int16_t stereo[240*2];
            for (size_t i=0;i<count;++i) {
                int16_t sample;
                memcpy(&sample,block.data+(offset+i)*2u,sizeof(sample));
                stereo[2*i]=stereo[2*i+1]=sample;
            }
            while (!bsp_audio_voice_push(stereo,count) && !stream_canceled()) {
                bsp_audio_voice_status_t audio;
                bsp_audio_voice_status(&audio);
                if (audio.fault) { err=ESP_FAIL; break; }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            if (err!=ESP_OK || stream_canceled()) break;
            submitted+=(uint32_t)count;offset+=count;
            if (!resumed) { bsp_audio_voice_pause(false); resumed=true; }
        }
        if (err!=ESP_OK || stream_canceled()) break;
        bsp_audio_voice_status_t audio;
        bsp_audio_voice_status(&audio);
        if (audio.fault) { err=ESP_FAIL; break; }
        if (audio.completed_frames!=last) {
            last=audio.completed_frames;progress_at=esp_timer_get_time();
        }
        if (audio.completed_frames) {
            meo_sentence_frame_t frame;
            if (!meo_sentence_sample(&player,audio.completed_frames,
                    audio.audible && bsp_audio_get_volume()>0,audio.envelope/1000.f,false,&frame)) {
                err=ESP_FAIL;break;
            }
            publish(&frame,&audio);
        }
        if (esp_timer_get_time()-progress_at>10000000LL) { err=ESP_ERR_TIMEOUT; break; }
    }
    if (stream_canceled()) err=ESP_ERR_INVALID_STATE;
    if (err==ESP_OK) {
        int64_t drain_at=esp_timer_get_time();
        for (;;) {
            bsp_audio_voice_status_t audio;
            bsp_audio_voice_status(&audio);
            if (audio.fault) { err=ESP_FAIL; break; }
            if (audio.completed_frames!=last) {
                last=audio.completed_frames;drain_at=esp_timer_get_time();
                meo_sentence_frame_t frame;
                if (!meo_sentence_sample(&player,audio.completed_frames,
                        audio.audible && bsp_audio_get_volume()>0,audio.envelope/1000.f,false,&frame)) {
                    err=ESP_FAIL;break;
                }
                publish(&frame,&audio);
            }
            /* The final DMA completion changes completed_frames to exactly
               total before the next task tick.  Sample it once before leaving
               the drain loop; otherwise the report and the last face cue stay
               one 240-frame DMA block behind even though playback succeeded. */
            if (audio.completed_frames>=player.sentence.frames) break;
            if (audio.completed_frames==last && esp_timer_get_time()-drain_at>3000000LL) {
                err=ESP_ERR_TIMEOUT;break;
            }
            if (stream_canceled()) { err=ESP_ERR_INVALID_STATE;break; }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    esp_err_t close=bsp_audio_voice_close();
    if (err==ESP_OK) err=close;
    char line[224];
    snprintf(line,sizeof(line),"MEO:VOICE_SENTENCE_DONE generation=%lu sentence=%lu played=%lu frames=%lu streamed=1 error=%d\n",
        (unsigned long)player.generation,(unsigned long)player.sentence.sentence_id,
        (unsigned long)player.played_frames,(unsigned long)player.sentence.frames,err);
    (void)bsp_usb_cdc_write(line);
    return err;
}

static void stream_voice_task(void *arg)
{
    (void)arg;
    esp_err_t error=play_stream_sentence();
    if (error!=ESP_OK) meo_sentence_cancel(&player);
    bsp_audio_output_release();
    portENTER_CRITICAL(&lock);
    view.error=error;view.busy=false;stream_mode=false;stream_end=false;
    ++view.sequence;
    portEXIT_CRITICAL(&lock);
    vTaskDelete(NULL);
}

static bool start(const meo_sentence_t *s,const int16_t *mono,size_t frames,bool is_demo)
{
    if (!s || !mono || frames!=s->frames || !meo_sentence_valid(s) || !bsp_audio_output_try_acquire()) return false;
    portENTER_CRITICAL(&lock);
    bool ok=!view.busy;
    if (ok) {view.busy=true;cancel_requested=false;demo=is_demo;}
    portEXIT_CRITICAL(&lock);
    if (!ok) {bsp_audio_output_release();return false;}
    if (s->generation!=player.generation && !meo_sentence_turn(&player,s->generation)) ok=false;
    if (ok) ok=meo_sentence_begin(&player,s);
    if (ok) {
        source=mono;
        portENTER_CRITICAL(&lock);
        view.generation=s->generation;view.sentence_id=s->sentence_id;view.total_frames=s->frames;
        view.error=ESP_OK;view.has_frame=false;view.underruns=0;++view.sequence;
        portEXIT_CRITICAL(&lock);
        ok=xTaskCreate(voice_task,"voice_sentence",8192,NULL,6,NULL)==pdPASS;
    }
    if (!ok) {
        /* A failed generation/sentence cannot accidentally launch later. */
        meo_sentence_cancel(&player);
        portENTER_CRITICAL(&lock);view.busy=false;demo=false;portEXIT_CRITICAL(&lock);
        bsp_audio_output_release();
    }
    return ok;
}

bool meo_voice_start(const meo_sentence_t *s,const int16_t *mono,size_t frames)
{ return start(s,mono,frames,false); }

bool meo_voice_cancel(void)
{
    portENTER_CRITICAL(&lock);
    bool ok=view.busy;
    if (ok) cancel_requested=true;
    portEXIT_CRITICAL(&lock);
    return ok;
}

void meo_voice_stream_init(void)
{
    if (!stream_queue) stream_queue=xQueueCreate(STREAM_QUEUE_DEPTH,sizeof(stream_block_t));
}

bool meo_voice_stream_begin_sentence(const meo_sentence_t *s)
{
    if (!s || !meo_sentence_valid(s) || !stream_queue || !bsp_audio_output_try_acquire()) return false;
    portENTER_CRITICAL(&lock);
    bool ok=!view.busy && !stream_mode;
    if (ok) {
        view.busy=true;view.generation=s->generation;view.sentence_id=s->sentence_id;
        view.total_frames=s->frames;view.error=ESP_OK;view.has_frame=false;
        view.underruns=0;stream_mode=true;stream_end=false;stream_received_frames=0;
        cancel_requested=false;
    }
    portEXIT_CRITICAL(&lock);
    if (!ok) { bsp_audio_output_release(); return false; }
    (void)xQueueReset(stream_queue);
    if (s->generation!=player.generation) ok=meo_sentence_turn(&player,s->generation);
    if (ok) ok=meo_sentence_begin(&player,s);
    if (ok) ok=xTaskCreate(stream_voice_task,"voice_stream",8192,NULL,6,NULL)==pdPASS;
    if (!ok) {
        meo_sentence_cancel(&player);
        portENTER_CRITICAL(&lock);view.busy=false;stream_mode=false;portEXIT_CRITICAL(&lock);
        bsp_audio_output_release();
    }
    return ok;
}

bool meo_voice_stream_push_pcm(const uint8_t *pcm,size_t bytes)
{
    if (!pcm || !bytes || bytes>MEO_VOICE_STREAM_BLOCK_BYTES || (bytes&1u) || !stream_queue) return false;
    stream_block_t block={.bytes=(uint16_t)bytes};
    memcpy(block.data,pcm,bytes);
    portENTER_CRITICAL(&lock);
    bool ok=stream_mode && view.busy &&
        stream_received_frames+(uint32_t)(bytes/2u)<=view.total_frames;
    portEXIT_CRITICAL(&lock);
    if (!ok || xQueueSend(stream_queue,&block,0)!=pdTRUE) return false;
    portENTER_CRITICAL(&lock);stream_received_frames+=(uint32_t)(bytes/2u);portEXIT_CRITICAL(&lock);
    return true;
}

bool meo_voice_stream_end(void)
{
    portENTER_CRITICAL(&lock);
    bool ok=stream_mode && view.busy;
    if (ok) stream_end=true;
    portEXIT_CRITICAL(&lock);
    return ok;
}

bool meo_voice_stream_active(void)
{
    portENTER_CRITICAL(&lock);bool ok=stream_mode && view.busy;portEXIT_CRITICAL(&lock);return ok;
}

void meo_voice_view(meo_voice_view_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&lock);*out=view;portEXIT_CRITICAL(&lock);
}

bool meo_voice_demo_available(void)
{
#ifdef MEO_VOICE_FIXTURE
    return true;
#else
    return false;
#endif
}

bool meo_voice_demo(void)
{
#ifdef MEO_VOICE_FIXTURE
    meo_voice_view_t state;meo_voice_view(&state);
    if (state.busy || state.generation==UINT32_MAX) return false;
    demo_generation=state.generation+1;
    meo_sentence_t first;
    if (!fixture_sentence(1,&first)) return false;
    return start(&first,(const int16_t*)fixture_start,first.frames,true);
#else
    return false;
#endif
}
