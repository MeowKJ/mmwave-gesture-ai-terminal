#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meoave_espsr.h"

/*
 * Wake-word boundary for the product UI.
 *
 * The P4 already owns the microphone stream and the visual feedback.  The
 * acoustic model is deliberately kept behind this small contract so it can
 * be supplied by ESP-SR or a C5/server ASR bridge without coupling the UI to
 * one inference implementation.  Until that model is installed, the text
 * bridge is useful for deterministic hardware validation: an upstream
 * recognizer can submit the recognized phrase and the exact same visual path
 * is exercised.
 */
#define MEO_WAKE_KEYWORD MEOAVE_ESPSR_WAKE_KEYWORD

typedef struct {
    bool requested;
    bool armed;
    bool detected;
    uint32_t detections;
    uint32_t acoustic_detections;
    uint32_t text_test_detections;
    uint64_t detected_at_ms;
    char backend[24];
    char event_source[24];
    bool model_loaded;
    char model[48];
    char error[32];
} meo_wake_view_t;

void meo_wake_init(uint64_t now_ms);
void meo_wake_set_armed(bool armed, uint64_t now_ms);

/* Explicit test injection only; exact keyword, never acoustic evidence. */
bool meo_wake_submit_text(const char *text, uint64_t now_ms);
void meo_wake_feed_audio(const int16_t *stereo, size_t frames,
                         uint64_t now_ms, void *ctx);
void meo_wake_service(uint64_t now_ms);
void meo_wake_tick(uint64_t now_ms);
void meo_wake_view(meo_wake_view_t *out);
