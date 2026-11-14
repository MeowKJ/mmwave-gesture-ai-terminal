"""Host-only regression: python tests/test_audio_concurrency.py (GCC + pthreads).

Compiles the production capture/playback sections and complete audio_selftest.c
with in-memory RTOS/I2S doubles. Temporary sources/binaries stay outside the
repository. No ESP-IDF build, serial device, CMake edit or board is involved.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


MAIN = Path(__file__).resolve().parents[1] / "main"
RTOS = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) assert(pthread_mutex_lock(p) == 0)
#define portEXIT_CRITICAL(p) assert(pthread_mutex_unlock(p) == 0)
typedef unsigned TickType_t;
typedef struct task *TaskHandle_t;
typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdPASS 1
int xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(int, TickType_t);
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p)
{ assert(pthread_mutex_init(p, NULL) == 0); return p; }
static inline int xSemaphoreTake(SemaphoreHandle_t p, TickType_t timeout)
{ (void)timeout; return pthread_mutex_lock(p) == 0; }
static inline int xSemaphoreGive(SemaphoreHandle_t p)
{ return pthread_mutex_unlock(p) == 0; }
'''

PRELUDE = r'''
#include <math.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
/* Winsock defines s_addr as an in_addr field alias; production BSP owns a
   codec address variable with this name and does not include Winsock. */
#undef s_addr
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}
#endif
#include "freertos/FreeRTOS.h"
#include "bsp_audio.h"
#include "audio_selftest.h"
#include "audio_preview.h"
#include "audio_click.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK(e) assert((e) == ESP_OK)
#define PIN_PA_CTRL 24
static void *s_tx_chan = (void *)1, *s_rx_chan = (void *)1;
static bool s_pa_on;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_output_leased;
static bool s_voice_owned;
static uint8_t s_addr;
static unsigned s_codec_attempts;
static int s_init_error;
esp_err_t bsp_audio_read_reg(uint8_t reg, uint8_t *value)
{ (void)reg; *value = 0; return ESP_OK; }
static int volume = 63, writes, mutations, creates, mic_creates, self_deletes;
static bool fail_create, fail_tx;
static bool track_cue;
static size_t cue_frames, cue_tail, cue_nonzero;
static unsigned cue_wake_ms;
static char usb_log[512];
static bsp_audio_mic_route_t route = {.gain = 4};
struct task { pthread_t thread; pthread_mutex_t mutex; pthread_cond_t cv;
              unsigned notifications; void (*fn)(void *); void *arg; };
static struct task mic_thread;
static _Thread_local TaskHandle_t current_task;
static void (*pending_fn)(void *);
static void *pending_arg;
static jmp_buf worker_exit;
static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static bool hold_read, read_entered, fail_read;
static int readers, read_calls;
static atomic_bool control_started, control_done;
static void (*io_hook)(void);

static void *task_entry(void *p) {
    current_task = p; current_task->fn(current_task->arg); return NULL;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *out) {
    (void)stack; (void)priority; ++creates;
    if (fail_create) { fail_create = false; return 0; }
    if (!strcmp(name, "mic_level")) {
        ++mic_creates; assert(mic_creates == 1);
        mic_thread.fn = fn; mic_thread.arg = arg;
        assert(pthread_mutex_init(&mic_thread.mutex, NULL) == 0);
        assert(pthread_cond_init(&mic_thread.cv, NULL) == 0);
        *out = &mic_thread;
        assert(pthread_create(&mic_thread.thread, NULL, task_entry, &mic_thread) == 0);
    } else {
        assert(pending_fn == NULL); pending_fn = fn; pending_arg = arg;
    }
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) {
    assert(task == NULL && current_task == NULL); /* Never kill the RX worker. */
    ++self_deletes; longjmp(worker_exit, 1);
}
void vTaskDelay(TickType_t ticks) {
    if (track_cue && s_pa_on && cue_frames==0) cue_wake_ms+=ticks;
    sleep_ms(ticks ? ticks : 1);
}
void xTaskNotifyGive(TaskHandle_t task) {
    assert(pthread_mutex_lock(&task->mutex) == 0);
    ++task->notifications; pthread_cond_signal(&task->cv);
    assert(pthread_mutex_unlock(&task->mutex) == 0);
}
uint32_t ulTaskNotifyTake(int clear, TickType_t timeout) {
    (void)clear; (void)timeout; assert(current_task);
    assert(pthread_mutex_lock(&current_task->mutex) == 0);
    while (!current_task->notifications)
        pthread_cond_wait(&current_task->cv, &current_task->mutex);
    unsigned count = current_task->notifications; current_task->notifications = 0;
    assert(pthread_mutex_unlock(&current_task->mutex) == 0); return count;
}
esp_err_t bsp_audio_record(int16_t *samples, size_t len, size_t *got) {
    assert(current_task == &mic_thread);
    pthread_mutex_lock(&read_lock);
    assert(++readers == 1); ++read_calls; read_entered = true;
    while (hold_read) {
        pthread_mutex_unlock(&read_lock); sleep_ms(1); pthread_mutex_lock(&read_lock);
    }
    bool fail = fail_read;
    pthread_mutex_unlock(&read_lock);
    sleep_ms(2);
    for (size_t i = 0; i < len; ++i) samples[i] = 1000;
    *got = fail ? 0 : len * sizeof(*samples);
    pthread_mutex_lock(&read_lock); --readers; pthread_mutex_unlock(&read_lock);
    return fail ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_audio_rx_read(int16_t *samples, size_t frames, size_t *got) {
    pthread_mutex_lock(&read_lock); assert(++readers == 1);
    memset(samples, 0, frames * BSP_AUDIO_FRAME_BYTES); *got = frames;
    --readers; pthread_mutex_unlock(&read_lock); return ESP_OK;
}
esp_err_t bsp_audio_tx_write(const int16_t *samples, size_t frames, size_t *got) {
    assert(!bsp_audio_output_try_acquire()); ++writes;
    if (track_cue && !fail_tx) {
        /* NS4150B Tst is 120 ms typical. A short cue must not decay while
           the amplifier is still waking; allow a 30 ms board margin. */
        assert(cue_wake_ms>=150);
        cue_frames+=frames;
        for(size_t i=0;i<frames;++i) {
            if(samples[2*i] || samples[2*i+1]) {++cue_nonzero;cue_tail=0;}
            else ++cue_tail;
        }
    }
    if (io_hook) { void (*fn)(void) = io_hook; io_hook = NULL; fn(); }
    if (got) *got = frames;
    return fail_tx ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_audio_play(const int16_t *samples, size_t len)
{ return bsp_audio_tx_write(samples, len / 2, NULL); }
esp_err_t bsp_audio_set_volume(int value) { ++mutations; volume = value; return ESP_OK; }
int bsp_audio_get_volume(void) { return volume; }
esp_err_t bsp_audio_dac_mute(bool mute) { (void)mute; ++mutations; return ESP_OK; }
esp_err_t bsp_audio_adc_mute(bool mute) { (void)mute; ++mutations; return ESP_OK; }
esp_err_t bsp_audio_set_mute(bool mute) { (void)mute; ++mutations; return ESP_OK; }
static esp_err_t gpio_set_level(int pin, int value)
{ (void)pin; (void)value; ++mutations; return ESP_OK; }
static int gpio_get_level(int pin) { (void)pin; return s_pa_on; }
esp_err_t bsp_usb_cdc_write(const char *text)
{ snprintf(usb_log, sizeof(usb_log), "%s", text); return ESP_OK; }
int64_t esp_timer_get_time(void) { static int64_t t; return ++t * 1000; }
void bsp_audio_status(bsp_audio_status_t *s) {
    memset(s, 0, sizeof(*s)); s->volume = volume; s->pa_on = s_pa_on;
    s->mic_running = bsp_audio_mic_active(); s->route = route;
}
esp_err_t bsp_audio_mic_route_set(const bsp_audio_mic_route_t *r)
{ route = *r; ++mutations; return ESP_OK; }
'''

TESTS = r'''
static void assert_available(void) {
    assert(bsp_audio_output_try_acquire()); bsp_audio_output_release();
}
static void run_pending(void) {
    assert(pending_fn); void (*fn)(void *) = pending_fn; void *arg = pending_arg;
    pending_fn = NULL;
    if (!setjmp(worker_exit)) { fn(arg); assert(!"worker must self-delete"); }
    assert_available(); assert(!audio_selftest_busy()); assert(volume == 63);
}
static void *lease_contender(void *unused) {
    (void)unused;
    static atomic_int owners;
    for (unsigned i = 0; i < 20000; ++i) {
        if (!bsp_audio_output_try_acquire()) continue;
        assert(atomic_fetch_add(&owners, 1) == 0);
        assert(atomic_fetch_sub(&owners, 1) == 1);
        bsp_audio_output_release();
    }
    return NULL;
}
static void wait_for_read(void) {
    for (unsigned i = 0; i < 2000; ++i) {
        pthread_mutex_lock(&read_lock); bool entered = read_entered;
        pthread_mutex_unlock(&read_lock);
        if (entered) return;
        sleep_ms(1);
    }
    assert(!"capture worker did not read");
}
static void *control_call(void *pause) {
    atomic_store(&control_started, true);
    if (pause) bsp_audio_mic_pause(true); else bsp_audio_mic_stop();
    atomic_store(&control_done, true); return NULL;
}
static void blocked_read_control(bool pause, bool timeout) {
    pthread_mutex_lock(&read_lock);
    hold_read = true; read_entered = false; fail_read = timeout;
    pthread_mutex_unlock(&read_lock);
    assert(bsp_audio_mic_start() == ESP_OK); wait_for_read();
    atomic_store(&control_started, false); atomic_store(&control_done, false);
    pthread_t control; assert(!pthread_create(&control, NULL, control_call, pause ? (void *)1 : NULL));
    while (!atomic_load(&control_started)) sleep_ms(1);
    sleep_ms(35); assert(!atomic_load(&control_done));
    pthread_mutex_lock(&read_lock); hold_read = false; pthread_mutex_unlock(&read_lock);
    assert(!pthread_join(control, NULL)); assert(atomic_load(&control_done));
    pthread_mutex_lock(&read_lock); assert(readers == 0); int before = read_calls;
    pthread_mutex_unlock(&read_lock);
    sleep_ms(30);
    pthread_mutex_lock(&read_lock); assert(read_calls == before); fail_read = false;
    pthread_mutex_unlock(&read_lock);
    assert(bsp_audio_mic_active() == pause);
    if (pause) { bsp_audio_mic_stop(); bsp_audio_mic_pause(false); }
    int rms[2], peak[2]; bsp_audio_mic_level(rms, peak);
    assert(rms[0] == 0 && rms[1] == 0 && peak[0] == 0 && peak[1] == 0);
}
static void enter_mic_during_test(void) { assert(bsp_audio_mic_start() == ESP_OK); }
static void leave_mic_during_test(void) { bsp_audio_mic_stop(); }
int main(void) {
    /* A failed voice drain may retain DMA ownership after releasing its
       ordinary lease. No legacy preview/cue may overwrite that stale ring. */
    s_voice_owned=true;
    assert(!bsp_audio_output_try_acquire());
    assert(bsp_audio_tone(1000,10)==ESP_ERR_INVALID_STATE);
    assert(bsp_audio_click()==ESP_ERR_INVALID_STATE);
    assert(writes==0 && mutations==0);
    s_voice_owned=false;
    pthread_t contenders[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&contenders[i], NULL, lease_contender, NULL));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(contenders[i], NULL));
    assert_available(); puts("PASS: 160000 concurrent lease attempts, one owner");

    fail_create = true; assert(bsp_audio_mic_start() == ESP_ERR_NO_MEM);
    assert(!bsp_audio_mic_active());
    blocked_read_control(false, false); blocked_read_control(true, false);
    blocked_read_control(false, true); blocked_read_control(true, true);
    for (unsigned i = 0; i < 100; ++i) {
        assert(bsp_audio_mic_start() == ESP_OK); bsp_audio_mic_stop();
    }
    assert(mic_creates == 1);
    puts("PASS: stop/pause wait for blocked and failed reads; 100 restarts reuse worker");

    assert(bsp_audio_output_try_acquire()); int before = creates, changed = mutations;
    audio_selftest_start("reserved", 10); audio_selftest_calibrate("reserved");
    assert(bsp_audio_command("SPK TONE 10")); bsp_audio_beep(10);
    assert(bsp_audio_tone(1000, 10) == ESP_ERR_INVALID_STATE);
    assert(bsp_audio_test_tone(1000, 10) == ESP_ERR_INVALID_STATE);
    assert(creates == before && mutations == changed);
    assert(bsp_audio_preview_music() == ESP_OK); /* Caller already holds lease. */
    assert(!bsp_audio_output_try_acquire()); bsp_audio_output_release();
    puts("PASS: reserved preview blocks all engineering playback without IO; no nested acquire");

    for (unsigned i = 0; i < 3; ++i) {
        fail_create = true;
        if (i == 0) audio_selftest_start("fail", 10);
        else if (i == 1) audio_selftest_calibrate("fail");
        else assert(bsp_audio_command("SPK TONE 10"));
        assert_available(); assert(!audio_selftest_busy()); assert(!pending_fn);
    }
    audio_selftest_start("loop", 10); assert(audio_selftest_busy());
    assert(!bsp_audio_output_try_acquire()); before = creates;
    audio_selftest_calibrate("busy"); assert(creates == before);
    io_hook = enter_mic_during_test; run_pending(); assert(bsp_audio_mic_active());
    audio_selftest_calibrate("cal"); assert(!bsp_audio_output_try_acquire());
    io_hook = leave_mic_during_test; run_pending(); assert(!bsp_audio_mic_active());
    assert(bsp_audio_command("SPK TONE 10")); assert(!bsp_audio_output_try_acquire());
    run_pending(); assert(self_deletes == 3);
    puts("PASS: loop/calibration/tone reserve before spawn; release on completion/failure; capture intent preserved");

    track_cue=true;
    assert(bsp_audio_click()==ESP_OK);assert_available();
    assert(!s_pa_on && cue_nonzero>0);
    assert(cue_frames==MEO_CLICK_FRAMES+4*480 && cue_tail>=1440);
    track_cue=false;
    assert(bsp_audio_output_try_acquire());changed=mutations;
    assert(bsp_audio_click()==ESP_ERR_INVALID_STATE && mutations==changed);
    bsp_audio_output_release();
    fail_tx=true;assert(bsp_audio_click()==ESP_FAIL);
    assert_available();assert(!s_pa_on);fail_tx=false;
    puts("PASS: actual cue waits for PA startup, drains DMA silence, preserves volume, releases PA/lease on failure");
    assert(bsp_audio_tone(1000, 10) == ESP_OK); assert_available();
    assert(bsp_audio_tone(0, 10) == ESP_ERR_INVALID_ARG); assert_available();
    assert(bsp_audio_test_tone(1000, 10) == ESP_OK); assert_available();
    bsp_audio_beep(10); assert_available(); assert(volume == 63);
    fail_tx = true; assert(bsp_audio_output_try_acquire());
    assert(bsp_audio_preview_music() == ESP_FAIL);
    assert(!bsp_audio_output_try_acquire()); bsp_audio_output_release(); assert_available();
    assert(!s_pa_on && mic_creates == 1);
    puts("PASS: direct tones/beep release; preview error retains caller lease and closes PA");
    return 0; /* OS reclaims the sleeping persistent mock worker. */
}
'''


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main():
    compiler = shutil.which(os.environ.get("CC", "gcc"))
    if not compiler:
        raise SystemExit("GCC with pthread support is required")
    bsp = (MAIN / "bsp_audio.c").read_text(encoding="utf-8")
    pieces = [
        PRELUDE,
        section(bsp, "bool bsp_audio_output_try_acquire(", "esp_err_t bsp_audio_tx_write("),
        section(bsp, "#define TONE_LUT_SIZE", "static uint8_t volume_to_reg("),
        section(bsp, "void bsp_audio_beep(", "bool bsp_audio_codec_present("),
        section(bsp, "#define MIC_BLOCK_FRAMES", "/* ---- global playback volume"),
        section(bsp, "void bsp_audio_mic_level(", "void bsp_audio_status("),
        section(bsp, "bool bsp_audio_command(", '    if (!line || strncmp(line, "MIC ')
        + "    return false;\n}\n",
        (MAIN / "audio_selftest.c").read_text(encoding="utf-8"),
        TESTS,
    ]
    with tempfile.TemporaryDirectory(prefix="meoave-audio-") as temp:
        root = Path(temp)
        stubs = {
            "freertos/FreeRTOS.h": RTOS,
            "freertos/task.h": '#include "FreeRTOS.h"\n',
            "freertos/semphr.h": '#include "FreeRTOS.h"\n',
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n"
                         "#define ESP_FAIL -1\n#define ESP_ERR_INVALID_STATE 1\n"
                         "#define ESP_ERR_INVALID_ARG 2\n#define ESP_ERR_NO_MEM 3\n",
            "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
            "bsp_usb.h": '#include "esp_err.h"\nesp_err_t bsp_usb_cdc_write(const char *);\n',
        }
        for name, text in stubs.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        source = root / "audio_regression.c"
        source.write_text("\n".join(pieces), encoding="utf-8")
        binary = root / ("audio_regression.exe" if os.name == "nt" else "audio_regression")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-I", str(root), "-I", str(MAIN), str(source), "-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
