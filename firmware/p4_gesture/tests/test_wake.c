#include "meoave_wake.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static meoave_espsr_view_t engine;
void fake_log(const char *level,const char *tag,const char *fmt,...)
{(void)level;(void)tag;(void)fmt;}
esp_err_t meoave_espsr_init(void)
{
    memset(&engine,0,sizeof(engine));
    strcpy(engine.backend,"ESP_SR_UNAVAILABLE"); strcpy(engine.error,"MODEL_MISSING");
    return ESP_ERR_NOT_FOUND;
}
void meoave_espsr_set_armed(bool armed)
{engine.requested=armed;engine.armed=armed&&engine.keyword_ready;}
void meoave_espsr_feed_stereo_48k(const int16_t *pcm,size_t frames,uint64_t now)
{(void)pcm;(void)frames;(void)now;}
void meoave_espsr_view(meoave_espsr_view_t *out){*out=engine;}

int main(void)
{
    meo_wake_view_t view;
    meo_wake_init(100);
    meo_wake_set_armed(true,100);
    meo_wake_view(&view);
    assert(view.requested && !view.armed); /* requested is NOT model readiness */
    assert(!meo_wake_submit_text("你好，Meoave",120));
    assert(!meo_wake_submit_text("你好猫猫",130));
    assert(meo_wake_submit_text("猫猫",200));
    meo_wake_service(210); meo_wake_view(&view);
    assert(view.detected && view.detections==1 && view.text_test_detections==1);
    assert(!view.acoustic_detections && !strcmp(view.event_source,"TEXT_BRIDGE_TEST"));
    assert(!strcmp(view.backend,"ESP_SR_UNAVAILABLE")); /* refresh cannot relabel source */
    assert(!meo_wake_submit_text("猫猫",300)); /* debounce */
    meo_wake_tick(2101); meo_wake_view(&view); assert(!view.detected);
    meo_wake_set_armed(false,2200); assert(!meo_wake_submit_text("猫猫",2300));
    engine.detections=1; meo_wake_service(2400);
    engine.model_loaded=engine.keyword_ready=true;
    strcpy(engine.backend,"ESP_SR_WAKENET"); engine.error[0]=0;
    meo_wake_set_armed(true,2500);
    meo_wake_service(2501); meo_wake_view(&view);
    assert(view.detections==1); /* disarmed acoustic result consumed, not replayed */
    engine.detections=2; engine.last_detected_ms=3000;
    meo_wake_service(3010); meo_wake_view(&view);
    assert(view.armed && view.detected && view.detections==2 && view.acoustic_detections==1);
    assert(view.text_test_detections==1 && !strcmp(view.event_source,"ACOUSTIC_ESP_SR"));
    meo_wake_tick(4801); meo_wake_view(&view); assert(!view.detected);
    engine.detections=3; engine.last_detected_ms=5000; engine.armed=false;
    strcpy(engine.error,"MODEL_REBUILDING");
    meo_wake_service(5010); meo_wake_view(&view);
    assert(view.acoustic_detections==2 && view.detected); /* accepted before rebuild */
    meo_wake_set_armed(false,5100); engine.detections=4;
    meo_wake_service(5110); meo_wake_view(&view);
    assert(view.acoustic_detections==2 && !view.detected); /* page exit still discards */
    puts("PASS: readiness vs request, exact text-test keyword, debounce, provenance, acoustic events, disarmed discard");
    return 0;
}
