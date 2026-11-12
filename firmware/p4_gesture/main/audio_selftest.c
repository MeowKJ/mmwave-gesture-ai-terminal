/* Acoustic loopback self test + microphone mux calibration.
 * See audio_selftest.h for why DAC-mute and PA-off are BOTH required. */
#include "audio_selftest.h"

#include "bsp_audio.h"
#include "bsp_usb.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOOP_BLOCK_FRAMES   480          /* 10 ms @ 48 kHz */
#define LOOP_FREQ_HZ        1000
#define LOOP_AMPLITUDE      9000         /* about -11 dBFS */
#define LOOP_TASK_STACK     6144
#define LOOP_REASON_MAX     AUDIO_LOOP_REASON_LEN

static audio_loop_result_t s_result;
static volatile bool s_busy;

static void emit(const char *token, const char *fmt, ...)
{
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    char line[240];
    snprintf(line, sizeof(line), "MEO:AUDIO_LOOP token=%s %s\n", token, body);
    (void)bsp_usb_cdc_write(line);
}

/* Goertzel magnitude of `freq_hz` for one channel of an interleaved stereo
 * block. DC is removed first so a constant line / offset cannot fake signal.
 * The returned value is proportional to the tone amplitude (not squared),
 * which keeps the PASS thresholds intuitive and the numbers small. */
static uint32_t goertzel_mag(const int16_t *x, size_t frames, int ch, int freq_hz)
{
    const int n = (int)frames;
    if (n < 8) return 0;
    double mean = 0;
    for (int i = 0; i < n; ++i) mean += x[2 * i + ch];
    mean /= n;

    const double omega = 2.0 * 3.14159265358979 * (double)freq_hz /
                         (double)BSP_AUDIO_SAMPLE_RATE;
    const double coeff = 2.0 * cos(omega);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        double s0 = ((double)x[2 * i + ch] - mean) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (power < 0 || !isfinite(power)) power = 0;
    return (uint32_t)(sqrt(power) / 16.0);
}

/* Drive TX and sample RX in lockstep for `ms`, returning the mean Goertzel
 * magnitude per channel. TX/RX share one I2S controller but have independent
 * DMA; writing one block then reading one block keeps them aligned. */
static void measure(uint32_t out[2], int ms, bool play_tone, uint32_t *phase_q16)
{
    static int16_t tx[LOOP_BLOCK_FRAMES * 2];
    static int16_t rx[LOOP_BLOCK_FRAMES * 2];
    uint64_t acc[2] = {0, 0};
    int valid = 0;
    int blocks = ms / 10;
    if (blocks < 1) blocks = 1;
    if (blocks > 600) blocks = 600;

    for (int b = 0; b < blocks; ++b) {
        if (play_tone) {
            bsp_audio_fill_tone(tx, LOOP_BLOCK_FRAMES, LOOP_FREQ_HZ,
                                LOOP_AMPLITUDE, phase_q16);
        } else {
            memset(tx, 0, sizeof(tx));
        }
        (void)bsp_audio_tx_write(tx, LOOP_BLOCK_FRAMES, NULL);
        size_t got = 0;
        if (bsp_audio_rx_read(rx, LOOP_BLOCK_FRAMES, &got) == ESP_OK && got >= 8) {
            acc[0] += goertzel_mag(rx, got, 0, LOOP_FREQ_HZ);
            acc[1] += goertzel_mag(rx, got, 1, LOOP_FREQ_HZ);
            ++valid;
        }
    }
    out[0] = valid ? (uint32_t)(acc[0] / (uint32_t)valid) : 0;
    out[1] = valid ? (uint32_t)(acc[1] / (uint32_t)valid) : 0;
}

static uint32_t best_ch(const uint32_t v[2]) { return v[0] > v[1] ? v[0] : v[1]; }

static void restore_state(int volume, bool pa_on, bool mic_running)
{
    (void)bsp_audio_set_volume(volume);
    (void)bsp_audio_dac_mute(false);
    (void)bsp_audio_adc_mute(false);
    (void)bsp_audio_speaker(pa_on);
    if (!mic_running) (void)bsp_audio_mic_stop();
    bsp_audio_mic_pause(false);
}

