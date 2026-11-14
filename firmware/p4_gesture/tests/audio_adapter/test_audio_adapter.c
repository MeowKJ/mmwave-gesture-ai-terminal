/* Actual app adapter with in-memory dependencies; never opens USB/I2S/NVS. */
#include "esp_err.h"
#define ESP_ERR_INVALID_STATE 0x103
#include "../../main/meoave_app.c"
#include <assert.h>
#include <stdarg.h>

static int64_t now;
static bool output_owned,task_failure,gui_busy,ui_busy,key_down,selftest_busy;
static bool capturing;
static bool volume_applied;
static int applied_volume=-1;
static bool volume_failure;
static unsigned played,clicks;
static unsigned audio_pushes,commits,mic_starts,mic_stops,poweroffs;
static int shown=-1;
static meo_audio_view_t delivered;
static bsp_audio_mic_route_t actual_route={.left_sel=6,.right_sel=5,.gain=4};
static meo_model_t fake_model;
static bool wide_status,capture_status;
static char status_record[768];
struct fake_queue {bool full;meo_settings_t item;};
static struct fake_queue queue;
static bool inject_request_at_lock;

void fake_enter_critical(portMUX_TYPE *l){(void)l;}
void fake_exit_critical(portMUX_TYPE *l){(void)l;}
void fake_error_check(esp_err_t e,const char *f,int l){(void)f;(void)l;assert(e==ESP_OK);}
void fake_log(const char *level,const char *tag,const char *format,...){(void)level;(void)tag;(void)format;}
const char *esp_err_to_name(esp_err_t err){(void)err;return "fake";}
int64_t esp_timer_get_time(void){return now;}
QueueHandle_t xQueueCreate(UBaseType_t n,UBaseType_t size){(void)n;(void)size;return &queue;}
BaseType_t xQueueOverwrite(QueueHandle_t q,const void *v){q->item=*(const meo_settings_t*)v;q->full=true;return pdTRUE;}
BaseType_t xQueueReceive(QueueHandle_t q,void *v,TickType_t wait){(void)wait;if(!q->full)return pdFALSE;*(meo_settings_t*)v=q->item;q->full=false;return pdTRUE;}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q){return q->full;}
BaseType_t xTaskCreate(void (*entry)(void*),const char*n,unsigned stack,void*arg,unsigned priority,TaskHandle_t*h)
{(void)n;(void)stack;(void)priority;(void)h;if(task_failure)return 0;entry(arg);return pdPASS;}
void vTaskDelete(TaskHandle_t h){(void)h;}
bool lvgl_port_lock(uint32_t timeout)
{
    if(gui_busy)return false;
    if(inject_request_at_lock && timeout==5) {
        inject_request_at_lock=false;assert(speaker_test_request());ui_busy=true;
    }
    return true;
}
void lvgl_port_unlock(void){}
void lv_async_call(void (*callback)(void*),void*arg){callback(arg);}
esp_err_t nvs_flash_init(void){return ESP_OK;}
esp_err_t nvs_open(const char*n,nvs_open_mode_t mode,nvs_handle_t*h){(void)n;(void)mode;*h=1;return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char*k,uint8_t*v){(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char*k,uint8_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;++commits;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t fls2_display_set_brightness(uint8_t v){(void)v;return ESP_OK;}
esp_err_t bsp_usb_cdc_write(const char*s)
{
    if(capture_status && !strncmp(s,"MEO:UI ",7))
        snprintf(status_record,sizeof(status_record),"%s",s);
    return ESP_OK;
}
int bsp_power_battery_percent(void){return 70;}
bool bsp_power_charging(void){return false;}
bool bsp_power_key_pressed(void){return key_down;}
esp_err_t bsp_power_off(void){++poweroffs;return ESP_OK;}
bool bsp_audio_output_try_acquire(void){if(output_owned)return false;output_owned=true;return true;}
void bsp_audio_output_release(void){assert(output_owned);output_owned=false;}
esp_err_t bsp_audio_preview_music(void){assert(volume_applied);++played;return ESP_FAIL;}
esp_err_t bsp_audio_click(void){if(output_owned)return ESP_ERR_INVALID_STATE;++clicks;return ESP_OK;}
esp_err_t bsp_audio_set_volume(int v){applied_volume=v;volume_applied=true;return volume_failure ? ESP_FAIL : ESP_OK;}
void bsp_audio_mic_route_default(bsp_audio_mic_route_t*r){*r=actual_route;}
void bsp_audio_mic_route_get(bsp_audio_mic_route_t*r){*r=actual_route;}
esp_err_t bsp_audio_mic_route_set(const bsp_audio_mic_route_t*r){actual_route=*r;return ESP_OK;}
esp_err_t bsp_audio_mic_start(void){++mic_starts;return ESP_OK;}
void bsp_audio_mic_stop(void){++mic_stops;}
bool bsp_audio_mic_active(void){return false;}
bool bsp_audio_mic_capturing(void){return capturing;}
bool bsp_audio_speaker_on(void){return false;}
esp_err_t bsp_audio_mic_set_frame_callback(bsp_audio_mic_frame_cb_t cb,void *ctx)
{(void)cb;(void)ctx;return ESP_OK;}
void meo_voice_link_init(void){}
void meo_ui_set_wake_state(bool detected){(void)detected;}
void meo_ui_capture_view(meo_ui_capture_view_t *v){memset(v,0,sizeof(*v));}
void bsp_audio_mic_level(int rms[2],int peak[2]){rms[0]=rms[1]=peak[0]=peak[1]=0;}
void bsp_audio_status(bsp_audio_status_t*out){memset(out,0,sizeof(*out));out->route=actual_route;out->volume=30;}
bool audio_selftest_busy(void){return selftest_busy;}
void audio_selftest_start(const char*t,int ms){(void)t;(void)ms;}
void audio_selftest_result(audio_loop_result_t*r){memset(r,0,sizeof(*r));}
void meo_ui_create(const meo_ui_config_t*c){meo_model_init(&fake_model,0,&c->settings);}
void meo_voice_view(meo_voice_view_t *v){memset(v,0,sizeof(*v));}
bool meo_voice_demo_available(void){return false;}
bool meo_voice_demo(void){return false;}
bool meo_voice_cancel(void){return false;}
void meo_ui_set_sentence_pose(meo_expression_t e,float level,const meo_face_pose_t *p)
{meo_model_set_media_pose(&fake_model,e,level,p);}
void meo_ui_auto_expression(void){meo_model_auto_expression(&fake_model);}
void meo_ui_set_expression(meo_expression_t expression,uint32_t hold_ms)
{meo_model_set_expression(&fake_model,expression,hold_ms);}
void meo_ui_settings_saved(bool ok){(void)ok;}
void meo_ui_set_audio(const meo_audio_view_t*v){delivered=*v;ui_busy=v->playback_busy;++audio_pushes;}
void meo_ui_navigate(meo_page_t p){fake_model.page=p;}
void meo_ui_set_volume(int v){fake_model.settings.volume=(uint8_t)v;changed(&fake_model.settings);}
void meo_ui_set_shutdown(int seconds){shown=seconds;}
void meo_ui_set_power(int pct,bool charging){(void)pct;(void)charging;}
void meo_ui_set_mic(const float level[2],bool running){(void)level;(void)running;}
bool meo_ui_font_has(const char*s){(void)s;return true;}
void meo_ui_fps_start(uint32_t ms,const char*t){(void)ms;(void)t;}
void meo_ui_fps_read(meo_ui_fps_t*r){memset(r,0,sizeof(*r));}
void meo_ui_perf(meo_ui_perf_t*r)
{
    memset(r,wide_status ? 0xFF : 0,sizeof(*r));
    if(wide_status)for(unsigned i=0;i<4;++i)r->max_area[i]=INT32_MIN;
}
uint32_t meo_ui_frames(void){return wide_status ? UINT32_MAX : 0;}
const char *meo_ui_page_name(meo_page_t p)
{return p==MEO_EXPRESSIONS ? "expressions" : "accessibility";}
const meo_model_t *meo_ui_model(void){return &fake_model;}
bool meo_ui_warmup_active(void){return false;}
int meo_ui_warmup_count(void){return 10;}

static void service_at(int ms){now=(int64_t)ms*1000;meoave_app_service();}
int main(void)
{
    setbuf(stdout,NULL);s_ready=s_storage_ok=true;s_changes=&queue;
    s_last_route=actual_route;s_pending=meo_default_settings();
    s_power_poll_at=s_mic_poll_at=INT64_MAX;
    task_failure=true;
    assert(speaker_test_request());ui_busy=true;service_at(0);service_at(210);
    assert(!ui_busy && delivered.playback_error==ESP_ERR_NO_MEM && !output_owned);
    uint32_t first=delivered.playback_seq;unsigned pushed=audio_pushes;
    assert(speaker_test_request());ui_busy=true;service_at(250);assert(ui_busy);
    service_at(420);assert(!ui_busy && audio_pushes>pushed && delivered.playback_seq==first+1);
    puts("PASS: repeated same-error fast failures acknowledge each accepted preview generation");
    /* Another request can be accepted while service waits for the GUI lock. */
    inject_request_at_lock=true;s_last_audio_view.volume=-1;
    service_at(630);assert(delivered.playback_seq==s_music_seq && ui_busy);
    service_at(840);assert(!ui_busy && !output_owned);
    puts("PASS: snapshot refreshed after GUI lock, no stale idle acknowledgement");
    commits=0;
    for(int i=0;i<45;++i) {
        meo_settings_t settings=meo_default_settings();settings.mic_gain=(uint8_t)(i%15);
        changed(&settings);service_at(1000+i*20);assert(commits==0);
    }
    service_at(2600);assert(commits==1);service_at(3000);assert(commits==1);
    puts("PASS: gain drag changes hardware but commits settings once after debounce, no 5 Hz route writes");
    selftest_busy=true;actual_route.left_sel=5;service_at(3300);
    actual_route.left_sel=6;service_at(3550);service_at(4300);assert(commits==1);
    selftest_busy=false;service_at(4600);assert(commits==2);
    puts("PASS: calibration route persistence waits for idle and settles");
    task_failure=false;meo_settings_t settings=meo_default_settings();settings.volume=0;
    volume_applied=false;changed(&settings);assert(speaker_test_request());service_at(5000);
    assert(volume_applied && !s_music_busy && !output_owned);
    unsigned before=played;volume_failure=true;
    assert(speaker_test_request());service_at(5005);
    assert(played==before && s_music_error==ESP_FAIL && !s_music_busy && !output_owned);
    volume_failure=false;
    mic_test_request(true);service_at(5010);mic_test_request(false);service_at(5020);
    assert(mic_starts==1 && mic_stops==1);
    puts("PASS: queued volume precedes preview task; independent microphone callback start/stop");
    gui_busy=true;meoave_app_request_volume(30);service_at(5030);
    assert(s_volume_req==30);
    gui_busy=false;service_at(5040);
    assert(fake_model.settings.volume==30 && s_pending.volume==30 && applied_volume==30);
    meoave_app_request_volume(-1);meoave_app_request_volume(101);service_at(5050);
    assert(fake_model.settings.volume==30 && s_volume_req==-1);
    assert(meoave_app_request_preview());
    assert(!meoave_app_request_preview());
    service_at(5060);assert(!s_music_busy && !output_owned && applied_volume==30);
    s_ready=false;assert(!meoave_app_request_preview());s_ready=true;
    s_shutdown=true;assert(!meoave_app_request_preview());s_shutdown=false;
    puts("PASS: native USB diagnostics use queued UI volume and the same preview lease, with bounds/readiness guards");
    settings=meo_default_settings();settings.volume=30;settings.interaction_sound=true;
    changed(&settings);interaction_sound_request();interaction_sound_request();service_at(5100);
    assert(clicks==1); /* coalesced, not a delayed burst of clicks */
    settings.interaction_sound=false;changed(&settings);interaction_sound_request();service_at(5110);
    assert(clicks==1 && !s_interaction_requested);
    settings.interaction_sound=true;settings.volume=0;changed(&settings);
    interaction_sound_request();service_at(5120);assert(clicks==1);
    settings.volume=30;changed(&settings);output_owned=true;
    interaction_sound_request();service_at(5130);assert(clicks==1);output_owned=false;
    settings.volume=40;changed(&settings);volume_failure=true;
    interaction_sound_request();service_at(5140);assert(clicks==1);volume_failure=false;
    changed(&settings);interaction_sound_request();service_at(5150);assert(clicks==2);
    assert(s_interaction_requests==7 && s_interaction_played==2 && s_interaction_error==ESP_OK);
    assert(meoave_app_request_interaction_sound());service_at(5160);
    assert(s_interaction_requests==8 && s_interaction_played==3);
    s_ready=false;assert(!meoave_app_request_interaction_sound());s_ready=true;
    puts("PASS: dong coalescing, current enable switch, mute without amp cue, playback lease and failed-volume guard");
    s_ai_view_seq=s_ai_applied_seq=0;
    s_ai_request=0;
    meo_conversation_init(&s_conversation,0,conversation_changed,NULL);
    assert(meoave_app_request_ai_demo());
    service_at(0);    assert(fake_model.expression==MEO_EYE_CURIOUS && fake_model.expression_override);
    service_at(1200); assert(fake_model.expression==MEO_EYE_SLEEPY && fake_model.expression_override);
    service_at(2000); assert(fake_model.expression==MEO_EYE_HAPPY && fake_model.expression_override);
    service_at(3800); assert(fake_model.expression==MEO_EYE_CALM && fake_model.expression_override);
    service_at(4500); assert(fake_model.expression==MEO_EYE_CALM && !fake_model.expression_override);
    assert(!meoave_app_request_ai_cancel());
    puts("PASS: app-level offline AI demo drives CURIOUS/SLEEPY/HAPPY/CALM and restores idle expressions");
    capture_status=wide_status=true;
    fake_model.page=MEO_EXPRESSIONS;fake_model.elapsed_ms=UINT64_MAX;
    meoave_app_request_status();service_at(4600);
    assert(strstr(status_record,"page=expressions "));
    assert(strstr(status_record,"area=-2147483648,-2147483648,-2147483648,-2147483648\n"));
    assert(status_record[strlen(status_record)-1]=='\n');
    assert(strlen(status_record)>320 && strlen(status_record)<512);
    capture_status=wide_status=false;fake_model.elapsed_ms=0;
    puts("PASS: long page name, maximum counters/timestamps and signed areas produce one complete newline-terminated USB status record");
    /* GUI contention must not masquerade as repeated page exits. Real page
       transitions still close the wake gate immediately, even while busy. */
    capturing=true;ai_mic_test_request(true);gui_busy=false;service_at(4700);
    meo_wake_view_t wake;meo_wake_view(&wake);assert(wake.requested);
    gui_busy=true;service_at(4710);service_at(4720);
    meo_wake_view(&wake);assert(wake.requested);
    ai_mic_test_request(false);service_at(4730);
    meo_wake_view(&wake);assert(!wake.requested);
    gui_busy=false;capturing=false;
    puts("PASS: renderer lock contention preserves listening; real page exit disarms");
    s_shutdown=false;key_down=true;now=6000000;meoave_app_key_tick();
    now=7500000;meoave_app_key_tick();assert(shown==3);
    gui_busy=true;key_down=false;now=7600000;meoave_app_key_tick();assert(shown==3);
    gui_busy=false;now=7620000;meoave_app_key_tick();assert(shown==-1 && poweroffs==0);
    key_down=true;now=8000000;meoave_app_key_tick();now=9500000;meoave_app_key_tick();
    assert(shown==3);now=12500000;meoave_app_key_tick();assert(poweroffs==1 && shown==0);
    puts("PASS: release cancellation survives GUI lock failure; power drops after 1.5+3 seconds");
    return 0;
}
