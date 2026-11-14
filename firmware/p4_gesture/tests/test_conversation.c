#include "meoave_conversation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    meo_conversation_view_t events[32];
    unsigned count;
} recorder_t;

static void changed(const meo_conversation_view_t *view, void *ctx)
{
    recorder_t *recorder = ctx;
    assert(recorder->count < sizeof(recorder->events) / sizeof(recorder->events[0]));
    recorder->events[recorder->count++] = *view;
}

static void assert_event(const recorder_t *recorder, unsigned index,
                         meo_conversation_state_t state, meo_expression_t expression)
{
    assert(index < recorder->count);
    assert(recorder->events[index].state == state);
    assert(recorder->events[index].expression == expression);
}

static void test_one_complete_turn(void)
{
    recorder_t recorder = {0};
    meo_conversation_t conversation;
    meo_conversation_init(&conversation, 100, changed, &recorder);
    conversation.timing = (meo_conversation_timing_t){
        .listening_ms = 100, .thinking_ms = 100, .speaking_ms = 100,
        .complete_ms = 100, .error_ms = 100,
    };

    assert(recorder.count == 1);
    assert_event(&recorder, 0, MEO_CONV_IDLE, MEO_EYE_CALM);
    assert(!meo_conversation_active(&conversation));
    assert(!strcmp(meo_conversation_demo_prompt(), "你好，Meoave"));
    assert(strstr(meo_conversation_demo_response(), "陪着你") != NULL);

    assert(meo_conversation_start(&conversation, 100));
    assert(!meo_conversation_start(&conversation, 100));
    assert(meo_conversation_active(&conversation));
    assert_event(&recorder, 1, MEO_CONV_LISTENING, MEO_EYE_CURIOUS);

    meo_conversation_tick(&conversation, 199);
    assert(conversation.state == MEO_CONV_LISTENING);
    meo_conversation_tick(&conversation, 200);
    assert_event(&recorder, 2, MEO_CONV_THINKING, MEO_EYE_SLEEPY);
    meo_conversation_tick(&conversation, 300);
    assert_event(&recorder, 3, MEO_CONV_SPEAKING, MEO_EYE_HAPPY);
    meo_conversation_tick(&conversation, 400);
    assert_event(&recorder, 4, MEO_CONV_COMPLETE, MEO_EYE_CALM);
    assert(meo_conversation_active(&conversation));
    meo_conversation_tick(&conversation, 500);
    assert_event(&recorder, 5, MEO_CONV_IDLE, MEO_EYE_CALM);
    assert(!meo_conversation_active(&conversation));
    assert(conversation.generation == 1);
}

static void test_cancel_error_and_delayed_tick(void)
{
    recorder_t recorder = {0};
    meo_conversation_t conversation;
    meo_conversation_init(&conversation, 0, changed, &recorder);
    conversation.timing = (meo_conversation_timing_t){
        .listening_ms = 50, .thinking_ms = 50, .speaking_ms = 50,
        .complete_ms = 50, .error_ms = 50,
    };

    assert(meo_conversation_start(&conversation, 10));
    meo_conversation_tick(&conversation, 60);
    assert(conversation.state == MEO_CONV_THINKING);
    assert(meo_conversation_fail(&conversation, 65));
    assert(conversation.state == MEO_CONV_ERROR);
    assert(conversation.expression == MEO_EYE_SAD);
    meo_conversation_tick(&conversation, 115);
    assert(conversation.state == MEO_CONV_IDLE);

    assert(meo_conversation_start(&conversation, 200));
    assert(meo_conversation_cancel(&conversation, 210));
    assert(conversation.state == MEO_CONV_IDLE);
    assert(!meo_conversation_cancel(&conversation, 220));

    assert(meo_conversation_start(&conversation, 300));
    recorder_t before = recorder;
    meo_conversation_tick(&conversation, 299);
    assert(!memcmp(&before, &recorder, sizeof(before)));
    /* One delayed task pass may cross every phase, but never emits a second
       start and always finishes in the same deterministic idle state. */
    meo_conversation_tick(&conversation, 1000);
    assert(conversation.state == MEO_CONV_IDLE);
    assert(conversation.generation == 3);
}

int main(void)
{
    test_one_complete_turn();
    test_cancel_error_and_delayed_tick();
    assert(!strcmp(meo_conversation_state_name(MEO_CONV_SPEAKING), "SPEAKING"));
    assert(!strcmp(meo_conversation_expression_name(MEO_EYE_HAPPY), "HAPPY"));
    assert(!strcmp(meo_conversation_state_name((meo_conversation_state_t)-1), "INVALID"));
    puts("PASS: offline AI turn state machine, deterministic phase timings,");
    puts("      cancel/error recovery, delayed ticks, generation IDs and eye-expression mapping");
    return 0;
}
