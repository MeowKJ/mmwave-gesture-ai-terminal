#include "meoave_sentence.h"
#include "meoave_pcm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static meo_sentence_t sentence(void)
{
    return (meo_sentence_t){.generation=1,.sentence_id=1,.frames=96000,.text="你好，我在这里。",
        .cue_count=3,.cues={{0,100,MEO_EYE_CURIOUS,.5f},{500,180,MEO_EYE_HAPPY,.8f},
                          {1300,200,MEO_EYE_RELIEVED,.7f}}};
}

static void expressions(void)
{
    assert(MEO_EYE_COUNT>=16);
    meo_face_pose_t neutral,poses[MEO_EYE_COUNT],animated;
    assert(meo_expression_pose(MEO_EYE_CALM,1,0,false,&neutral));
    for (int e=0;e<MEO_EYE_COUNT;++e) {
        assert(strcmp(meo_expression_name((meo_expression_t)e),"INVALID"));
        assert(meo_expression_pose((meo_expression_t)e,0,725,true,&animated));
        assert(!memcmp(&neutral,&animated,sizeof(neutral)));
        assert(meo_expression_pose((meo_expression_t)e,1,0,false,&poses[e]));
        for (int j=0;j<e;++j) assert(memcmp(&poses[e],&poses[j],sizeof(neutral)));
        assert(meo_expression_pose((meo_expression_t)e,.5f,0,false,&animated));
        assert(fabsf(animated.eye[0].width-(neutral.eye[0].width+poses[e].eye[0].width)/2)<.001f);
        if (e) {
            assert(meo_expression_pose((meo_expression_t)e,1,320,true,&animated));
            assert(memcmp(&poses[e],&animated,sizeof(neutral)));
        }
    }
    assert(!meo_expression_pose(MEO_EYE_HAPPY,NAN,0,false,&animated));
    meo_model_t m;meo_model_init(&m,0,NULL);
    meo_model_set_media_pose(&m,MEO_EYE_HAPPY,.5f,&poses[MEO_EYE_HAPPY]);
    meo_model_tick(&m,5000);assert(!memcmp(m.eye_shape,poses[MEO_EYE_HAPPY].eye,sizeof(m.eye_shape)));
    assert(m.expression_media);
    meo_model_auto_expression(&m);assert(!m.expression_media);
}

static void timeline(void)
{
    meo_sentence_t s=sentence(),bad=s;
    assert(meo_sentence_valid(&s));
    bad.cue_count=0;assert(!meo_sentence_valid(&bad));
    bad=s;bad.cues[0].at_ms=1;assert(!meo_sentence_valid(&bad));
    bad=s;bad.cues[1].at_ms=0;assert(!meo_sentence_valid(&bad));
    bad=s;bad.cues[0].intensity=NAN;assert(!meo_sentence_valid(&bad));
    bad=s;bad.cues[1].transition_ms=1000;assert(!meo_sentence_valid(&bad));
    bad=s;bad.cues[2].expression=MEO_EYE_COUNT;assert(!meo_sentence_valid(&bad));
    meo_sentence_player_t p={0},q={0};
    assert(!meo_sentence_begin(&p,&s));assert(meo_sentence_turn(&p,1));
    assert(meo_sentence_begin(&p,&s));assert(!meo_sentence_begin(&p,&s));
    meo_sentence_frame_t a,b;
    assert(meo_sentence_sample(&p,0,false,0,false,&a));assert(a.cue_index==0 && !a.pose.mouth.aperture);
    assert(meo_sentence_sample(&p,24000,true,.6f,false,&a));assert(a.cue_index==1);
    /* A wall clock is deliberately absent from this API. */
    for (int i=0;i<50;++i) {
        assert(meo_sentence_sample(&p,24000,false,0,false,&b));
        assert(!memcmp(a.pose.eye,b.pose.eye,sizeof(a.pose.eye)) && !b.pose.mouth.aperture);
    }
    assert(!meo_sentence_sample(&p,23999,true,.5f,false,&b));
    assert(!meo_sentence_sample(&p,96001,true,.5f,false,&b));
    assert(meo_sentence_sample(&p,80000,true,.4f,false,&a));
    meo_sentence_turn(&q,1);meo_sentence_begin(&q,&s);
    for (unsigned i=0;i<=80000;i+=100) assert(meo_sentence_sample(&q,i,true,.4f,false,&b));
    assert(!memcmp(&a.pose,&b.pose,sizeof(a.pose))); /* skipped UI ticks */
    assert(meo_sentence_sample(&p,96000,true,1,false,&a));assert(a.finished && !p.active && !a.pose.mouth.aperture);
    assert(!meo_sentence_begin(&p,&s));s.sentence_id=2;assert(meo_sentence_begin(&p,&s));
    meo_sentence_cancel(&p);s.sentence_id=3;assert(!meo_sentence_begin(&p,&s));
    assert(!meo_sentence_turn(&p,1));assert(meo_sentence_turn(&p,2));
    assert(!meo_sentence_begin(&p,&s));s.generation=2;s.sentence_id=1;assert(meo_sentence_begin(&p,&s));
    assert(meo_sentence_sample(&p,100,true,1,true,&a));
    meo_face_pose_t still;meo_expression_pose(MEO_EYE_CURIOUS,.5f,0,false,&still);
    assert(!memcmp(a.pose.eye,still.eye,sizeof(still.eye)));
}

