#include "meoave_espsr.h"
#include "meoave_sr_pack.h"
#include "meoave_wake_dsp.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#if defined(MEOAVE_ESPSR_LINKED) && MEOAVE_ESPSR_LINKED
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#endif

#ifndef CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL
#define CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL "wn9_maomao"
#endif
#ifndef CONFIG_APP_MEOAVE_ESPSR_MIC_CHANNEL
#define CONFIG_APP_MEOAVE_ESPSR_MIC_CHANNEL 0
#endif

#define TAG "MEO_ESPSR"
#define PCM_OUT_RATE 16000
#define PCM_CHUNK_MAX 2048
#define AUDIO_BLOCK_FRAMES 480
#define AUDIO_QUEUE_BLOCKS 12
#define MAX_AUDIO_AGE_MS 200
#define DETECTION_COOLDOWN_MS 1800
#define WORKER_STACK_BYTES 8192

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static meoave_espsr_view_t s_view;
static bool s_initialized;
static uint32_t s_epoch;
static uint64_t s_output_quiet_at;

static void set_error(const char *error)
{
    portENTER_CRITICAL(&s_lock);
    s_view.armed = false;
    snprintf(s_view.error, sizeof(s_view.error), "%s", error ? error : "UNKNOWN");
    portEXIT_CRITICAL(&s_lock);
}

#if defined(MEOAVE_ESPSR_LINKED) && MEOAVE_ESPSR_LINKED
typedef struct {
    uint32_t epoch, sequence;
    uint64_t now_ms;
    size_t frames;
    int16_t mono[AUDIO_BLOCK_FRAMES];
} audio_block_t;

static QueueHandle_t s_queue;
static StaticQueue_t s_queue_control;
static StaticTask_t s_worker_tcb;
static uint8_t *s_queue_storage;
static StackType_t *s_worker_stack;
static uint32_t s_sequence;
static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_wn_data;
static int s_wn_chunk;
/* All signal/model state below is exclusively owned by wake_worker. */
static meo_wake_dsp_t s_dsp;
/* Do not permanently steal 4 KiB of internal BSS from display/PPA startup. */
static int16_t *s_chunk;
static size_t s_chunk_count;
static uint32_t s_worker_epoch, s_worker_sequence;
static bool s_worker_contiguous;
static bool s_model_dirty;
static uint64_t s_last_detection;

/* Preserve only diagnosis, never a model pointer, across a panic. An upstream
   assert must not make the entire product reboot forever. The guard is scoped
   to the ELF identity, so a new repaired image gets one fresh attempt. */
#define SR_GUARD_MAGIC 0x53524732u
enum { SR_STAGE_IDLE, SR_STAGE_VALIDATE, SR_STAGE_MAP, SR_STAGE_CREATE,
       SR_STAGE_FORMAT, SR_STAGE_CLEAN, SR_STAGE_DETECT, SR_STAGE_DESTROY };
typedef struct {
    uint32_t magic, image, stage, blocked, check;
} sr_guard_t;
static RTC_NOINIT_ATTR volatile sr_guard_t s_guard;
static uint32_t s_image;

static void guard_write(uint32_t stage, bool blocked)
{
    s_guard.magic = 0;
    s_guard.image = s_image;
    s_guard.stage = stage;
    s_guard.blocked = blocked;
    s_guard.check = SR_GUARD_MAGIC ^ s_image ^ stage ^ (uint32_t)blocked;
    __sync_synchronize();
    s_guard.magic = SR_GUARD_MAGIC;
    __sync_synchronize();
}

