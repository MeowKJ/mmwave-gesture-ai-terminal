#include "fls2_test_loop.h"

void fls2_test_loop_init(fls2_test_loop_t *loop, bool enabled)
{
    *loop = (fls2_test_loop_t){.enabled = enabled, .phase = FLS2_LOOP_WAIT};
}

void fls2_test_loop_enable(fls2_test_loop_t *loop, bool enabled)
{
    if (loop->enabled == enabled) return;
    loop->enabled = enabled;
    loop->phase = FLS2_LOOP_WAIT;
    loop->deadline_us = 0;
}

fls2_loop_action_t fls2_test_loop_step(fls2_test_loop_t *loop, bool ready,
                                    bool colors_active, int64_t now_us)
{
    if (!loop->enabled || !ready) return FLS2_LOOP_NO_ACTION;
    if (loop->phase != FLS2_LOOP_WAIT && now_us < loop->deadline_us) {
        return FLS2_LOOP_NO_ACTION;
    }
    switch (loop->phase) {
    case FLS2_LOOP_WAIT:
    case FLS2_LOOP_BARS:
        loop->round++;
        loop->phase = FLS2_LOOP_WIDGETS;
        loop->deadline_us = now_us + 8000000;
        return FLS2_LOOP_SHOW_WIDGETS;
    case FLS2_LOOP_WIDGETS:
        loop->phase = FLS2_LOOP_COLORS;
        /* Four 3-second colors plus 2 seconds for bounded-lock retries/restore. */
        loop->deadline_us = now_us + 14000000;
        return FLS2_LOOP_SHOW_COLORS;
    case FLS2_LOOP_COLORS:
        if (colors_active) {
            loop->enabled = false;
            loop->phase = FLS2_LOOP_FAULT;
            return FLS2_LOOP_ABORT;
        }
        loop->phase = FLS2_LOOP_BARS;
        /* Hardware pattern auto-restores at 20 seconds; leave a 2-second gap. */
        loop->deadline_us = now_us + 22000000;
        return FLS2_LOOP_SHOW_BARS;
    default:
        return FLS2_LOOP_NO_ACTION;
    }
}

const char *fls2_test_loop_phase_name(fls2_loop_phase_t phase)
{
    switch (phase) {
    case FLS2_LOOP_WAIT: return "WAIT";
    case FLS2_LOOP_WIDGETS: return "WIDGETS";
    case FLS2_LOOP_COLORS: return "COLORS";
    case FLS2_LOOP_BARS: return "BARS";
    case FLS2_LOOP_FAULT: return "FAULT";
    default: return "UNKNOWN";
    }
}
