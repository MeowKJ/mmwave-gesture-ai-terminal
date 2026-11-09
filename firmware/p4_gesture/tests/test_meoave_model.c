#include "meoave_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void assert_mode_noop(meo_model_t *m, meo_breath_mode_t mode)
{
    meo_model_t before;
    memcpy(&before, m, sizeof(before));
    meo_model_set_mode(m, mode);
    assert(memcmp(&before, m, sizeof(before)) == 0);
}

static void test_mode_idempotence(void)
{
    meo_model_t m;
    meo_model_init(&m, 100, NULL);
    assert_mode_noop(&m, MEO_GUIDED);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 35100);
    assert(m.running && m.elapsed_ms == 35000 && !m.inhaling);
    assert_mode_noop(&m, MEO_GUIDED);
    assert_mode_noop(&m, (meo_breath_mode_t)-1);
    assert_mode_noop(&m, (meo_breath_mode_t)2);
    meo_model_toggle_session(&m);
    assert_mode_noop(&m, MEO_GUIDED);
    meo_model_tick(&m, 40100);
    assert(m.elapsed_ms == 35000 && m.guide_phase == .5f);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 125100);
    assert(m.complete && !m.running && m.elapsed_ms == 120000);
    assert_mode_noop(&m, MEO_GUIDED);
    meo_model_set_mode(&m, MEO_RADAR);
    assert(!m.running && !m.complete && m.elapsed_ms == 0);
    assert(m.guide_phase_units == 0 && m.guide_phase == 0 && m.inhaling);
    assert_mode_noop(&m, MEO_RADAR);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 130100);
    assert(!m.running && m.elapsed_ms == 0 && m.guide_phase == 0);
    meo_model_set_mode(&m, MEO_GUIDED);
    assert(!m.running && m.elapsed_ms == 0 && m.guide_phase == 0 && m.inhaling);
}

static void test_phase_continuity(void)
{
    meo_model_t m;
    meo_model_init(&m, 0, NULL);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 35000);
    assert(m.guide_phase == .5f && !m.inhaling);
    meo_model_set_rate(&m, 7);
    assert(m.guide_phase == .5f && m.guide_phase_units == 30000);
    meo_model_tick(&m, 35000);
    assert(m.guide_phase == .5f && !m.inhaling && m.elapsed_ms == 35000);
    meo_model_tick(&m, 36000);
    assert(m.guide_phase_units == 37000 && m.elapsed_ms == 36000);
    assert(fabsf(m.guide_phase - 37.0f / 60.0f) < .000001f);
    meo_model_toggle_session(&m);
    meo_model_set_rate(&m, 99);
    meo_model_tick(&m, 41000);
    assert(m.settings.guide_bpm == 12 && m.guide_phase_units == 37000);
    assert(m.elapsed_ms == 36000);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 42000);
    assert(m.guide_phase_units == 49000 && m.elapsed_ms == 37000);
    meo_model_set_rate(&m, 0);
    meo_model_tick(&m, 43000);
    assert(m.settings.guide_bpm == 4 && m.guide_phase_units == 53000);
    assert(m.elapsed_ms == 38000);

    meo_model_init(&m, 0, NULL);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 3999);
    assert(m.inhaling && m.guide_phase_units == 23994);
    meo_model_tick(&m, 4000);
    assert(!m.inhaling && m.guide_phase_units == 24000);
    meo_model_tick(&m, 9999);
    assert(!m.inhaling && m.guide_phase_units == 59994);
    meo_model_tick(&m, 10000);
    assert(m.inhaling && m.guide_phase == 0);
    meo_model_tick(&m, 110000);
    assert(m.guide_phase_units == 0 && m.elapsed_ms == 110000);
    meo_model_set_rate(&m, 7);
    meo_model_tick(&m, 119999);
    assert(!m.complete && m.elapsed_ms == 119999 && m.guide_phase_units == 9993);
    meo_model_tick(&m, UINT64_MAX / 2);
    assert(m.complete && !m.running && m.elapsed_ms == 120000);
    assert(m.guide_phase_units == 10000); /* Only the final active ms counts. */
    meo_model_set_rate(&m, 12);
    meo_model_tick(&m, UINT64_MAX / 2 + 1000);
    assert(m.guide_phase_units == 10000 && m.elapsed_ms == 120000);
    meo_model_toggle_session(&m);
    assert(m.running && !m.complete && m.elapsed_ms == 0);
    assert(m.guide_phase_units == 0 && m.guide_phase == 0 && m.inhaling);
    meo_model_tick(&m, UINT64_MAX / 2 + 2000);
    assert(m.guide_phase_units == 12000 && m.elapsed_ms == 1000);

    /* Small ticks must accumulate exactly at a non-integral cycle length. */
    meo_model_init(&m, 0, NULL);
    meo_model_set_rate(&m, 7);
    meo_model_toggle_session(&m);
    for (uint64_t now = 1; now <= 120000; ++now) meo_model_tick(&m, now);
    assert(m.complete && m.elapsed_ms == 120000 && m.guide_phase_units == 0);
}