static bool guard_boot(void)
{
    s_view.crash_guarded = false;
    s_view.crash_stage = 0;
    memcpy(&s_image, esp_app_get_description()->app_elf_sha256, sizeof(s_image));
    esp_reset_reason_t reason = esp_reset_reason();
    bool valid = s_guard.magic == SR_GUARD_MAGIC && s_guard.image == s_image &&
                 s_guard.stage <= SR_STAGE_DESTROY && s_guard.blocked <= 1 &&
                 s_guard.check == (SR_GUARD_MAGIC ^ s_image ^ s_guard.stage ^ s_guard.blocked);
    bool crash = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                 reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
    if (valid && reason != ESP_RST_POWERON &&
        (s_guard.blocked || (crash && s_guard.stage != SR_STAGE_IDLE))) {
        uint32_t stage = s_guard.stage;
        guard_write(stage, true);
        s_view.crash_guarded = true;
        s_view.crash_stage = stage;
        set_error("PREVIOUS_WAKE_CRASH");
        ESP_LOGE(TAG, "wake disabled after crash: stage=%lu reset=%d; UI/USB remain available",
                 (unsigned long)stage, reason);
        return false;
    }
    guard_write(SR_STAGE_IDLE, false);
    return true;
}

static bool partition_read(void *ctx, size_t offset, void *out, size_t size)
{
    return esp_partition_read(ctx, offset, out, size) == ESP_OK;
}

static bool validate_partition(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!part) {
        set_error("MODEL_PARTITION_MISSING");
        return false;
    }
    meo_sr_pack_result_t valid = meo_sr_pack_validate(partition_read, (void *)part, part->size);
    if (valid != MEO_SR_PACK_OK) {
        set_error(valid == MEO_SR_PACK_EMPTY ? "MODEL_MISSING" :
                  valid == MEO_SR_PACK_IO ? "MODEL_READ_FAILED" : "MODEL_PACK_INVALID");
        return false;
    }
    return true;
}

/* Some official wn9_* folders contain wakenet9l metadata. Select the
   implementation from the validated metadata, not the legacy folder alias.
   The factory still receives the actual unchanged model asset name. */
static const char *model_engine_selector(const char *info)
{
    if (!info) return NULL;
    char family[12];
    size_t i = 0;
    while (i < sizeof(family) - 1 && info[i] && info[i] != '_') {
        family[i] = (char)tolower((unsigned char)info[i]);
        ++i;
    }
    if (info[i] != '_') return NULL;
    family[i] = 0;
    if (!strcmp(family, "wakenet9")) return "wn9_";
    if (!strcmp(family, "wakenet9l")) return "wn9l_";
    if (!strcmp(family, "wakenet9s")) return "wn9s_";
    return NULL;
}

