#include "meoave_conversation.h"

#include <string.h>

static meo_expression_t expression_for(meo_conversation_state_t state)
{
    switch (state) {
    case MEO_CONV_LISTENING: return MEO_EYE_CURIOUS;
    case MEO_CONV_THINKING:  return MEO_EYE_SLEEPY;
    case MEO_CONV_SPEAKING:  return MEO_EYE_HAPPY;
    case MEO_CONV_ERROR:     return MEO_EYE_SAD;
    case MEO_CONV_COMPLETE:
    case MEO_CONV_IDLE:
    default:                 return MEO_EYE_CALM;
    }
}

static uint32_t duration_for(const meo_conversation_t *conversation,
                             meo_conversation_state_t state)
{
    switch (state) {
    case MEO_CONV_LISTENING: return conversation->timing.listening_ms;
    case MEO_CONV_THINKING:  return conversation->timing.thinking_ms;
    case MEO_CONV_SPEAKING:  return conversation->timing.speaking_ms;
    case MEO_CONV_COMPLETE:  return conversation->timing.complete_ms;
    case MEO_CONV_ERROR:     return conversation->timing.error_ms;
    case MEO_CONV_IDLE:
    default:                 return 0;
    }
}

static bool state_active(meo_conversation_state_t state)
{
    return state != MEO_CONV_IDLE;
}

static void publish(meo_conversation_t *conversation)
{
    conversation->expression = expression_for(conversation->state);
    if (!conversation->changed) return;
    meo_conversation_view_t view = meo_conversation_view(conversation);
    conversation->changed(&view, conversation->changed_ctx);
}

static void set_state(meo_conversation_t *conversation,
                      meo_conversation_state_t state, uint64_t now_ms)
{
    conversation->state = state;
    conversation->state_started_ms = now_ms;
    publish(conversation);
}

meo_conversation_timing_t meo_conversation_default_timing(void)
{
    /* Long enough to see each expression on the real 50 Hz UI tick, short
       enough that an offline demo finishes in about five seconds. */
    return (meo_conversation_timing_t){
        .listening_ms = 1200,
        .thinking_ms = 800,
        .speaking_ms = 1800,
        .complete_ms = 700,
        .error_ms = 900,
    };
}

void meo_conversation_init(meo_conversation_t *conversation, uint64_t now_ms,
                           meo_conversation_changed_cb changed, void *ctx)
{
    if (!conversation) return;
    memset(conversation, 0, sizeof(*conversation));
    conversation->timing = meo_conversation_default_timing();
    conversation->changed = changed;
    conversation->changed_ctx = ctx;
    conversation->state = MEO_CONV_IDLE;
    conversation->expression = MEO_EYE_CALM;
    conversation->state_started_ms = now_ms;
    conversation->last_ms = now_ms;
    conversation->initialized = true;
    publish(conversation);
}

bool meo_conversation_start(meo_conversation_t *conversation, uint64_t now_ms)
{
    if (!conversation || !conversation->initialized ||
        state_active(conversation->state)) return false;
    if (now_ms < conversation->last_ms) now_ms = conversation->last_ms;
    conversation->last_ms = now_ms;
    ++conversation->generation;
    if (conversation->generation == 0) ++conversation->generation;
    set_state(conversation, MEO_CONV_LISTENING, now_ms);
    return true;
}

bool meo_conversation_cancel(meo_conversation_t *conversation, uint64_t now_ms)
{
    if (!conversation || !conversation->initialized ||
        !state_active(conversation->state)) return false;
    if (now_ms < conversation->last_ms) now_ms = conversation->last_ms;
    conversation->last_ms = now_ms;
    set_state(conversation, MEO_CONV_IDLE, now_ms);
    return true;
}

bool meo_conversation_fail(meo_conversation_t *conversation, uint64_t now_ms)
{
    if (!conversation || !conversation->initialized ||
        !state_active(conversation->state)) return false;
    if (now_ms < conversation->last_ms) now_ms = conversation->last_ms;
    conversation->last_ms = now_ms;
    set_state(conversation, MEO_CONV_ERROR, now_ms);
    return true;
}

static meo_conversation_state_t next_state(meo_conversation_state_t state)
{
    switch (state) {
    case MEO_CONV_LISTENING: return MEO_CONV_THINKING;
    case MEO_CONV_THINKING:  return MEO_CONV_SPEAKING;
    case MEO_CONV_SPEAKING:  return MEO_CONV_COMPLETE;
    case MEO_CONV_COMPLETE:
    case MEO_CONV_ERROR:     return MEO_CONV_IDLE;
    case MEO_CONV_IDLE:
    default:                 return MEO_CONV_IDLE;
    }
}

void meo_conversation_tick(meo_conversation_t *conversation, uint64_t now_ms)
{
    if (!conversation || !conversation->initialized || now_ms < conversation->last_ms)
        return;
    conversation->last_ms = now_ms;
    /* A delayed app task may cross several phases in one call. The bounded
       loop keeps the state machine deterministic without replaying audio. */
    for (unsigned guard = 0; guard < 6 && state_active(conversation->state); ++guard) {
        uint32_t duration = duration_for(conversation, conversation->state);
        if (now_ms - conversation->state_started_ms < duration) break;
        set_state(conversation, next_state(conversation->state),
                  conversation->state_started_ms + duration);
    }
}

bool meo_conversation_active(const meo_conversation_t *conversation)
{
    return conversation && conversation->initialized && state_active(conversation->state);
}

meo_conversation_view_t meo_conversation_view(const meo_conversation_t *conversation)
{
    if (!conversation) {
        return (meo_conversation_view_t){
            .state = MEO_CONV_IDLE, .expression = MEO_EYE_CALM,
        };
    }
    return (meo_conversation_view_t){
        .state = conversation->state,
        .expression = conversation->expression,
        .state_started_ms = conversation->state_started_ms,
        .generation = conversation->generation,
        .active = state_active(conversation->state),
    };
}

const char *meo_conversation_state_name(meo_conversation_state_t state)
{
    static const char *const names[] = {
        "IDLE", "LISTENING", "THINKING", "SPEAKING", "COMPLETE", "ERROR",
    };
    if (state < MEO_CONV_IDLE || state > MEO_CONV_ERROR) return "INVALID";
    return names[state];
}

const char *meo_conversation_expression_name(meo_expression_t expression)
{
    return meo_expression_name(expression);
}

const char *meo_conversation_demo_prompt(void)
{
    return "你好，Meoave";
}

const char *meo_conversation_demo_response(void)
{
    return "我在这里，今天也陪着你。";
}