static void test_reduced_motion(void)
{
    meo_model_t m;
    meo_settings_t settings = meo_default_settings();
    settings.reduced_motion = true;
    meo_model_init(&m, 0, &settings);
    uint32_t rng = m.rng;
    meo_model_tick(&m, 3200);
    meo_model_tick(&m, 3305);
    meo_model_tick(&m, 10000);
    assert(m.openness == 1 && m.blink_start_ms == 0 && m.rng == rng);
    assert(m.eye_x == 0 && m.eye_y == 0);
    meo_model_gaze(&m, (meo_gaze_t){.x=1,.y=-1,.at_ms=10000,.valid=true});
    meo_model_tick(&m, 10100);
    assert(m.gaze_live && m.eye_x > 0 && m.eye_x < 1 && m.eye_y < 0 && m.eye_y > -1);
    assert(m.openness == 1 && m.rng == rng);
    float eye_x = m.eye_x;
    meo_model_tick(&m, 11501);
    assert(!m.gaze_live && m.eye_x < eye_x && m.eye_x > 0 && m.openness == 1);
    m.settings.reduced_motion = false;
    meo_model_tick(&m, 14701);
    meo_model_tick(&m, 14806);
    assert(m.openness < .1f); /* Normal blinking still works. */
    m.settings.reduced_motion = true;
    meo_model_tick(&m, 14839);
    assert(m.openness == 1 && m.blink_start_ms == 0); /* Cancel an active blink. */
    m.settings.reduced_motion = false;
    meo_model_tick(&m, 14872);
    assert(m.openness == 1 && m.blink_start_ms == 0); /* No overdue blink. */
    meo_model_tick(&m, 18039);
    meo_model_tick(&m, 18144);
    assert(m.openness < .1f);
}

