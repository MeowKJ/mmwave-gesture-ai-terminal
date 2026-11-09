#include "meoave_model.h"
#include <math.h>
#include <string.h>

static float limit(float n, float a, float b) { return n < a ? a : n > b ? b : n; }
static bool fresh(uint64_t now, uint64_t at, uint64_t max_age)
{ return at <= now && now - at <= max_age; }

meo_page_t meo_page_parent(meo_page_t page)
{
    switch (page) {
    /* AI is the default dial, but a peer of settings and breathing.
       The launcher itself has no parent/back destination. */
    case MEO_HOME: case MEO_APPS: case MEO_SETTINGS: case MEO_BREATHE: return MEO_APPS;
    case MEO_WIFI: case MEO_DISPLAY: case MEO_ACCESSIBILITY: return MEO_SETTINGS;
    default: return MEO_HOME;
    }
}
static uint32_t random_next(meo_model_t *m)
{ uint32_t n = m->rng; n ^= n << 13; n ^= n >> 17; n ^= n << 5; return m->rng = n; }

static const meo_eye_shape_t expressions[MEO_EYE_COUNT][2] = {
    [MEO_EYE_CALM]      = {{0,1,0,76,0}, {0,1,0,76,0}},
    [MEO_EYE_HAPPY]     = {{32,0,27,16,0}, {32,0,27,16,0}},
    [MEO_EYE_CURIOUS]   = {{0,5,0,82,-6}, {0,1,0,62,9}},
    [MEO_EYE_SLEEPY]    = {{30,0,-3,12,8}, {30,0,-3,12,8}},
    [MEO_EYE_SURPRISED] = {{0,8,0,90,-4}, {0,8,0,90,-4}},
    [MEO_EYE_WINK]      = {{31,0,6,12,2}, {0,1,0,76,0}},
    [MEO_EYE_SAD]       = {{30,0,-24,16,3}, {30,0,-24,16,3}},
};

/* Long neutral rests; brief accents, not a flashing expression slideshow.
 * Sadness is available to AI but is not invented as an idle mood. */
static const struct { meo_expression_t expression; uint32_t duration; } idle[] = {
    {MEO_EYE_CALM,6000}, {MEO_EYE_CURIOUS,2400}, {MEO_EYE_CALM,5000},
    {MEO_EYE_HAPPY,2600}, {MEO_EYE_CALM,6500}, {MEO_EYE_WINK,900},
    {MEO_EYE_CALM,6000}, {MEO_EYE_SLEEPY,2400}, {MEO_EYE_CALM,6000},
    {MEO_EYE_SURPRISED,1400},
};
#define IDLE_PERIOD_MS 39200u

void meo_model_set_expression(meo_model_t *m, meo_expression_t expression, uint32_t hold_ms)
{
    if (expression < MEO_EYE_CALM || expression >= MEO_EYE_COUNT) return;
    m->expression = expression;
    m->expression_override = true;
    m->expression_at_ms = m->now_ms;
    m->expression_hold_ms = hold_ms;
}

void meo_model_auto_expression(meo_model_t *m)
{
    m->expression_override = false;
    m->expression_clock_ms = 0;
    m->expression = MEO_EYE_CALM;
}

static void update_expression(meo_model_t *m, uint64_t elapsed, float mix)
{
    if (m->expression_override && m->expression_hold_ms &&
        m->now_ms - m->expression_at_ms >= m->expression_hold_ms) {
        uint64_t after_expiry = m->now_ms - m->expression_at_ms - m->expression_hold_ms;
        /* A long frame must not count the held expression as idle time. */
        if (elapsed > after_expiry) elapsed = after_expiry;
        meo_model_auto_expression(m);
    }
    if (!m->expression_override) {
        if (m->settings.reduced_motion) m->expression = MEO_EYE_CALM;
        else if (m->page == MEO_HOME) {
            m->expression_clock_ms = (m->expression_clock_ms + elapsed % IDLE_PERIOD_MS) % IDLE_PERIOD_MS;
            uint64_t age = m->expression_clock_ms;
            for (unsigned i = 0; i < sizeof(idle)/sizeof(idle[0]); ++i) {
                if (age < idle[i].duration) { m->expression = idle[i].expression; break; }
                age -= idle[i].duration;
            }
        }
    }
    /* Reduce Motion still accepts explicit AI expressions, without tweening. */
    if (m->settings.reduced_motion) mix = 1;
    for (int i = 0; i < 2; ++i) {
        meo_eye_shape_t *s = &m->eye_shape[i];
        const meo_eye_shape_t *target = &expressions[m->expression][i];
#define MORPH(field) s->field += (target->field - s->field) * mix
        MORPH(half_x); MORPH(half_y); MORPH(arch); MORPH(width); MORPH(y);
#undef MORPH
    }
}
static void reset_session(meo_model_t *m)
{
    m->elapsed_ms = 0;
    m->guide_phase_units = 0;
    m->guide_phase = 0;
    m->inhaling = true;
    m->complete = false;
}

meo_settings_t meo_default_settings(void)
{ return (meo_settings_t){.brightness=80, .guide_bpm=6, .reduced_motion=false}; }

