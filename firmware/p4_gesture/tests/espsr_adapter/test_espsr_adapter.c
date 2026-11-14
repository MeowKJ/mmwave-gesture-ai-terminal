/* Production adapter with fake RTOS/model only: this is NOT an acoustic test. */
#define MEOAVE_ESPSR_LINKED 1
#include <assert.h>
#include <stdarg.h>
#include "../../main/meoave_espsr.c"

static uint64_t clock_ms = 1000;
static unsigned detects, cleans, mappings;
static unsigned creates, destroys;
static bool model_create_failed;
static const char *expected_selector = "wn9_";
static bool cancel_in_detect, task_failed, queue_failed, psram_failed, internal_failed, chunk_failed;
static unsigned heap_allocations;
static esp_reset_reason_t reset_reason = ESP_RST_POWERON;
static esp_app_desc_t app_descriptor = {.app_elf_sha256 = {1, 2, 3, 4}};
static int detect_result, model_rate = 16000, model_channels = 1;
static const char *model_word = MEOAVE_ESPSR_WAKE_KEYWORD;
static uint8_t image[128];
static const esp_partition_t partition = {.size = sizeof(image)};
static char *metadata[] = {"wakenet9_test_猫猫_3_0.6_0.6"};
static srmodel_list_t models = {.num = 1, .model_info = metadata};
static model_iface_data_t model;
struct fake_sr_queue { unsigned head, count; audio_block_t blocks[AUDIO_QUEUE_BLOCKS]; };
static struct fake_sr_queue queue;
static void (*pending_worker)(void *);

void fake_enter_critical(portMUX_TYPE *lock) { (void)lock; }
void fake_exit_critical(portMUX_TYPE *lock) { (void)lock; }
void fake_log(const char *level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size)
{
    assert(n == AUDIO_QUEUE_BLOCKS && size == sizeof(audio_block_t));
    return queue_failed ? NULL : &queue;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t n,UBaseType_t size,uint8_t *storage,StaticQueue_t *control)
{ assert(storage && control); return xQueueCreate(n,size); }
void vQueueDelete(QueueHandle_t q){assert(q==&queue);}
void *heap_caps_malloc(size_t size,unsigned caps)
{
    ++heap_allocations;
    if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
        return psram_failed ? NULL : malloc(size);
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) && size == WORKER_STACK_BYTES);
    return internal_failed ? NULL : malloc(size);
}
void heap_caps_free(void *ptr){free(ptr);}
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{
    assert(alignment == 16 && size == PCM_CHUNK_MAX * sizeof(int16_t));
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return chunk_failed ? NULL : malloc(size);
}
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 100000; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 20000; }
esp_reset_reason_t esp_reset_reason(void) { return reset_reason; }
const esp_app_desc_t *esp_app_get_description(void) { return &app_descriptor; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 3000; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    assert(wait == 0);
    if (q->count == AUDIO_QUEUE_BLOCKS) return pdFALSE;
    q->blocks[(q->head + q->count++) % AUDIO_QUEUE_BLOCKS] = *(const audio_block_t *)item;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (!q->count) return pdFALSE;
    *(audio_block_t *)item = q->blocks[q->head];
    q->head = (q->head + 1) % AUDIO_QUEUE_BLOCKS;
    --q->count;
    return pdTRUE;
}
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, StackType_t *buffer, StaticTask_t *control)
{
    (void)name; (void)stack; (void)arg; (void)priority;
    assert(buffer && control);
    if (task_failed) return NULL;
    pending_worker = fn; return control;
}
void vTaskSuspend(TaskHandle_t task) { (void)task; }
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY);
    assert(!strcmp(name, "model")); return &partition;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t size)
{
    assert(offset <= p->size && size <= p->size - offset);
    memcpy(out, image + offset, size); return ESP_OK;
}
srmodel_list_t *esp_srmodel_init(const char *name)
{ assert(!strcmp(name, "model")); ++mappings; return &models; }
int esp_srmodel_exists(srmodel_list_t *m, char *name)
{ (void)m; assert(!strcmp(name, CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL)); return 0; }
char *esp_srmodel_get_wake_words(srmodel_list_t *m, char *name)
{
    (void)m; (void)name; size_t size = strlen(model_word) + 1;
    char *result = malloc(size); assert(result); memcpy(result, model_word, size); return result;
}
void esp_srmodel_deinit(srmodel_list_t *m) { assert(m == &models); }
static model_iface_data_t *create(const char *name, int mode)
{ assert(!strcmp(name, CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL)); (void)mode; ++creates; return model_create_failed ? NULL : &model; }
static int chunk(model_iface_data_t *m) { (void)m; return 160; }
static int rate(model_iface_data_t *m) { (void)m; return model_rate; }
static int channels(model_iface_data_t *m) { (void)m; return model_channels; }
static int detect(model_iface_data_t *m, int16_t *pcm)
{
    assert(s_guard.stage == SR_STAGE_DETECT);
    (void)m; assert(pcm); ++detects;
    if (cancel_in_detect) meoave_espsr_set_armed(false);
    return detect_result;
}
static void clean(model_iface_data_t *m)
{ (void)m; ++cleans; assert(!"unsafe vendor clean must never be called"); }
static void destroy(model_iface_data_t *m) { assert(m && s_guard.stage == SR_STAGE_DESTROY); ++destroys; }
static const esp_wn_iface_t wn = {
    .create = create, .get_samp_chunksize = chunk, .get_samp_rate = rate,
    .get_channel_num = channels, .detect = detect, .clean = clean, .destroy = destroy
};
const esp_wn_iface_t *esp_wn_handle_from_name(const char *name)
{ assert(!strcmp(name, expected_selector)); return &wn; }

