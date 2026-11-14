#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meoave_model.h"

/*
 * A deliberately local, deterministic conversation slice.
 *
 * This module is the seam between a future audio/network session and the
 * product UI.  It does not read the microphone, open a socket, store audio,
 * or call a model provider.  A caller supplies the future transport later;
 * today it can exercise a complete turn and the eye-expression contract
 * without making the device depend on Wi-Fi or C5.
 */
typedef enum {
    MEO_CONV_IDLE = 0,
    MEO_CONV_LISTENING,
    MEO_CONV_THINKING,
    MEO_CONV_SPEAKING,
    MEO_CONV_COMPLETE,
    MEO_CONV_ERROR,
} meo_conversation_state_t;

typedef struct {
    uint32_t listening_ms;
    uint32_t thinking_ms;
    uint32_t speaking_ms;
    uint32_t complete_ms;
    uint32_t error_ms;
} meo_conversation_timing_t;

typedef struct {
    meo_conversation_state_t state;
    meo_expression_t expression;
    uint64_t state_started_ms;
    uint32_t generation;
    bool active;
} meo_conversation_view_t;

typedef void (*meo_conversation_changed_cb)(const meo_conversation_view_t *view,
                                             void *ctx);

typedef struct {
    meo_conversation_timing_t timing;
    meo_conversation_changed_cb changed;
    void *changed_ctx;
    meo_conversation_state_t state;
    meo_expression_t expression;
    uint64_t state_started_ms;
    uint64_t last_ms;
    uint32_t generation;
    bool initialized;
} meo_conversation_t;

meo_conversation_timing_t meo_conversation_default_timing(void);
void meo_conversation_init(meo_conversation_t *conversation, uint64_t now_ms,
                           meo_conversation_changed_cb changed, void *ctx);

/* Start exactly one local demo turn. Returns false while a turn is active. */
bool meo_conversation_start(meo_conversation_t *conversation, uint64_t now_ms);
bool meo_conversation_cancel(meo_conversation_t *conversation, uint64_t now_ms);
bool meo_conversation_fail(meo_conversation_t *conversation, uint64_t now_ms);
void meo_conversation_tick(meo_conversation_t *conversation, uint64_t now_ms);

bool meo_conversation_active(const meo_conversation_t *conversation);
meo_conversation_view_t meo_conversation_view(const meo_conversation_t *conversation);
const char *meo_conversation_state_name(meo_conversation_state_t state);
const char *meo_conversation_expression_name(meo_expression_t expression);

/* Fixed local content makes the demo auditable and keeps it offline. */
const char *meo_conversation_demo_prompt(void);
const char *meo_conversation_demo_response(void);
