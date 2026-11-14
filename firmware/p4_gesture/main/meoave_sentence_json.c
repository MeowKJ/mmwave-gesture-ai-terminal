#include "meoave_sentence.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

static const cJSON *field(const cJSON *object,const char *name)
{
    if (!object) return NULL;
    const cJSON *found=NULL;
    for (const cJSON *v=object->child;v;v=v->next) {
        if (v->string && !strcmp(v->string,name)) {
            if (found) return NULL; /* Ambiguous duplicate fields are invalid. */
            found=v;
        }
    }
    return found;
}

static bool number(const cJSON *o,const char *name,double max,double *out)
{
    const cJSON *v=field(o,name);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble<0 || v->valuedouble>max) return false;
    *out=v->valuedouble;return true;
}

static bool integer(const cJSON *o,const char *name,uint32_t max,uint32_t *out)
{
    double n;
    if (!number(o,name,max,&n) || floor(n)!=n) return false;
    *out=(uint32_t)n;return true;
}

bool meo_sentence_parse(const char *json,size_t bytes,meo_sentence_t *out)
{
    if (!json || !out || !bytes || bytes>MEO_SENTENCE_JSON_BYTES || memchr(json,0,bytes)) return false;
    unsigned depth=0;bool quoted=false,escape=false;
    for (size_t i=0;i<bytes;++i) {
        char c=json[i];
        if (quoted) {
            if (escape) escape=false;
            else if (c=='\\') escape=true;
            else if (c=='"') quoted=false;
        } else if (c=='"') quoted=true;
        else if (c=='{' || c=='[') {if (++depth>6) return false;}
        else if (c=='}' || c==']') {if (!depth) return false;--depth;}
    }
    if (quoted || depth) return false;
    /* cJSON strings cannot represent embedded NUL; fail closed instead of
       accepting a truncated expression ID/text. No unbounded C-string reads. */
    for (size_t i=0;i+6<=bytes;++i) if (!memcmp(json+i,"\\u0000",6)) return false;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(json,bytes,&end,false);
    if (!root) return false;
    while (end<json+bytes && (*end==' ' || *end=='\n' || *end=='\t' || *end=='\r')) ++end;
    bool ok=cJSON_IsObject(root) && end==json+bytes;
    meo_sentence_t s={0};uint32_t v,rate,channels;
    const cJSON *format=field(root,"format");
    ok=ok && integer(root,"v",1,&v) && v==1 && integer(root,"sample_rate",48000,&rate) && rate==48000 &&
       integer(root,"channels",1,&channels) && channels==1 && cJSON_IsString(format) &&
       !strcmp(format->valuestring,"pcm_s16le") &&
       integer(root,"generation",UINT32_MAX,&s.generation) && integer(root,"sentence_id",UINT32_MAX,&s.sentence_id) &&
       integer(root,"frames",MEO_SENTENCE_MAX_FRAMES,&s.frames);
    const cJSON *text=field(root,"text"),*chain=field(root,"expressions");
    ok=ok && cJSON_IsString(text) && strlen(text->valuestring)<=MEO_SENTENCE_TEXT_BYTES && cJSON_IsArray(chain);
    if (ok) {
        strcpy(s.text,text->valuestring);
        s.cue_count=(unsigned)cJSON_GetArraySize(chain);
        ok=s.cue_count>0 && s.cue_count<=MEO_SENTENCE_MAX_CUES;
    }
    for (unsigned i=0;ok && i<s.cue_count;++i) {
        const cJSON *cue=cJSON_GetArrayItem(chain,(int)i),*name=field(cue,"expression");
        double intensity;uint32_t transition;
        ok=cJSON_IsObject(cue) && cJSON_IsString(name) &&
           integer(cue,"at_ms",30000,&s.cues[i].at_ms) &&
           integer(cue,"transition_ms",2000,&transition) && number(cue,"intensity",1,&intensity);
        if (!ok) break;
        s.cues[i].expression=MEO_EYE_COUNT;
        for (int e=0;e<MEO_EYE_COUNT;++e) if (!strcmp(name->valuestring,meo_expression_name((meo_expression_t)e)))
            s.cues[i].expression=(meo_expression_t)e;
        s.cues[i].transition_ms=(uint16_t)transition;s.cues[i].intensity=(float)intensity;
    }
    ok=ok && meo_sentence_valid(&s);
    cJSON_Delete(root);
    if (ok) *out=s;
    return ok;
}
