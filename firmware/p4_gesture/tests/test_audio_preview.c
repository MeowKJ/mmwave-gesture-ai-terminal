#include "audio_preview.h"
#include "audio_click.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int main(void)
{
    meo_preview_t a={0},b={0};
    int16_t block[960],single[2];
    unsigned frames=0,crossings=0,nonzero=0; int previous=0,peak=0;
    uint64_t music_energy=0;
    size_t count;
    while ((count=meo_preview_render(&a,block,480))) {
        for (size_t i=0;i<count;++i) {
            assert(meo_preview_render(&b,single,1)==1);
            assert(block[2*i]==single[0] && block[2*i+1]==single[1]);
            assert(single[0]==single[1]);
            int v=single[0];if (abs(v)>peak) peak=abs(v);
            if (v*previous<0) ++crossings;
            if (frames >= MEO_PREVIEW_MUSIC_FRAMES) assert(v==0); /* final 100 ms rest */
            else music_energy+=(uint64_t)((int64_t)v*v);
            previous=v;++frames;
            if (v) ++nonzero;
        }
    }
    assert(frames==MEO_PREVIEW_TOTAL_FRAMES);
    assert(frames>48000 && frames<96000);
    /* A quiet source must not silently defeat a 100% user volume. Leave at
       least ~3.5 dB sample headroom; do not hard-clip or change the codec. */
    assert(peak>=21000 && peak<=22000 && crossings>500 && nonzero>frames/2);
    double music_rms=sqrt((double)music_energy/MEO_PREVIEW_MUSIC_FRAMES);
    assert(music_rms>5000 && music_rms<6500);
    printf("Music sample peak=%d RMS=%.1f, no clipped samples\n",peak,music_rms);
    assert(meo_preview_render(&a,block,480)==0);
    assert(meo_preview_render(NULL,block,480)==0);
    assert(meo_preview_render(&b,NULL,480)==0);
    meo_preview_t zero={0};assert(meo_preview_render(&zero,block,0)==0 && zero.position==0);
    int16_t sine[512];
    for(int i=0;i<512;++i)sine[i]=(int16_t)lround(32767*sin(i*6.283185307179586/512));
    meo_click_t ca={0},cb={0};frames=0;peak=0;
    while((count=meo_click_render(&ca,block,480,sine))) {
        for(size_t i=0;i<count;++i) {
            assert(meo_click_render(&cb,single,1,sine)==1);
            assert(block[2*i]==single[0] && block[2*i+1]==single[1]);
            int v=single[0];if(abs(v)>peak)peak=abs(v);
            if(frames==0 || frames==MEO_CLICK_FRAMES-1)assert(v==0);
            if(frames>=MEO_CLICK_FRAMES-96)assert(abs(v)<20);
            ++frames;
        }
    }
    assert(frames==MEO_CLICK_FRAMES && peak>3500 && peak<7000);
    assert(meo_click_render(&ca,block,480,sine)==0);
    puts("PASS: CC0 PCM music, bounded headroom, stereo, exact duration, final silence, chunk invariance; soft dong zero-ended envelope");
    return 0;
}
