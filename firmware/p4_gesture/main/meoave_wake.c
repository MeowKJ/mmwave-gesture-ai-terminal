#include "meoave_wake.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static meo_wake_view_t s_view;
static meoave_espsr_view_t s_espsr;
static uint32_t s_espsr_seen;

static void copy_espsr_state(void)
{
    meoave_espsr_view(&s_espsr);
    s_view.model_loaded = s_espsr.model_loaded;
    s_view.armed = s_espsr.armed;
    (void)snprintf(s_view.backend, sizeof(s_view.backend), "%s", s_espsr.backend);
    (void)snprintf(s_view.model, sizeof(s_view.model), "%s", s_espsr.model);
    (void)snprintf(s_view.error, sizeof(s_view.error), "%s", s_espsr.error);
}

void meo_wake_init(uint64_t now_ms)
{
    memset(&s_view, 0, sizeof(s_view));
    s_espsr_seen = 0;
    s_view.detected_at_ms = now_ms;
    (void)meoave_espsr_init();
    copy_espsr_state();
}

void meo_wake_set_armed(bool armed, uint64_t now_ms)
{
    s_view.requested = armed;
    meoave_espsr_set_armed(armed);
    copy_espsr_state();
    if (!armed) {
        s_view.detected = false;
    } else if (s_view.detected && now_ms - s_view.detected_at_ms > 1800) {
        s_view.detected = false;
    }
}

bool meo_wake_submit_text(const char *text, uint64_t now_ms)
{
    if (!s_view.requested || !text || strcmp(text, MEO_WAKE_KEYWORD)) return false;
    if (s_view.detections && now_ms - s_view.detected_at_ms < 1800) return false;
    /* Retain this command for deterministic USB/UI tests, but make it
     * unmistakably a test bridge.  It is never reported as real acoustic
     * detection. */
    s_view.detected = true;
    s_view.detected_at_ms = now_ms;
    ++s_view.detections;
    ++s_view.text_test_detections;
    (void)snprintf(s_view.event_source, sizeof(s_view.event_source), "%s", "TEXT_BRIDGE_TEST");
    return true;
}

void meo_wake_feed_audio(const int16_t *stereo, size_t frames,
                         uint64_t now_ms, void *ctx)
{
    (void)ctx;
    meoave_espsr_feed_stereo_48k(stereo, frames, now_ms);
}

void meo_wake_service(uint64_t now_ms)
{
    meoave_espsr_view_t next;
    meoave_espsr_view(&next);
    if (next.detections != s_espsr_seen) {
        uint32_t delta = next.detections - s_espsr_seen;
        s_espsr_seen = next.detections;
        /* The worker validates arming/epoch at the moment of detection.
           A subsequent model rebuild may temporarily inhibit input before
           this service tick. Do not lose that already-valid event; leaving
           the AI page still consumes/discards it rather than replaying it. */
        if (!s_view.requested) {
            copy_espsr_state();
            return;
        }
        s_view.detections += delta;
        s_view.acoustic_detections += delta;
        (void)snprintf(s_view.event_source, sizeof(s_view.event_source), "%s", "ACOUSTIC_ESP_SR");
        s_view.detected = true;
        s_view.detected_at_ms = next.last_detected_ms ? next.last_detected_ms : now_ms;
        ESP_LOGI("MEO_WAKE", "local ESP-SR keyword=%s model=%s count=%lu",
                 MEO_WAKE_KEYWORD, next.model, (unsigned long)next.detections);
    }
    copy_espsr_state();
}

void meo_wake_tick(uint64_t now_ms)
{
    if (s_view.detected && now_ms - s_view.detected_at_ms > 1800)
        s_view.detected = false;
}

void meo_wake_view(meo_wake_view_t *out)
{
    if (out) {
        copy_espsr_state();
        *out = s_view;
    }
}