static void loopback_task(void *arg)
{
    /* arg points at a static "token\0<ms>" payload copied by the caller. */
    const char *payload = (const char *)arg;
    char token[17] = {0};
    strncpy(token, payload, sizeof(token) - 1);
    int ms = atoi(payload + strlen(token) + 1);
    if (ms <= 0 || ms > 5000) ms = 800;

    bsp_audio_status_t saved;
    bsp_audio_status(&saved);
    const bool mic_running = saved.mic_running;

    memset(&s_result, 0, sizeof(s_result));
    s_result.state = AUDIO_LOOP_RUNNING;
    const int64_t t0 = esp_timer_get_time() / 1000;

    /* RX must have exactly one reader. The level meter stands down, but the
       task object stays alive so nothing deletes a task blocked in I2S. */
    bsp_audio_mic_pause(true);
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)bsp_audio_adc_mute(false);
    (void)bsp_audio_dac_mute(false);
    (void)bsp_audio_set_volume(BSP_AUDIO_TEST_VOLUME_PCT);
    (void)bsp_audio_speaker(true);
    vTaskDelay(pdMS_TO_TICKS(60));             /* amp out of shutdown */

    uint32_t phase = 0;
    measure(s_result.baseline, 200, false, &phase);
    measure(s_result.tone, ms, true, &phase);

    /* A digital TX->RX loop or electrical crosstalk lives BEFORE the DAC
       analog output and BEFORE the amplifier. Killing either one must remove
       the tone from the microphone; only an acoustic path satisfies both. */
    (void)bsp_audio_dac_mute(true);
    vTaskDelay(pdMS_TO_TICKS(40));
    measure(s_result.dac_mute, 300, true, &phase);
    (void)bsp_audio_dac_mute(false);

    (void)bsp_audio_speaker(false);
    vTaskDelay(pdMS_TO_TICKS(40));
    measure(s_result.pa_off, 300, true, &phase);
    (void)bsp_audio_speaker(true);
    vTaskDelay(pdMS_TO_TICKS(60));

    measure(s_result.restore, 250, true, &phase);

    restore_state(saved.volume, saved.pa_on, mic_running);
    s_result.elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000 - t0);

    const uint32_t tone = best_ch(s_result.tone);
    const uint32_t base = best_ch(s_result.baseline);
    const uint32_t dacm = best_ch(s_result.dac_mute);
    const uint32_t paof = best_ch(s_result.pa_off);
    const uint32_t back = best_ch(s_result.restore);

    /* Integer-only comparisons: no floating point in the verdict. */
    /* Relative to the measured floor, not an absolute magnitude: at the quiet
       20% test level the tone is ~30 dB below the old 0 dB bench level, yet
       still tens of times above the noise floor. A disconnected speaker falls
       back to the floor and must FAIL here. */
    const bool strong   = tone >= 30 && tone >= base + 20 && tone >= base * 4;
    const bool dac_kill = dacm * 100 <= tone * 40;
    const bool pa_kill  = paof * 100 <= tone * 40;
    const bool recover  = back * 100 >= tone * 40;   /* amp re-bias takes ~100 ms */
    const bool ok = strong && dac_kill && pa_kill && recover;

    /* Confidence: how far the two physical kills sit below the live tone. */
    int score = 0;
    if (strong) score += 35;
    if (dac_kill) score += 25;
    if (pa_kill) score += 25;
    if (recover) score += 15;
    s_result.score = score;
    s_result.state = ok ? AUDIO_LOOP_PASS : AUDIO_LOOP_FAIL;

    if (!strong) snprintf(s_result.reason, LOOP_REASON_MAX,
                          "no acoustic tone (tone=%lu base=%lu)",
                          (unsigned long)tone, (unsigned long)base);
    else if (!dac_kill) snprintf(s_result.reason, LOOP_REASON_MAX,
                                 "DAC mute did not remove tone -> digital path");
    else if (!pa_kill) snprintf(s_result.reason, LOOP_REASON_MAX,
                                 "PA off did not remove tone -> not via speaker");
    else if (!recover) snprintf(s_result.reason, LOOP_REASON_MAX,
                          "tone did not return after restore");
    else snprintf(s_result.reason, LOOP_REASON_MAX, "physical DAC->PA->air->MIC->ADC path");

    emit(token, "phase=baseline l=%lu r=%lu",
         (unsigned long)s_result.baseline[0], (unsigned long)s_result.baseline[1]);
    emit(token, "phase=tone l=%lu r=%lu",
         (unsigned long)s_result.tone[0], (unsigned long)s_result.tone[1]);
    emit(token, "phase=dac_mute l=%lu r=%lu",
         (unsigned long)s_result.dac_mute[0], (unsigned long)s_result.dac_mute[1]);
    emit(token, "phase=pa_off l=%lu r=%lu",
         (unsigned long)s_result.pa_off[0], (unsigned long)s_result.pa_off[1]);
    emit(token, "phase=restore l=%lu r=%lu",
         (unsigned long)s_result.restore[0], (unsigned long)s_result.restore[1]);
    emit(token, "verdict=%s score=%d elapsed_ms=%lu vol=%d testvol=%d reason=%s",
         ok ? "PASS" : "FAIL", score, (unsigned long)s_result.elapsed_ms,
         saved.volume, BSP_AUDIO_TEST_VOLUME_PCT, s_result.reason);

    s_busy = false;
    vTaskDelete(NULL);
}

/* Sweep the ES8389 ADC input mux while a known tone plays, then keep the two
 * codes that actually carry a microphone signal. This is how the crossed
 * P/N harness is compensated without touching the wiring. */