static void test_expressions(void)
{
    meo_model_t m, before;
    /* Expiry must give the same idle target whether a long frame was split. */
    meo_model_init(&m, 0, NULL);
    meo_model_init(&before, 0, NULL);
    meo_model_set_expression(&m, MEO_EYE_SAD, 6000);
    meo_model_set_expression(&before, MEO_EYE_SAD, 6000);
    meo_model_tick(&m, 6000);
    meo_model_tick(&before, 5999); meo_model_tick(&before, 6000);
    assert(m.expression_clock_ms == 0 && before.expression_clock_ms == 0);
    assert(m.expression == MEO_EYE_CALM && before.expression == MEO_EYE_CALM);
    meo_model_set_expression(&m, MEO_EYE_HAPPY, 6000);
    meo_model_set_expression(&before, MEO_EYE_HAPPY, 6000);
    meo_model_tick(&m, 19000);
    meo_model_tick(&before, 12000); meo_model_tick(&before, 19000);
    assert(m.expression_clock_ms == 7000 && before.expression_clock_ms == 7000);
    assert(m.expression == MEO_EYE_CURIOUS && before.expression == MEO_EYE_CURIOUS);
    meo_model_init(&m, 100, NULL);
    assert(m.expression == MEO_EYE_CALM && !m.expression_override);
    meo_model_tick(&m, 6099); assert(m.expression == MEO_EYE_CALM);
    meo_model_tick(&m, 6100); assert(m.expression == MEO_EYE_CURIOUS);
    meo_model_tick(&m, 8500); assert(m.expression == MEO_EYE_CALM);
    meo_model_tick(&m, 13500); assert(m.expression == MEO_EYE_HAPPY);
    uint64_t idle_clock = m.expression_clock_ms;
    m.page = MEO_APPS;
    meo_model_tick(&m, 23500);
    assert(m.expression_clock_ms == idle_clock); /* no hidden idle slideshow */
    for (int e = 0; e < MEO_EYE_COUNT; ++e) {
        meo_model_set_expression(&m, (meo_expression_t)e, 0);
        meo_model_tick(&m, m.now_ms + 1000);
        assert(m.expression == (meo_expression_t)e && m.expression_override);
        for (int i = 0; i < 2; ++i) {
            const meo_eye_shape_t *s = &m.eye_shape[i];
            assert(isfinite(s->half_x) && isfinite(s->half_y) && isfinite(s->arch));
            assert(isfinite(s->width) && isfinite(s->y));
            assert(s->width >= 12 && s->width <= 90);
        }
    }
    memcpy(&before, &m, sizeof(m));
    meo_model_set_expression(&m, (meo_expression_t)-1, 10);
    meo_model_set_expression(&m, MEO_EYE_COUNT, 10);
    assert(!memcmp(&before, &m, sizeof(m)));
    meo_model_set_expression(&m, MEO_EYE_HAPPY, 2000);
    uint64_t at = m.now_ms;
    meo_model_tick(&m, at+1999); assert(m.expression_override && m.expression == MEO_EYE_HAPPY);
    meo_model_tick(&m, at+2000); assert(!m.expression_override && m.expression == MEO_EYE_CALM);
    m.page = MEO_HOME;
    meo_model_tick(&m, at+6000);
    meo_model_set_expression(&m, MEO_EYE_SAD, 0);
    meo_model_tick(&m, at+100000); assert(m.expression == MEO_EYE_SAD);
    meo_model_auto_expression(&m);
    assert(!m.expression_override && m.expression_clock_ms == 0 && m.expression == MEO_EYE_CALM);

    m.settings.reduced_motion = true;
    uint32_t rng = m.rng;
    idle_clock = m.expression_clock_ms;
    meo_model_tick(&m, at+200000);
    assert(m.expression == MEO_EYE_CALM && m.expression_clock_ms == idle_clock && m.rng == rng);
    meo_model_set_expression(&m, MEO_EYE_HAPPY, 1000);
    meo_model_tick(&m, m.now_ms);
    assert(m.eye_shape[0].half_x == 32 && m.eye_shape[0].arch == 27 && m.eye_shape[0].width == 16);
    memcpy(&before, &m, sizeof(m));
    meo_model_tick(&m, m.now_ms-1); assert(!memcmp(&before, &m, sizeof(m)));
    meo_model_tick(&m, m.now_ms+1000);
    assert(m.expression == MEO_EYE_CALM && m.eye_shape[0].half_x == 0 && m.eye_shape[0].width == 76);

    /* Every advertised idle accent appears; sadness is explicit-only.
     * Huge jumps wrap in one step rather than iterating missed animations. */
    meo_model_init(&m, 0, NULL);
    unsigned seen = 0;
    for (unsigned t = 0; t < 39200; t += 100) {
        meo_model_tick(&m, t); seen |= 1u << m.expression;
    }
    assert(seen == (1u << MEO_EYE_SAD)-1);
    meo_model_tick(&m, 39200); assert(m.expression_clock_ms == 0 && m.expression == MEO_EYE_CALM);
    meo_model_tick(&m, UINT64_MAX/2);
    assert(m.expression_clock_ms < 39200 && m.expression < MEO_EYE_COUNT);
}