static void json_parser(void)
{
    const char *json="{\"v\":1,\"generation\":1,\"sentence_id\":1,\"sample_rate\":48000,"
        "\"channels\":1,\"format\":\"pcm_s16le\",\"frames\":48000,\"text\":\"hello\",\"expressions\":[{\"at_ms\":0,\"expression\":\"HAPPY\","
        "\"intensity\":0.8,\"transition_ms\":100}]}";
    meo_sentence_t parsed,before=sentence();
    assert(meo_sentence_parse(json,strlen(json),&parsed));
    assert(parsed.cue_count==1 && parsed.cues[0].expression==MEO_EYE_HAPPY);
    char text[1024];
    const char *bad[]={"{}","[]","null","{\"x\":[[[[[[[]]]]]]]}","{\"text\":\"\\u0000\"}","{broken}","1"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        parsed=before;assert(!meo_sentence_parse(bad[i],strlen(bad[i]),&parsed));
        assert(!memcmp(&before,&parsed,sizeof(parsed)));
    }
    snprintf(text,sizeof(text),"%s garbage",json);assert(!meo_sentence_parse(text,strlen(text),&parsed));
    snprintf(text,sizeof(text),"{\"v\":1,%s",json+1);assert(!meo_sentence_parse(text,strlen(text),&parsed));
    /* Parse with no terminating NUL, as on a bounded network frame. */
    char *exact=malloc(strlen(json));memcpy(exact,json,strlen(json));
    assert(meo_sentence_parse(exact,strlen(json),&parsed));free(exact);
    srand(42);
    for (unsigned i=0;i<10000;++i) {
        size_t n=(unsigned)rand()%sizeof(text);
        for (size_t j=0;j<n;++j) text[j]=(char)(rand()%128);
        (void)meo_sentence_parse(text,n,&parsed);
    }
}

static void pcm_clock(void)
{
    static meo_pcm_t p;
    int16_t dma[6][240*2]={{0}},block[240*2];
    assert(meo_pcm_open(&p,240*40+101));
    unsigned submitted=0,source_block=0,played=0;
    /* Fill each descriptor once while paused: queued audio must not count. */
    for (int i=0;i<6;++i) meo_pcm_sent(&p,dma[i],sizeof(dma[i]));
    assert(!p.completed);p.paused=false;
    for (unsigned step=0;step<200 && (p.active || p.draining);++step) {
        while (submitted<p.total && p.count<12) {
            unsigned n=p.total-submitted;if(n>240)n=240;
            for (unsigned i=0;i<n*2;++i)block[i]=(int16_t)(source_block+1);
            assert(meo_pcm_push(&p,block,n));submitted+=n;++source_block;
        }
        unsigned slot=step%6;
        unsigned n=p.dma[slot].frames;
        if (n) {
            assert(dma[slot][0]==(int16_t)(played/240+1));
            played+=n;
        }
        meo_pcm_sent(&p,dma[slot],sizeof(dma[slot]));
        assert(p.completed==played);assert(p.completed<=submitted);
    }
    assert(p.completed==p.total && !p.active && !p.draining && !p.fault && !p.underruns);
    assert(meo_pcm_open(&p,2400));
    for (unsigned i=0;i<480;++i)block[i]=6000;
    assert(meo_pcm_push(&p,block,240));p.paused=false;
    for (unsigned i=0;i<7;++i)meo_pcm_sent(&p,dma[i%6],sizeof(dma[0]));
    assert(p.completed==240 && p.envelope==1000);
    for (unsigned i=0;i<20;++i)meo_pcm_sent(&p,dma[i%6],sizeof(dma[0]));
    assert(p.completed==240 && !p.audible && p.underruns>0); /* true starvation */
    p.paused=true;unsigned under=p.underruns;
    for (unsigned i=0;i<6;++i)meo_pcm_sent(&p,dma[i],sizeof(dma[i]));
    assert(p.underruns==under);
    meo_pcm_cancel(&p);
    assert(!meo_pcm_open(&p,240));
    for (unsigned i=0;i<6;++i)meo_pcm_sent(&p,dma[i],sizeof(dma[i]));
    assert(!p.draining && p.completed==240);
    for (unsigned i=0;i<6;++i)for(unsigned j=0;j<480;++j)assert(!dma[i][j]);
    assert(meo_pcm_open(&p,240));meo_pcm_sent(&p,dma[0],12);assert(p.fault && !p.active);
}

int main(int argc,char **argv)
{
    if (argc==2) {
        char json[MEO_SENTENCE_JSON_BYTES+1];meo_sentence_t s;
        FILE *f=fopen(argv[1],"rb");assert(f);
        size_t n=fread(json,1,sizeof(json),f);fclose(f);
        assert(meo_sentence_parse(json,n,&s));
        printf("PASS: gateway packet parsed by device code; sentence=%u cues=%u\n",s.sentence_id,s.cue_count);
        return 0;
    }
    expressions();timeline();json_parser();pcm_clock();
    puts("PASS: 16 unique animated poses/strength, mandatory sentence chains, parser limits/fuzz,");
    puts("      exact DMA completion clock, pause/underrun/cancel, stale generations and skipped UI frames");
    return 0;
}
