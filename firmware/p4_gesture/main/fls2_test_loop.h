#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FLS2_LOOP_WAIT,
    FLS2_LOOP_WIDGETS,
    FLS2_LOOP_COLORS,
    FLS2_LOOP_BARS,
    FLS2_LOOP_FAULT,
} fls2_loop_phase_t;

typedef enum {
    FLS2_LOOP_NO_ACTION,
    FLS2_LOOP_SHOW_WIDGETS,
    FLS2_LOOP_SHOW_COLORS,
    FLS2_LOOP_SHOW_BARS,
    FLS2_LOOP_ABORT,
} fls2_loop_action_t;

typedef struct {
    bool enabled;
    fls2_loop_phase_t phase;
    uint32_t round;
    int64_t deadline_us;
} fls2_test_loop_t;

/* Pure scheduler: only the app task owns this state. No hardware or IDF calls. */
void fls2_test_loop_init(fls2_test_loop_t *loop, bool enabled);
void fls2_test_loop_enable(fls2_test_loop_t *loop, bool enabled);
fls2_loop_action_t fls2_test_loop_step(fls2_test_loop_t *loop, bool ready,
                                    bool colors_active, int64_t now_us);
const char *fls2_test_loop_phase_name(fls2_loop_phase_t phase);