static void calibrate_task(void *arg)
{
    char token[17] = {0};
    strncpy(token, (const char *)arg, sizeof(token) - 1);

    bsp_audio_status_t saved;
    bsp_audio_status(&saved);
    const bool mic_running = saved.mic_running;

    bsp_audio_mic_pause(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    (void)bsp_audio_adc_mute(false);
    (void)bsp_audio_dac_mute(false);
    (void)bsp_audio_set_volume(BSP_AUDIO_TEST_VOLUME_PCT);
    (void)bsp_audio_speaker(true);
    vTaskDelay(pdMS_TO_TICKS(60));

    static const uint8_t sels[] = {5, 6, 1};   /* single P, other P, differential */
    uint8_t best_l = 6, best_r = 5;
    uint32_t best_score = 0;

    for (unsigned li = 0; li < sizeof(sels); ++li) {
        for (unsigned ri = 0; ri < sizeof(sels); ++ri) {
            bsp_audio_mic_route_t r = {.left_sel = sels[li], .right_sel = sels[ri],
                                       .gain = saved.route.gain,
                                       .swap = saved.route.swap};
            (void)bsp_audio_mic_route_set(&r);
            vTaskDelay(pdMS_TO_TICKS(25));
            uint32_t phase = 0;
            uint32_t mag[2] = {0, 0};
            measure(mag, 220, true, &phase);
            uint32_t score = mag[0] + mag[1];
            emit(token, "cal lsel=%u rsel=%u l=%lu r=%lu score=%lu",
                 sels[li], sels[ri], (unsigned long)mag[0],
                 (unsigned long)mag[1], (unsigned long)score);
            if (score > best_score) {
                best_score = score;
                best_l = sels[li];
                best_r = sels[ri];
            }
        }
    }

    bsp_audio_mic_route_t chosen = {.left_sel = best_l, .right_sel = best_r,
                                    .gain = saved.route.gain,
                                    .swap = saved.route.swap};
    (void)bsp_audio_mic_route_set(&chosen);
    restore_state(saved.volume, saved.pa_on, mic_running);

    s_result.best_left_sel = best_l;
    s_result.best_right_sel = best_r;
    emit(token, "cal verdict=BEST lsel=%u rsel=%u score=%lu", best_l, best_r,
         (unsigned long)best_score);
    s_busy = false;
    vTaskDelete(NULL);
}

bool audio_selftest_busy(void) { return s_busy; }

void audio_selftest_result(audio_loop_result_t *out)
{
    if (out) *out = s_result;
}

void audio_selftest_start(const char *token, int ms)
{
    if (s_busy) {
        emit(token, "verdict=BUSY");
        return;
    }
    /* payload layout: "<token>\0<ms>" - read back by the worker. */
    static char payload[24];
    const char *src = token ? token : "none";
    size_t n = strlen(src);
    if (n > 16) n = 16;
    memcpy(payload, src, n);
    payload[n] = '\0';
    snprintf(payload + n + 1, sizeof(payload) - n - 1, "%d", ms);

    s_busy = true;
    if (xTaskCreate(loopback_task, "audio_loop", LOOP_TASK_STACK, payload, 3, NULL)
        != pdPASS) {
        s_busy = false;
        emit(token, "verdict=FAIL reason=no-task");
    }
}

void audio_selftest_calibrate(const char *token)
{
    if (s_busy) {
        emit(token, "verdict=BUSY");
        return;
    }
    static char cal_token[17];
    strncpy(cal_token, token ? token : "none", sizeof(cal_token) - 1);
    s_busy = true;
    if (xTaskCreate(calibrate_task, "audio_cal", LOOP_TASK_STACK, cal_token, 3, NULL)
        != pdPASS) {
        s_busy = false;
        emit(token, "verdict=FAIL reason=no-task");
    }
}

bool audio_selftest_command(const char *line)
{
    if (!line || strncmp(line, "AUDIO ", 6) != 0) return false;
    char verb[16] = {0}, token[17] = {0};
    int ms = 0;
    int got = sscanf(line, "AUDIO %15s %16s %d", verb, token, &ms);
    if (got < 2) return true;

    if (strcmp(verb, "LOOPBACK") == 0) {
        audio_selftest_start(token, got >= 3 ? ms : 800);
    } else if (strcmp(verb, "CALIBRATE") == 0) {
        audio_selftest_calibrate(token);
    } else if (strcmp(verb, "STATUS") == 0 || strcmp(verb, "RESULT") == 0) {
        audio_loop_result_t r;
        audio_selftest_result(&r);
        bsp_audio_status_t st;
        bsp_audio_status(&st);
        emit(token,
             "status state=%d busy=%d score=%d codec=%d addr=0x%02X mclk=%d%% "
             "vol=%d reg=0x%02X pa=%d mic=%d route=%u/%u gain=%u swap=%d "
             "tone=%lu/%lu dacmute=%lu/%lu paoff=%lu/%lu reason=%s",
             (int)r.state, (int)s_busy, r.score, (int)st.codec_present, st.codec_addr,
             st.mclk_duty, st.volume, st.volume_reg, (int)st.pa_on,
             (int)st.mic_running, st.route.left_sel, st.route.right_sel,
             st.route.gain, (int)st.route.swap,
             (unsigned long)r.tone[0], (unsigned long)r.tone[1],
             (unsigned long)r.dac_mute[0], (unsigned long)r.dac_mute[1],
             (unsigned long)r.pa_off[0], (unsigned long)r.pa_off[1], r.reason);
    } else {
        emit(token, "verdict=FAIL reason=unknown-verb");
    }
    return true;
}
