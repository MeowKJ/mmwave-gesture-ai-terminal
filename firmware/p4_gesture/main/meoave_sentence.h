#pragma once
#include "meoave_model.h"
#include <stddef.h>

#define MEO_SENTENCE_RATE 48000u
#define MEO_SENTENCE_MAX_FRAMES (MEO_SENTENCE_RATE * 30u)
#define MEO_SENTENCE_MAX_CUES 32u
#define MEO_SENTENCE_TEXT_BYTES 768u
#define MEO_SENTENCE_JSON_BYTES 8192u

typedef struct {
    uint32_t at_ms;
    uint16_t transition_ms;
    meo_expression_t expression;
    float intensity;
} meo_expression_cue_t;

typedef struct {
    uint32_t generation, sentence_id, frames;
    char text[MEO_SENTENCE_TEXT_BYTES+1];
    unsigned cue_count;
    meo_expression_cue_t cues[MEO_SENTENCE_MAX_CUES];
} meo_sentence_t;

/* Only fixed-size semantic data, never code, paths or GPIO operations. */
bool meo_sentence_valid(const meo_sentence_t *sentence);
bool meo_sentence_parse(const char *json, size_t bytes, meo_sentence_t *out);

typedef struct {
    meo_sentence_t sentence;
    uint32_t generation, last_sentence, played_frames;
    bool active;
} meo_sentence_player_t;

typedef struct {
    meo_face_pose_t pose;
    meo_expression_t expression;
    float intensity;
    unsigned cue_index;
    uint32_t played_frames;
    bool finished;
} meo_sentence_frame_t;

/* Caller serializes these calls. New generations strictly increase until
 * device restart; sentence IDs start at one with no holes or duplicates. */
bool meo_sentence_turn(meo_sentence_player_t *player, uint32_t generation);
bool meo_sentence_begin(meo_sentence_player_t *player, const meo_sentence_t *sentence);
void meo_sentence_cancel(meo_sentence_player_t *player);
/* completed_frames MUST come from TX completion, NOT write()/network progress.
 * Stalls do not advance phase; paused/underrun/silent output closes the mouth.
 * Large jumps evaluate the current pose, never replay a backlog of moods. */
bool meo_sentence_sample(meo_sentence_player_t *player, uint32_t completed_frames,
                          bool audible, float envelope, bool reduced_motion,
                          meo_sentence_frame_t *out);