static void valid_image(void)
{
    memset(image, 0, sizeof(image)); image[0] = 1;
    memcpy(image + 4, "wn9_maomao", 11); image[36] = 1;
    memcpy(image + 40, "_MODEL_INFO_", 13); image[72] = 80; image[76] = 32;
    memcpy(image + 80, "wakenet9_test_猫猫_3_0.6_0.6", 30);
}
static void drain(void)
{
    audio_block_t block;
    while (xQueueReceive(s_queue, &block, 0) == pdTRUE) process_block(&block);
}
static void feed(void)
{
    int16_t pcm[AUDIO_BLOCK_FRAMES * 2];
    for (unsigned i = 0; i < AUDIO_BLOCK_FRAMES; ++i) {
        pcm[i * 2] = (int16_t)i; pcm[i * 2 + 1] = -1234;
    }
    meoave_espsr_feed_stereo_48k(pcm, AUDIO_BLOCK_FRAMES, clock_ms);
}

int main(void)
{
    assert(!strcmp(model_engine_selector("wakeNet9l_test"), "wn9l_"));
    assert(!strcmp(model_engine_selector("wakenet9s_test"), "wn9s_"));
    assert(!model_engine_selector(NULL) && !model_engine_selector("wakenet10_test") &&
           !model_engine_selector("wakenet9") && !model_engine_selector("wakenet9evil_test"));
    memset(image,0,sizeof(image));
    assert(meoave_espsr_init()==ESP_ERR_NOT_FOUND && !s_queue && !pending_worker);
    s_initialized=false;
    valid_image();
    psram_failed=true;
    assert(meoave_espsr_init()==ESP_ERR_NO_MEM && !strcmp(s_view.error,"PSRAM_ALLOC_FAILED"));
    psram_failed=false;s_initialized=false;
    chunk_failed=true;
    assert(meoave_espsr_init()==ESP_ERR_NO_MEM && !s_queue_storage && !s_chunk &&
           !strcmp(s_view.error,"PCM_ALLOC_FAILED"));
    chunk_failed=false;s_initialized=false;
    internal_failed=true;
    assert(meoave_espsr_init()==ESP_ERR_NO_MEM && !s_queue_storage && !s_worker_stack && !s_chunk &&
           !strcmp(s_view.error,"INTERNAL_STACK_ALLOC_FAILED"));
    internal_failed=false;s_initialized=false;
    queue_failed=true;
    assert(meoave_espsr_init()==ESP_ERR_NO_MEM && !s_queue_storage && !s_worker_stack && !s_chunk);
    queue_failed=false;s_initialized=false;
    task_failed=true;
    assert(meoave_espsr_init()==ESP_ERR_NO_MEM && !s_queue && !s_queue_storage && !s_worker_stack && !s_chunk);
    task_failed=false;s_initialized=false;
    assert(meoave_espsr_init() == ESP_OK && pending_worker && !mappings);
    assert(meoave_espsr_init() == ESP_OK); /* no duplicate queue/worker */
    memset(image, 0xff, sizeof(image));
    assert(!load_model() && !mappings && !strcmp(s_view.error, "MODEL_PACK_INVALID"));
    memset(image, 0, sizeof(image));
    assert(!load_model() && !mappings && !strcmp(s_view.error, "MODEL_MISSING"));
    valid_image();
    model_word = "你好猫猫";
    assert(!load_model() && !strcmp(s_view.error, "MODEL_WORD_MISMATCH"));
    model_word = "猫猫;你好";
    assert(!load_model() && !strcmp(s_view.error, "MODEL_WORD_MISMATCH"));
    model_word = "Hi,喵喵";
    assert(!load_model() && !strcmp(s_view.error, "MODEL_WORD_MISMATCH"));
    model_word = MEOAVE_ESPSR_WAKE_KEYWORD;
    model_rate = 48000;
    assert(!load_model() && !strcmp(s_view.error, "BAD_MODEL_AUDIO_FORMAT"));
    model_rate = 16000; model_channels = 2;
    assert(!load_model());
    model_channels = 1;
    metadata[0] = "wakenet9l_tts1h8_猫猫_3_0.6_0.6";
    expected_selector = "wn9l_"; /* legacy wn9 asset alias must select 9l */
    assert(load_model());
    meoave_espsr_set_armed(true);
    feed(); assert(!detects && queue.count == 1); /* RX only copies; never infers */
    assert(queue.blocks[queue.head].mono[1] == 1); /* calibrated left channel */
    unsigned initial_creates = creates, initial_destroys = destroys;
    drain(); assert(detects == 1 && !cleans && creates == initial_creates);
    feed(); meoave_espsr_set_armed(false); meoave_espsr_set_armed(true);
    drain(); assert(detects == 1); /* old epoch discarded despite re-arm */
    feed(); drain(); assert(detects == 2 && !cleans && creates == initial_creates + 1 && destroys == initial_destroys + 1);
    feed(); clock_ms += MAX_AUDIO_AGE_MS + 1; drain();
    assert(detects == 2); /* no inference on stale audio */
    meoave_espsr_set_output_blocked(true, clock_ms);
    feed(); assert(!queue.count && !s_view.armed);
    meoave_espsr_set_output_blocked(false, clock_ms + 299); assert(!s_view.armed);
    clock_ms += 300;
    meoave_espsr_set_output_blocked(false, clock_ms); assert(s_view.armed);
    for (unsigned i = 0; i < AUDIO_QUEUE_BLOCKS + 1; ++i) feed();
    assert(s_view.queue_drops == 1 && !s_view.detections);
    drain(); unsigned resets = creates;
    feed(); drain(); assert(creates == resets + 1 && !cleans); /* overflow is a discontinuity */
    detect_result = 1; cancel_in_detect = true;
    feed(); drain(); assert(!s_view.detections); /* result invalidated during infer */
    cancel_in_detect = false;
    meoave_espsr_set_armed(true); feed(); drain();
    assert(s_view.detections == 1);
    feed(); drain(); assert(s_view.detections == 1); /* cooldown */
    clock_ms += DETECTION_COOLDOWN_MS; feed(); drain(); assert(s_view.detections == 2);
    meoave_espsr_view_t snapshot;
    meoave_espsr_view(&snapshot);
    assert(snapshot.keyword_ready && snapshot.inferred_chunks == detects && snapshot.queue_drops == 1);
    assert(snapshot.worker_stack_free == 3000 && s_guard.stage == SR_STAGE_IDLE);
    /* Rebuild allocation failure inhibits input, without reusing old state. */
    model_create_failed = true;
    assert(!reset_stream() && !s_wn_data && !s_view.keyword_ready && !s_view.armed);
    assert(!strcmp(s_view.error, "MODEL_CREATE_FAILED"));
    model_create_failed = false;
    /* Emulate retained LP RAM after a panic. No model, RTOS or heap work may
       run on the guarded boot; preserve the evidence until new ELF/cold boot. */
    guard_write(SR_STAGE_CREATE, false);
    reset_reason = ESP_RST_PANIC;
    s_initialized = false;
    unsigned before = heap_allocations;
    assert(meoave_espsr_init() == ESP_ERR_INVALID_STATE && heap_allocations == before);
    assert(s_view.crash_guarded && s_view.crash_stage == SR_STAGE_CREATE && s_guard.blocked);
    assert(!strcmp(s_view.error, "PREVIOUS_WAKE_CRASH") && !s_view.armed);
    reset_reason = ESP_RST_SW;
    assert(!guard_boot()); /* a software restart is not an implicit retry */
    app_descriptor.app_elf_sha256[0]++;
    assert(guard_boot() && !s_guard.blocked && s_guard.stage == SR_STAGE_IDLE);
    guard_write(SR_STAGE_DETECT, false);
    reset_reason = ESP_RST_TASK_WDT;
    assert(!guard_boot() && s_view.crash_stage == SR_STAGE_DETECT);
    reset_reason = ESP_RST_POWERON;
    assert(guard_boot() && !s_guard.blocked); /* power loss invalidates old blame */
    guard_write(SR_STAGE_CREATE, false);
    s_guard.check ^= 1;
    reset_reason = ESP_RST_PANIC;
    assert(guard_boot()); /* corrupt/random RTC bytes must not falsely block */
    puts("PASS: allocation failures, internal stack, crash guard, exact phrase, RX isolation, epochs, PA tail, drops, cooldown");
    return 0;
}
