#pragma once

/* Production acoustic self test for the ES8389 + NS4150B + microphone chain.
 *
 * The point of this module is to prove a PHYSICAL loop:
 *   DAC -> amplifier -> speaker -> air -> microphone -> PGA -> ADC -> I2S RX
 *
 * A GPIO-matrix TX->RX shortcut (or any electrical crosstalk inside the chip)
 * would also produce a tone in RX, so the test explicitly kills the signal in
 * two independent physical places and requires it to disappear both times:
 *   - DAC digital mute (0x40) : kills the signal before the analog output
 *   - PA shutdown (GPIO24)    : kills the signal before the speaker
 * Only a real acoustic path can satisfy all three conditions.
 *
 * Also discovers the ADC input mux codes that match the mis-wired mic harness,
 * so no board rework is needed.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AUDIO_LOOP_IDLE = 0,
    AUDIO_LOOP_RUNNING,
    AUDIO_LOOP_PASS,
    AUDIO_LOOP_FAIL,
} audio_loop_state_t;

#define AUDIO_LOOP_REASON_LEN 64

typedef struct {
    audio_loop_state_t state;
    /* Goertzel power at the stimulus frequency, per channel (arbitrary units). */
    uint32_t baseline[2];
    uint32_t tone[2];
    uint32_t dac_mute[2];
    uint32_t pa_off[2];
    uint32_t restore[2];
    uint32_t elapsed_ms;
    int      score;                 /* 0..100 confidence */
    char     reason[AUDIO_LOOP_REASON_LEN];
    /* Best ADC mux found by the last calibration run. */
    uint8_t  best_left_sel;
    uint8_t  best_right_sel;
} audio_loop_result_t;

/* Non-blocking: spawns a worker and returns immediately. `token` is echoed
 * in every MEO:AUDIO_LOOP line so a host can correlate a whole run. */
void audio_selftest_start(const char *token, int ms);
void audio_selftest_calibrate(const char *token);
bool audio_selftest_busy(void);
void audio_selftest_result(audio_loop_result_t *out);
bool audio_selftest_command(const char *line);
