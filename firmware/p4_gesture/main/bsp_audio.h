#pragma once

/* Audio BSP - ES8389 codec + NS4150B amplifier on ESP32-P4.
 *
 * Layering (all in one translation unit, separated by explicit sections):
 *   1. codec  : ES8389 register tables, clock/pad bring-up, volume, input mux
 *   2. stream : I2S TX/RX primitives, non-blocking tone generator, capture task
 *   3. status : one snapshot struct for UI / diagnostics
 *   4. shell  : USB CDC commands (MIC / SPK / AUDIO)
 *
 * Nothing in this file assumes the microphone harness is wired correctly:
 * the ADC input mux is described by bsp_audio_mic_route_t and can be
 * discovered at runtime by audio_selftest.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BSP_AUDIO_SAMPLE_RATE   48000
#define BSP_AUDIO_FRAME_BYTES   4       /* 16-bit stereo */
/* Every test/self-test sound plays at this level, independent of the user's
 * normal listening volume. 0 dB is far too loud for repeated bench tests. */
#define BSP_AUDIO_TEST_VOLUME_PCT 20

/* ---------------- 1. lifecycle / codec ---------------- */
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_write_reg(uint8_t reg, uint8_t val);
esp_err_t bsp_audio_read_reg(uint8_t reg, uint8_t *val);
bool      bsp_audio_codec_present(void);

/* DAC + ADC format mute together (0x40 / 0x20 bits 0..1). */
esp_err_t bsp_audio_set_mute(bool mute);
/* DAC only / ADC only - required by the physical loopback self test. */
esp_err_t bsp_audio_dac_mute(bool mute);
esp_err_t bsp_audio_adc_mute(bool mute);

/* Global playback volume 0..100. 0 = silence, 100 = 0 dB. The perceptual
 * curve is a compile-time table; writes are de-duplicated. Safe to call
 * before the codec answers - the value is remembered and applied on init. */
esp_err_t bsp_audio_set_volume(int percent);
int       bsp_audio_get_volume(void);
/* Raw ES8389 DACL/DACR code 0x00..0xBF, for calibration only. */
esp_err_t bsp_audio_set_volume_raw(uint8_t reg);
uint8_t   bsp_audio_volume_reg(void);

/* ---------------- 2. microphone input routing ---------------- */
/* ES8389 0x72/0x73 bits[6:4] select the ADC input, bits[3:0] the PGA gain.
 * The same code means DIFFERENT physical pins on L and R:
 *   sel 1 : L diff MIC1P-MIC1N / R diff MIC2N-MIC1P
 *   sel 5 : L single MIC1P     / R single MIC2N
 *   sel 6 : L single MIC2P     / R single MIC1N
 * On this board the external mic harness crosses P/N, so the differential
 * code would compare two grounds. Single-ended codes are used instead, and
 * `swap` lets the UI present channels in the physical left/right order. */
typedef struct {
    uint8_t left_sel;
    uint8_t right_sel;
    uint8_t gain;
    bool    swap;
} bsp_audio_mic_route_t;

void      bsp_audio_mic_route_default(bsp_audio_mic_route_t *route);
esp_err_t bsp_audio_mic_route_set(const bsp_audio_mic_route_t *route);
void      bsp_audio_mic_route_get(bsp_audio_mic_route_t *route);

/* ---------------- 3. streams ---------------- */
/* len is in int16 samples for the legacy helpers (kept for callers). */
esp_err_t bsp_audio_play(const int16_t *samples, size_t len);
esp_err_t bsp_audio_record(int16_t *samples, size_t len, size_t *read);
/* Frame-oriented primitives used by the self test. `frames` counts stereo
 * frames, not samples; got/written are also in frames. */
esp_err_t bsp_audio_tx_write(const int16_t *stereo, size_t frames, size_t *written_frames);
esp_err_t bsp_audio_rx_read(int16_t *stereo, size_t frames, size_t *got_frames);

/* Lookup-table sine, phase in Q16 (0..65535 = one cycle). No double, no sin()
 * per sample - safe to call from a 48 kHz streaming task. */
void bsp_audio_fill_tone(int16_t *stereo, size_t frames, int freq_hz,
                         int16_t amplitude, uint32_t *phase_q16);

/* Blocking tone; call from a worker task, never from the USB/UI path. */
esp_err_t bsp_audio_tone(int freq_hz, int ms);
/* Same tone at BSP_AUDIO_TEST_VOLUME_PCT, restoring the user's listening
 * volume afterwards. Used by every "test / self-test" trigger so bench runs
 * never blast at 0 dB. */
esp_err_t bsp_audio_test_tone(int freq_hz, int ms);
void      bsp_audio_beep(uint32_t ms);

/* Amplifier (PA_CTRL = GPIO24), high = on. */
esp_err_t bsp_audio_speaker(bool on);
bool      bsp_audio_speaker_on(void);

/* Continuous level meter. The self test pauses it so RX has a single owner. */
esp_err_t bsp_audio_mic_start(void);
void      bsp_audio_mic_stop(void);
void      bsp_audio_mic_pause(bool pause);
void      bsp_audio_mic_level(int rms[2], int peak[2]);
bool      bsp_audio_mic_active(void);

/* ---------------- 4. status snapshot ---------------- */
typedef struct {
    bool     codec_present;
    uint8_t  codec_addr;
    int      init_error;
    int      volume;
    uint8_t  volume_reg;
    int      mclk_duty;         /* last measured duty of the MCLK pad, % */
    bool     pa_on;
    bool     tx_ready;
    bool     rx_ready;
    bool     mic_running;
    uint32_t i2c_acks;
    bsp_audio_mic_route_t route;
} bsp_audio_status_t;

void bsp_audio_status(bsp_audio_status_t *out);

/* ---------------- 5. USB shell ---------------- */
/* MIC ... / SPK ... / AUDIO ... ; returns true when the line was consumed. */
bool bsp_audio_command(const char *line);
