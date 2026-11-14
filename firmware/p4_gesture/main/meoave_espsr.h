#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/*
 * ESP-SR WakeNet adapter.
 *
 * The audio BSP is the only owner of I2S RX.  The adapter therefore accepts
 * the already captured 48 kHz stereo blocks and performs the 48 kHz -> 16 kHz
 * mono conversion before passing exact WakeNet-sized chunks to ESP-SR.
 *
 * The adapter deliberately reports MODEL_MISSING/COMPONENT_MISSING instead of
 * silently falling back to the old text bridge.  "猫猫" is not one of the
 * stock Espressif wake words; a custom WakeNet asset named by
 * CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL must be installed before real acoustic
 * detection can be claimed.
 */

#ifndef CONFIG_APP_MEOAVE_ESPSR_WAKE_WORD
#define CONFIG_APP_MEOAVE_ESPSR_WAKE_WORD "猫猫"
#endif
#define MEOAVE_ESPSR_WAKE_KEYWORD CONFIG_APP_MEOAVE_ESPSR_WAKE_WORD

typedef struct {
    bool component_linked;
    bool model_loaded;
    bool keyword_ready;
    bool requested;
    bool output_blocked;
    bool armed;
    uint32_t queued_frames;
    uint32_t queue_drops;
    uint32_t inferred_chunks;
    uint32_t stream_resets;
    bool crash_guarded;
    uint32_t crash_stage;
    uint32_t worker_stack_free;
    uint32_t detections;
    uint64_t last_detected_ms;
    int feed_rate_hz;
    int feed_chunk_samples;
    char backend[24];
    char model[48];
    char model_word[32];
    char error[32];
} meoave_espsr_view_t;

esp_err_t meoave_espsr_init(void);
void meoave_espsr_set_armed(bool armed);
/* Immediate input-tap inhibition, with a 300 ms quiet tail after PA activity. */
void meoave_espsr_set_output_blocked(bool blocked, uint64_t now_ms);

/* Copies to a bounded zero-wait queue. Never runs inference on the RX task. */
void meoave_espsr_feed_stereo_48k(const int16_t *stereo, size_t frames,
                                  uint64_t now_ms);

void meoave_espsr_view(meoave_espsr_view_t *out);