static bool create_model_instance(void)
{
    ESP_LOGI(TAG, "model create: %s", CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL);
    guard_write(SR_STAGE_CREATE, false);
    s_wn_data = s_wn->create(CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL, DET_MODE_90);
    guard_write(SR_STAGE_IDLE, false);
    if (!s_wn_data) {
        set_error("MODEL_CREATE_FAILED");
        return false;
    }
    guard_write(SR_STAGE_FORMAT, false);
    s_wn_chunk = s_wn->get_samp_chunksize(s_wn_data);
    if (s_wn_chunk <= 0 || s_wn_chunk > PCM_CHUNK_MAX ||
        s_wn->get_samp_rate(s_wn_data) != PCM_OUT_RATE ||
        s_wn->get_channel_num(s_wn_data) != 1) {
        guard_write(SR_STAGE_IDLE, false);
        set_error("BAD_MODEL_AUDIO_FORMAT");
        guard_write(SR_STAGE_DESTROY, false);
        s_wn->destroy(s_wn_data);
        s_wn_data = NULL;
        guard_write(SR_STAGE_IDLE, false);
        return false;
    }
    guard_write(SR_STAGE_IDLE, false);
    s_model_dirty = false;
    portENTER_CRITICAL(&s_lock);
    s_view.model_loaded = s_view.keyword_ready = true;
    s_view.armed = s_view.requested && !s_view.output_blocked;
    s_view.feed_chunk_samples = s_wn_chunk;
    snprintf(s_view.backend, sizeof(s_view.backend), "%s", "ESP_SR_WAKENET");
    s_view.error[0] = 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "model ready; chunk=%d rate=%d internal_free=%lu largest=%lu",
             s_wn_chunk, PCM_OUT_RATE,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

static bool load_model(void)
{
    guard_write(SR_STAGE_VALIDATE, false);
    bool valid = validate_partition();
    guard_write(SR_STAGE_IDLE, false);
    if (!valid) return false;
    ESP_LOGI(TAG, "model map; internal_free=%lu largest=%lu psram_free=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    guard_write(SR_STAGE_MAP, false);
    s_models = esp_srmodel_init("model");
    guard_write(SR_STAGE_IDLE, false);
    int model_index = s_models ? esp_srmodel_exists(s_models, (char *)CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL) : -1;
    if (model_index < 0 || model_index >= s_models->num) {
        set_error("MODEL_MISSING");
        return false;
    }
    guard_write(SR_STAGE_FORMAT, false);
    char *word = esp_srmodel_get_wake_words(s_models, (char *)CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_view.model_word, sizeof(s_view.model_word), "%s", word ? word : "");
    portEXIT_CRITICAL(&s_lock);
    /* Exact phrase only. Do not accept '你好猫猫', a multi-word model, or a
       stock 喵喵 model renamed by its directory. */
    bool matches = word && !strcmp(word, MEOAVE_ESPSR_WAKE_KEYWORD);
    free(word);
    guard_write(SR_STAGE_IDLE, false);
    if (!matches) {
        set_error("MODEL_WORD_MISMATCH");
        return false;
    }
    const char *selector = s_models->model_info ? model_engine_selector(s_models->model_info[model_index]) : NULL;
    if (!selector) {
        set_error("MODEL_ARCH_UNSUPPORTED");
        return false;
    }
    s_wn = esp_wn_handle_from_name(selector);
    if (!s_wn) {
        set_error("MODEL_ENGINE_MISSING");
        return false;
    }
    ESP_LOGI(TAG, "model engine selector=%s (from metadata, asset name unchanged)", selector);
    return create_model_instance();
}

static bool block_current(const audio_block_t *block, uint64_t now_ms)
{
    bool current;
    portENTER_CRITICAL(&s_lock);
    current = s_view.armed && block->epoch == s_epoch;
    portEXIT_CRITICAL(&s_lock);
    return current && now_ms >= block->now_ms && now_ms - block->now_ms <= MAX_AUDIO_AGE_MS;
}

static bool reset_stream(void)
{
    meo_wake_dsp_reset(&s_dsp);
    s_chunk_count = 0;
    /* The shipped P4 WakeNet9 clean() walks nine queue slots although its
       factory allocates six (confirmed in the faulting ELF). Do not skip
       discontinuity isolation or patch opaque private layouts. A fresh model
       is already clean; after inference, rebuild with the public lifecycle
       calls on this worker only. Input is inhibited during the rebuild. */
    if (s_model_dirty) {
        portENTER_CRITICAL(&s_lock);
        s_view.model_loaded = s_view.keyword_ready = s_view.armed = false;
        snprintf(s_view.error, sizeof(s_view.error), "%s", "MODEL_REBUILDING");
        portEXIT_CRITICAL(&s_lock);
        guard_write(SR_STAGE_DESTROY, false);
        s_wn->destroy(s_wn_data);
        s_wn_data = NULL;
        guard_write(SR_STAGE_IDLE, false);
        if (!create_model_instance()) return false;
    }
    portENTER_CRITICAL(&s_lock);
    ++s_view.stream_resets;
    portEXIT_CRITICAL(&s_lock);
    return s_wn_data != NULL;
}

/* Separated from the task loop so tests execute the production queue and
   inference boundary without running an artificial infinite RTOS task. */
static void process_block(const audio_block_t *block)
{
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (!block_current(block, now_ms)) {
        s_worker_contiguous = false;
        return;
    }
    if (!s_worker_contiguous || block->epoch != s_worker_epoch ||
        block->sequence != s_worker_sequence + 1) {
        if (!reset_stream()) return;
    }
    s_worker_contiguous = true;
    s_worker_epoch = block->epoch;
    s_worker_sequence = block->sequence;
    for (size_t i = 0; i < block->frames; ++i) {
        int16_t sample;
        if (!meo_wake_dsp_sample(&s_dsp, block->mono[i], &sample)) continue;
        s_chunk[s_chunk_count++] = sample;
        if (s_chunk_count < (size_t)s_wn_chunk) continue;
        /* A pause/page change can happen during conversion or detect(). */
        now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (!block_current(block, now_ms)) {
            s_worker_contiguous = false;
            return;
        }
        guard_write(SR_STAGE_DETECT, false);
        s_model_dirty = true;
        int result = s_wn->detect(s_wn_data, s_chunk);
        guard_write(SR_STAGE_IDLE, false);
        uint32_t stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        s_chunk_count = 0;
        now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        bool detected = false;
        portENTER_CRITICAL(&s_lock);
        ++s_view.inferred_chunks;
        s_view.worker_stack_free = stack_free;
        if (result > 0 && s_view.armed && block->epoch == s_epoch &&
            now_ms >= block->now_ms && now_ms - block->now_ms <= MAX_AUDIO_AGE_MS &&
            (!s_last_detection || now_ms - s_last_detection >= DETECTION_COOLDOWN_MS)) {
            ++s_view.detections;
            s_view.last_detected_ms = now_ms;
            s_last_detection = now_ms;
            detected = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (detected) {
            ESP_LOGI(TAG, "acoustic keyword=%s model=%s", MEOAVE_ESPSR_WAKE_KEYWORD,
                     CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL);
            /* Discard the remainder; the next utterance starts cleanly. */
            s_worker_contiguous = false;
            return;
        }
    }
}

static void wake_worker(void *arg)
{
    (void)arg;
    if (!load_model()) {
        guard_write(SR_STAGE_DESTROY, false);
        if (s_wn_data) { s_wn->destroy(s_wn_data); s_wn_data = NULL; }
        if (s_models) { esp_srmodel_deinit(s_models); s_models = NULL; }
        guard_write(SR_STAGE_IDLE, false);
        ESP_LOGW(TAG, "local wake unavailable: %s (target=%s)", s_view.error,
                 MEOAVE_ESPSR_WAKE_KEYWORD);
        /* A static task is permanent, like the mic reader.
           Park it on model failure; never return from a task or ask the idle
           reaper to free a separately allocated static stack. */
        for (;;) vTaskSuspend(NULL);
    }
    audio_block_t block;
    for (;;) {
        if (xQueueReceive(s_queue, &block, portMAX_DELAY) == pdTRUE) process_block(&block);
    }
}
#endif

esp_err_t meoave_espsr_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_initialized = true;
    snprintf(s_view.model, sizeof(s_view.model), "%s", CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL);
    snprintf(s_view.backend, sizeof(s_view.backend), "%s", "ESP_SR_UNAVAILABLE");
    s_view.feed_rate_hz = PCM_OUT_RATE;
    portEXIT_CRITICAL(&s_lock);
#if defined(MEOAVE_ESPSR_LINKED) && MEOAVE_ESPSR_LINKED
    s_view.component_linked = true;
    if (!guard_boot()) return ESP_ERR_INVALID_STATE;
    snprintf(s_view.error, sizeof(s_view.error), "%s", "MODEL_LOADING");
    /* An empty/erased model pack must not consume a queue and task stack,
       especially during the first PPA/GUI memory peak. Called by app service
       after GUI warm-up, never while holding the GUI lock. */
    if (!validate_partition()) return ESP_ERR_NOT_FOUND;
    /* Queue payload can live in PSRAM. Keep the inference stack INTERNAL:
       model loading and vendor code may enter cache-disabled flash routines.
       Allowing an external RTOS stack does not make such calls safe. Allocate
       only after GUI warm-up; if SRAM is unavailable, fail without a reboot. */
    s_queue_storage = heap_caps_malloc(AUDIO_QUEUE_BLOCKS * sizeof(audio_block_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue_storage) {
        set_error("PSRAM_ALLOC_FAILED");
        return ESP_ERR_NO_MEM;
    }
    s_chunk = heap_caps_aligned_alloc(16, PCM_CHUNK_MAX * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chunk) {
        heap_caps_free(s_queue_storage); s_queue_storage = NULL;
        set_error("PCM_ALLOC_FAILED");
        return ESP_ERR_NO_MEM;
    }
    s_worker_stack = heap_caps_malloc(WORKER_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_worker_stack) {
        heap_caps_free(s_queue_storage); s_queue_storage = NULL;
        heap_caps_free(s_chunk); s_chunk = NULL;
        set_error("INTERNAL_STACK_ALLOC_FAILED");
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreateStatic(AUDIO_QUEUE_BLOCKS, sizeof(audio_block_t),
                                 s_queue_storage, &s_queue_control);
    if (!s_queue) {
        heap_caps_free(s_queue_storage); s_queue_storage = NULL;
        heap_caps_free(s_worker_stack); s_worker_stack = NULL;
        heap_caps_free(s_chunk); s_chunk = NULL;
        set_error("QUEUE_ALLOC_FAILED");
        return ESP_ERR_NO_MEM;
    }
    /* Model mapping and inference never run under LVGL or inside I2S RX. */
    if (!xTaskCreateStatic(wake_worker, "wake_sr", WORKER_STACK_BYTES, NULL, 2,
                          s_worker_stack, &s_worker_tcb)) {
        vQueueDelete(s_queue); s_queue = NULL;
        heap_caps_free(s_queue_storage); s_queue_storage = NULL;
        heap_caps_free(s_worker_stack); s_worker_stack = NULL;
        heap_caps_free(s_chunk); s_chunk = NULL;
        set_error("WORKER_CREATE_FAILED");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    set_error("COMPONENT_MISSING");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void meoave_espsr_set_armed(bool armed)
{
    portENTER_CRITICAL(&s_lock);
    if (s_view.requested != armed) ++s_epoch;
    s_view.requested = armed;
    /* The input tap also applies immediate speaker/quiet-tail inhibition. */
    s_view.armed = armed && s_view.keyword_ready && !s_view.output_blocked;
    portEXIT_CRITICAL(&s_lock);
}

void meoave_espsr_set_output_blocked(bool blocked, uint64_t now_ms)
{
    portENTER_CRITICAL(&s_lock);
    if (blocked) s_output_quiet_at = now_ms + 300;
    bool inhibit = blocked || now_ms < s_output_quiet_at;
    if (s_view.output_blocked != inhibit) ++s_epoch;
    s_view.output_blocked = inhibit;
    s_view.armed = s_view.requested && s_view.keyword_ready && !inhibit;
    portEXIT_CRITICAL(&s_lock);
}

void meoave_espsr_feed_stereo_48k(const int16_t *stereo, size_t frames, uint64_t now_ms)
{
#if defined(MEOAVE_ESPSR_LINKED) && MEOAVE_ESPSR_LINKED
    if (!stereo || !frames || !s_queue) return;
    const unsigned channel = CONFIG_APP_MEOAVE_ESPSR_MIC_CHANNEL > 0 ? 1u : 0u;
    while (frames) {
        audio_block_t block;
        portENTER_CRITICAL(&s_lock);
        bool active = s_view.armed;
        block.epoch = s_epoch;
        block.sequence = ++s_sequence;
        portEXIT_CRITICAL(&s_lock);
        if (!active) return;
        block.frames = frames > AUDIO_BLOCK_FRAMES ? AUDIO_BLOCK_FRAMES : frames;
        block.now_ms = now_ms;
        for (size_t i = 0; i < block.frames; ++i) block.mono[i] = stereo[i * 2 + channel];
        /* Zero wait; no inference, logging, allocation or model locks on RX. */
        bool queued = xQueueSend(s_queue, &block, 0) == pdTRUE;
        portENTER_CRITICAL(&s_lock);
        if (queued) s_view.queued_frames += (uint32_t)block.frames;
        else ++s_view.queue_drops;
        portEXIT_CRITICAL(&s_lock);
        frames -= block.frames;
        stereo += block.frames * 2;
    }
#else
    (void)stereo; (void)frames; (void)now_ms;
#endif
}

void meoave_espsr_view(meoave_espsr_view_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_view;
    portEXIT_CRITICAL(&s_lock);
}
