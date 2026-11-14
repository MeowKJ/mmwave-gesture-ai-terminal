#include "meoave_sentence.h"
#include <math.h>
#include <string.h>

bool meo_sentence_valid(const meo_sentence_t *s)
{
    if (!s || !s->generation || !s->sentence_id || !s->frames ||
        s->frames>MEO_SENTENCE_MAX_FRAMES || !s->text[0] ||
        !memchr(s->text,0,sizeof(s->text)) || !s->cue_count ||
        s->cue_count>MEO_SENTENCE_MAX_CUES || s->cues[0].at_ms!=0) return false;
    bool meaningful=false;
    for (const unsigned char *p=(const unsigned char*)s->text;*p;++p)
        if (*p>32) meaningful=true;
    if (!meaningful) return false;
    for (unsigned i=0;i<s->cue_count;++i) {
        const meo_expression_cue_t *c=&s->cues[i];
        uint64_t start=(uint64_t)c->at_ms*48, end=start+(uint64_t)c->transition_ms*48;
        if (c->expression<0 || c->expression>=MEO_EYE_COUNT ||
            !isfinite(c->intensity) || c->intensity<0 || c->intensity>1 ||
            c->transition_ms>2000 || start>=s->frames || end>s->frames ||
            (i && c->at_ms<=s->cues[i-1].at_ms) ||
            (i+1<s->cue_count && end>(uint64_t)s->cues[i+1].at_ms*48)) return false;
    }
    return true;
}

bool meo_sentence_turn(meo_sentence_player_t *p, uint32_t generation)
{
    if (!p || !generation || generation<=p->generation) return false;
    p->generation=generation;p->last_sentence=0;p->played_frames=0;p->active=false;
    return true;
}

bool meo_sentence_begin(meo_sentence_player_t *p, const meo_sentence_t *s)
{
    if (!p || p->active || !meo_sentence_valid(s) || s->generation!=p->generation ||
        p->last_sentence==UINT32_MAX || s->sentence_id!=p->last_sentence+1) return false;
    p->sentence=*s;p->last_sentence=s->sentence_id;p->played_frames=0;p->active=true;
    return true;
}

void meo_sentence_cancel(meo_sentence_player_t *p)
{
    if (p) {
        p->active=false;
        /* Seal the canceled generation: no late next-sentence can restart it. */
        p->last_sentence=UINT32_MAX;
        p->played_frames=0;
    }
}

static float ease(uint32_t age_frames, uint32_t duration_ms)
{
    if (!duration_ms || age_frames>=duration_ms*48) return 1;
    float t=(float)age_frames/(duration_ms*48);
    return t*t*(3-2*t);
}

bool meo_sentence_sample(meo_sentence_player_t *p, uint32_t played, bool audible,
                          float envelope, bool reduced, meo_sentence_frame_t *out)
{
    if (!p || !p->active || !out || played<p->played_frames || played>p->sentence.frames ||
        !isfinite(envelope) || envelope<0 || envelope>1) return false;
    const meo_sentence_t *s=&p->sentence;
    meo_face_pose_t from,target,result;
    meo_expression_pose(MEO_EYE_CALM,1,0,false,&from);
    result=from;
    unsigned selected=0;
    /* Compute each transition's initial pose at its exact audio timestamp,
       independent of UI tick frequency, network jitter or skipped frames. */
    for (unsigned i=0;i<s->cue_count;++i) {
        const meo_expression_cue_t *c=&s->cues[i];
        uint32_t at=c->at_ms*48;
        if (at>played) break;
        selected=i;
        uint32_t until=played;
        if (i+1<s->cue_count && s->cues[i+1].at_ms*48<=played)
            until=s->cues[i+1].at_ms*48;
        meo_expression_pose(c->expression,c->intensity,(until-at)/48,!reduced,&target);
        meo_face_blend(&from,&target,reduced ? 1 : ease(until-at,c->transition_ms),&result);
        from=result;
    }
    result.mouth.aperture=audible && played<s->frames ? envelope*12 : 0;
    *out=(meo_sentence_frame_t){.pose=result,.expression=s->cues[selected].expression,
        .intensity=s->cues[selected].intensity,.cue_index=selected,.played_frames=played,
        .finished=played==s->frames};
    p->played_frames=played;
    if (out->finished) p->active=false;
    return true;
}