void meo_validate_settings(meo_settings_t *s)
{
    if (s->brightness < 15 || s->brightness > 100) s->brightness = 80;
    if (s->guide_bpm < 4 || s->guide_bpm > 12) s->guide_bpm = 6;
}

void meo_model_init(meo_model_t *m, uint64_t now, const meo_settings_t *settings)
{
    memset(m, 0, sizeof(*m));
    m->settings = settings ? *settings : meo_default_settings();
    meo_validate_settings(&m->settings);
    m->page = MEO_HOME;
    m->mode = MEO_GUIDED;
    reset_session(m);
    m->now_ms = m->last_ms = now;
    m->next_blink_ms = now + 3200;
    m->openness = 1;
    m->rng = 0x6d656f31;
    memcpy(m->eye_shape, expressions[MEO_EYE_CALM], sizeof(m->eye_shape));
}

void meo_model_gaze(meo_model_t *m, meo_gaze_t s)
{
    if (!isfinite(s.x) || !isfinite(s.y)) s.valid = false;
    s.x = s.valid ? limit(s.x, -1, 1) : 0;
    s.y = s.valid ? limit(s.y, -1, 1) : 0;
    m->gaze = s;
}

void meo_model_respiration(meo_model_t *m, meo_resp_t s)
{
    if (!isfinite(s.waveform)) s.valid = false;
    s.waveform = s.valid ? limit(s.waveform, -1, 1) : 0;
    if (!isfinite(s.rate_bpm) || s.rate_bpm < 3 || s.rate_bpm > 60) s.rate_valid = false;
    m->respiration = s;
}

void meo_model_set_rate(meo_model_t *m, int rate)
{
    m->settings.guide_bpm = rate < 4 ? 4 : rate > 12 ? 12 : rate;
}

void meo_model_set_mode(meo_model_t *m, meo_breath_mode_t mode)
{
    if (mode != MEO_GUIDED && mode != MEO_RADAR) return;
    if (mode == m->mode) return;
    m->mode = mode;
    m->running = false;
    reset_session(m);
}

void meo_model_toggle_session(meo_model_t *m)
{
    if (m->mode != MEO_GUIDED) return;
    if (m->complete) reset_session(m);
    m->running = !m->running;
}

void meo_model_tick(meo_model_t *m, uint64_t now)
{
    if (now < m->last_ms) return;
    uint64_t elapsed = now - m->last_ms;
    float dt = (float)(elapsed > 200 ? 200 : elapsed);
    m->now_ms = now;
    m->last_ms = now;
    m->gaze_live = m->gaze.valid && fresh(now, m->gaze.at_ms, 1500);
    m->respiration_live = m->respiration.valid && fresh(now, m->respiration.at_ms, 2000);
    m->rate_live = m->respiration_live && m->respiration.rate_valid;
    float seconds = (float)(now % 600000) / 1000.0f;
    float x = m->gaze_live ? m->gaze.x : sinf(seconds * .39f) * .45f;
    float y = m->gaze_live ? m->gaze.y : sinf(seconds * .27f) * .22f;
    if (m->settings.reduced_motion && !m->gaze_live) x = y = 0;
    float mix = 1.0f - expf(-dt / 140.0f);
    update_expression(m, elapsed, mix);
    m->eye_x += (x - m->eye_x) * mix;
    m->eye_y += (y - m->eye_y) * mix;
    if (m->settings.reduced_motion) {
        m->blink_start_ms = 0;
        m->openness = 1;
        /* Do not replay an overdue blink when motion is enabled again. */
        m->next_blink_ms = now + 3200;
    } else {
        if (now >= m->next_blink_ms) {
            m->blink_start_ms = now;
            m->next_blink_ms = now + 2800 + random_next(m) % 3000;
        }
        uint64_t blink_age = now - m->blink_start_ms;
        m->openness = m->blink_start_ms && blink_age < 210
            ? 1.0f - .95f * sinf(3.14159265f * (float)blink_age / 210.0f) : 1.0f;
    }

    if (m->running && m->mode == MEO_GUIDED) {
        uint64_t remaining = 120000 - m->elapsed_ms;
        uint64_t active_ms = elapsed < remaining ? elapsed : remaining;
        m->elapsed_ms += active_ms;
        /* Clamp first: time beyond session completion must not advance phase. */
        m->guide_phase_units = (uint32_t)((m->guide_phase_units
            + active_ms * m->settings.guide_bpm) % 60000);
        if (m->elapsed_ms >= 120000) {
            m->running = false;
            m->complete = true;
        }
    }
    m->guide_phase = (float)m->guide_phase_units / 60000.0f;
    m->inhaling = m->guide_phase_units < 24000;
    float p = m->inhaling ? m->guide_phase / .4f : (1 - m->guide_phase) / .6f;
    float target = .5f - .5f * cosf(3.14159265f * p);
    if (m->mode == MEO_RADAR) target = m->respiration_live ? (m->respiration.waveform + 1) * .5f : .15f;
    m->breath_level += (target - m->breath_level) * mix;
}

void meo_rotate_touch_180(int raw_x, int raw_y, int *x, int *y)
{
    /* Test oracle only; LVGL itself performs this mapping in production.
       Applying this AND LVGL rotation would rotate input twice. */
    *x = 459 - raw_x;
    *y = 459 - raw_y;
}
