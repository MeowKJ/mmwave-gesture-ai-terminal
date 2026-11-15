#include "meoave_voice.h"
#include "bsp_audio.h"
#include "bsp_usb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static meo_voice_view_t view;
static meo_sentence_player_t player;
static const int16_t *source;
static bool cancel_requested;

typedef struct {
    uint16_t bytes;
    uint8_t data[MEO_VOICE_STREAM_BLOCK_BYTES];
} stream_block_t;

#define STREAM_QUEUE_DEPTH 64u
static QueueHandle_t stream_queue;
static StaticQueue_t stream_queue_control;
static uint8_t *stream_queue_storage;
static bool stream_mode;
static volatile bool stream_end;
static uint32_t stream_received_frames;
static const char *stream_start_error = "NONE";

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

static void voice_task(void *arg)
{
    (void)arg;
    esp_err_t error=play_sentence();
    if (error!=ESP_OK) meo_sentence_cancel(&player);
    bsp_audio_output_release();
    portENTER_CRITICAL(&lock);
    view.error=error;view.busy=false;
    if (error!=ESP_OK) view.has_frame=false;
    ++view.sequence;
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

static bool start(const meo_sentence_t *s,const int16_t *mono,size_t frames)
{
    if (!s || !mono || frames!=s->frames || !meo_sentence_valid(s) || !bsp_audio_output_try_acquire()) return false;
    portENTER_CRITICAL(&lock);
    bool ok=!view.busy;
    if (ok) {view.busy=true;cancel_requested=false;}
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
        portENTER_CRITICAL(&lock);view.busy=false;portEXIT_CRITICAL(&lock);
        bsp_audio_output_release();
    }
    return ok;
}

bool meo_voice_start(const meo_sentence_t *s,const int16_t *mono,size_t frames)
{ return start(s,mono,frames); }

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
    if (stream_queue) return;
    /* The PCM staging queue is task-to-task data, never ISR data. Keep its
       ~62 KiB payload in PSRAM so the display, ESP-SR and the runtime voice
       task can still obtain contiguous internal SRAM. Fall back only on
       boards without usable PSRAM; the product profile requires PSRAM. */
    stream_queue_storage = heap_caps_malloc(sizeof(stream_block_t) * STREAM_QUEUE_DEPTH,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream_queue_storage) {
        stream_queue = xQueueCreateStatic(STREAM_QUEUE_DEPTH, sizeof(stream_block_t),
                                          stream_queue_storage, &stream_queue_control);
    }
    if (!stream_queue) {
        stream_queue = xQueueCreate(STREAM_QUEUE_DEPTH, sizeof(stream_block_t));
        if (stream_queue) stream_start_error = "PSRAM_QUEUE_FALLBACK";
    }
}

const char *meo_voice_stream_start_error(void)
{
    return stream_start_error;
}

bool meo_voice_stream_begin_sentence(const meo_sentence_t *s)
{
    stream_start_error = "NONE";
    if (!s || !meo_sentence_valid(s)) { stream_start_error = "SENTENCE_INVALID"; return false; }
    if (!stream_queue) { stream_start_error = "STREAM_QUEUE"; return false; }
    if (!bsp_audio_output_try_acquire()) { stream_start_error = "AUDIO_BUSY"; return false; }
    portENTER_CRITICAL(&lock);
    bool ok=!view.busy && !stream_mode;
    if (ok) {
        view.busy=true;view.generation=s->generation;view.sentence_id=s->sentence_id;
        view.total_frames=s->frames;view.error=ESP_OK;view.has_frame=false;
        view.underruns=0;stream_mode=true;stream_end=false;stream_received_frames=0;
        cancel_requested=false;
    }
    portEXIT_CRITICAL(&lock);
    if (!ok) { stream_start_error = "VOICE_BUSY"; bsp_audio_output_release(); return false; }
    (void)xQueueReset(stream_queue);
    if (s->generation!=player.generation) ok=meo_sentence_turn(&player,s->generation);
    if (!ok) stream_start_error = "GENERATION";
    if (ok) ok=meo_sentence_begin(&player,s);
    if (!ok && !strcmp(stream_start_error, "NONE")) stream_start_error = "SENTENCE_BEGIN";
    if (ok) ok=xTaskCreate(stream_voice_task,"voice_stream",4096,NULL,6,NULL)==pdPASS;
    if (!ok && !strcmp(stream_start_error, "NONE")) stream_start_error = "TASK_NO_MEM";
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
