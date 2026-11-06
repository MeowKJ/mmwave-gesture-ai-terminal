#include <assert.h>
#include <stdio.h>
#include "fls2_test_loop.h"

int main(void)
{
    fls2_test_loop_t loop;
    fls2_test_loop_init(&loop, false);
    assert(fls2_test_loop_step(&loop, true, false, 0) == FLS2_LOOP_NO_ACTION);
    fls2_test_loop_enable(&loop, true);
    assert(fls2_test_loop_step(&loop, false, false, 50000000) == FLS2_LOOP_NO_ACTION);
    assert(loop.round == 0);
    assert(fls2_test_loop_step(&loop, true, false, 50000000) == FLS2_LOOP_SHOW_WIDGETS);
    assert(loop.round == 1 && loop.deadline_us == 58000000);
    assert(fls2_test_loop_step(&loop, true, false, 57999999) == FLS2_LOOP_NO_ACTION);
    assert(fls2_test_loop_step(&loop, true, false, 58000000) == FLS2_LOOP_SHOW_COLORS);
    assert(fls2_test_loop_step(&loop, true, true, 71999999) == FLS2_LOOP_NO_ACTION);
    assert(fls2_test_loop_step(&loop, true, false, 72000000) == FLS2_LOOP_SHOW_BARS);
    assert(fls2_test_loop_step(&loop, true, false, 93999999) == FLS2_LOOP_NO_ACTION);
    assert(fls2_test_loop_step(&loop, true, false, 94000000) == FLS2_LOOP_SHOW_WIDGETS);
    assert(loop.round == 2);
    fls2_test_loop_enable(&loop, true); /* Duplicate ON must not restart a round. */
    assert(loop.phase == FLS2_LOOP_WIDGETS && loop.round == 2);
    fls2_test_loop_enable(&loop, false);
    assert(fls2_test_loop_step(&loop, true, false, 200000000) == FLS2_LOOP_NO_ACTION);
    fls2_test_loop_enable(&loop, true);
    assert(fls2_test_loop_step(&loop, true, false, 200000000) == FLS2_LOOP_SHOW_WIDGETS);
    assert(loop.round == 3);
    assert(fls2_test_loop_step(&loop, true, false, 208000000) == FLS2_LOOP_SHOW_COLORS);
    assert(fls2_test_loop_step(&loop, true, true, 222000000) == FLS2_LOOP_ABORT);
    assert(!loop.enabled && loop.phase == FLS2_LOOP_FAULT);
    assert(fls2_test_loop_step(&loop, true, false, 300000000) == FLS2_LOOP_NO_ACTION);
    fls2_test_loop_enable(&loop, true);
    assert(fls2_test_loop_step(&loop, true, false, 300000000) == FLS2_LOOP_SHOW_WIDGETS);
    /* A delayed service call performs one transition, not a burst of old tests. */
    assert(fls2_test_loop_step(&loop, true, false, 900000000) == FLS2_LOOP_SHOW_COLORS);
    assert(loop.deadline_us == 914000000);
    /* Long-running operation and timestamps beyond signed 32-bit microseconds. */
    fls2_test_loop_init(&loop, true);
    assert(fls2_test_loop_step(&loop, true, false, INT64_C(5000000000)) == FLS2_LOOP_SHOW_WIDGETS);
    assert(loop.deadline_us == INT64_C(5008000000));
    puts("PASS: boot/ready, full rounds, deadlines, stop/resume, duplicate ON, stuck-color abort, delayed service, 64-bit time");
    return 0;
}