int main(void)
{
    assert(meo_page_parent(MEO_HOME)==MEO_APPS && meo_page_parent(MEO_APPS)==MEO_APPS);
    assert(meo_page_parent(MEO_SETTINGS)==MEO_APPS && meo_page_parent(MEO_BREATHE)==MEO_APPS);
    assert(meo_page_parent(MEO_DISPLAY)==MEO_SETTINGS && meo_page_parent(MEO_ACCESSIBILITY)==MEO_SETTINGS);
    assert(meo_page_parent(MEO_WIFI)==MEO_SETTINGS && meo_page_parent(MEO_PAGE_COUNT)==MEO_HOME);
    assert(!meo_default_settings().app_list_view);
    test_mode_idempotence();
    test_phase_continuity();
    test_reduced_motion();
    test_expressions();
    meo_model_t m;
    meo_model_init(&m, 0, NULL);
    assert(m.page == MEO_HOME && !m.running && m.settings.guide_bpm == 6);
    meo_model_gaze(&m, (meo_gaze_t){.x=5,.y=-5,.at_ms=0,.valid=true});
    meo_model_tick(&m, 200);
    assert(m.gaze_live && m.eye_x > 0 && m.eye_x <= 1 && m.eye_y < 0 && m.eye_y >= -1);
    meo_model_tick(&m, 1600); assert(!m.gaze_live);
    meo_model_gaze(&m, (meo_gaze_t){.x=NAN,.y=0,.valid=true,.at_ms=1600});
    meo_model_tick(&m, 1633); assert(!m.gaze_live && isfinite(m.eye_x));
    meo_model_respiration(&m, (meo_resp_t){.waveform=.7f,.rate_bpm=12.4f,.valid=true,
                                         .rate_valid=true,.at_ms=1700});
    meo_model_tick(&m, 1666); assert(!m.respiration_live); /* future sample */
    meo_model_tick(&m, 1700); assert(m.rate_live);
    meo_model_tick(&m, 3701); assert(!m.respiration_live && !m.rate_live);
    meo_model_respiration(&m, (meo_resp_t){.waveform=INFINITY,.rate_bpm=80,.valid=true,
                                         .rate_valid=true,.at_ms=3701});
    meo_model_tick(&m, 3734); assert(!m.respiration_live && !m.rate_live);

    meo_model_init(&m, 0, NULL);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 2000); assert(m.inhaling && m.elapsed_ms == 2000);
    meo_model_tick(&m, 6000); assert(!m.inhaling);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 9000); assert(m.elapsed_ms == 6000);
    meo_model_toggle_session(&m);
    meo_model_tick(&m, 123000); assert(m.complete && !m.running && m.elapsed_ms == 120000);
    meo_model_toggle_session(&m); assert(m.running && !m.complete && m.elapsed_ms == 0);
    meo_model_set_mode(&m, MEO_RADAR); assert(!m.running && m.elapsed_ms == 0);
    meo_model_toggle_session(&m); assert(!m.running); /* no invented radar session */
    meo_model_set_rate(&m, 0); assert(m.settings.guide_bpm == 4);
    meo_model_set_rate(&m, 99); assert(m.settings.guide_bpm == 12);
    uint64_t now = m.last_ms; meo_model_tick(&m, now - 1); assert(m.last_ms == now);
    meo_settings_t settings = {.brightness=0,.guide_bpm=0};
    meo_validate_settings(&settings); assert(settings.brightness == 80 && settings.guide_bpm == 6);
    int x,y;
    meo_rotate_touch_180(0,0,&x,&y); assert(x==459 && y==459);
    meo_rotate_touch_180(459,459,&x,&y); assert(x==0 && y==0);
    meo_rotate_touch_180(100,200,&x,&y); assert(x==359 && y==259);
    puts("PASS: defaults, gaze smoothing, finite/clamped samples, signal age/future, session clock,");
    puts("      pause/resume/completion, rate bounds, settings validation, 180-degree mapping oracle");
    puts("PASS: mode idempotence, continuous phase across rate changes, phase boundaries and completion,");
    puts("      reduced-motion blink suppression/cancellation and smooth live gaze");
    puts("PASS: seven expressions, idle boundaries/wrap, hidden-page pause, explicit hold/release/expiry,");
    puts("      invalid expression no-op, reduced-motion instant shapes, huge/rewound time");
    return 0;
}
