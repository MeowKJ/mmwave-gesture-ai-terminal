/* White-box state assertions plus real rotated LVGL pointer/render tests.
 * This target includes the production UI, never a parallel mock renderer. */
#include "../../main/meoave_ui.c"
#include <assert.h>

static uint8_t framebuffer[460*460*3],drawbuffer[460*460*3];
static lv_indev_data_t input;
static unsigned connects,mic_starts,mic_stops,previews,saves,feedbacks;
static bool accept_connection,accept_preview=true;
static unsigned scan_calls;
static bool scan_cb(void) {++scan_calls;return true;}
static char connected_pass[65],joined_ssid[33];
static const char *output;
static uint8_t played_volume;
static bool connect_cb(const char *ssid,const char *pass)
{
    (void)ssid;++connects;snprintf(connected_pass,sizeof(connected_pass),"%s",pass);
    return accept_connection;
}
static bool join_cb(const char *ssid)
{snprintf(joined_ssid,sizeof(joined_ssid),"%s",ssid);return false;}
static void mic_cb(bool on) {if(on)++mic_starts;else ++mic_stops;}
static bool preview_cb(void) {++previews;played_volume=ui.model.settings.volume;return accept_preview;}
static void save_cb(const meo_settings_t *s) {(void)s;++saves;}
static void feedback_cb(void) {++feedbacks;}
static void flush_cb(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
    int width=area->x2-area->x1+1;
    for(int y=area->y1;y<=area->y2;++y)
        memcpy(framebuffer+(y*460+area->x1)*3,pixels+(y-area->y1)*width*3,width*3);
    lv_display_flush_ready(display);
}
static void read_cb(lv_indev_t *indev,lv_indev_data_t *data) {(void)indev;*data=input;}
static void advance_ms(int ms) {for(int i=0;i<ms;i+=5){lv_tick_inc(5);lv_timer_handler();}}
static void point(int x,int y,bool down)
{input.point=(lv_point_t){459-x,459-y};input.state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void quick_tap(int x,int y) {point(x,y,true);advance_ms(80);point(x,y,false);advance_ms(80);}
static void tap_at(int x,int y) {quick_tap(x,y);advance_ms(250);}
static void drag_at(int x,int y,int x2,int y2)
{
    point(x,y,true);advance_ms(80);
    for(int n=1;n<=10;++n){point(x+(x2-x)*n/10,y+(y2-y)*n/10,true);advance_ms(40);}
    point(x2,y2,false);advance_ms(600);
}
static void navigate(meo_page_t page) {meo_ui_navigate(page);advance_ms(500);assert(ui.model.page==page);}
static void snapshot(const char *name)
{
    if(!output)return;
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",output,name);
    lv_refr_now(NULL);FILE*f=fopen(path,"wb");assert(f);
    fprintf(f,"P6\n460 460\n255\n");
    for(unsigned i=0;i<sizeof(framebuffer);i+=3){fputc(framebuffer[i+2],f);fputc(framebuffer[i+1],f);fputc(framebuffer[i],f);}
    fclose(f);
}
static void assert_type(lv_obj_t *parent);
static unsigned visible_text_count(lv_obj_t *parent,const char *needle)
{
    if(lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN))return 0;
    unsigned n=lv_obj_check_type(parent,&lv_label_class) &&
               !strcmp(lv_label_get_text(parent),needle);
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);++i)
        n+=visible_text_count(lv_obj_get_child(parent,(int)i),needle);
    return n;
}

static void test_keyboard_and_feedback(void)
{
    meo_ui_password_open("Small-device AP");advance_ms(400);
    for(unsigned i=0;i<PWD_KEY_COUNT;++i) {
        lv_area_t a;lv_obj_get_coords(ui.pwd_keys[i],&a);
        assert(a.x1>=24 && a.x2<437 && a.y1>=12 && a.y2<451);
        assert(lv_obj_get_width(ui.pwd_keys[i])>=64 && lv_obj_get_height(ui.pwd_keys[i])>=64);
    }
    assert(lv_obj_has_state(ui.pwd_next,LV_STATE_DISABLED));
    quick_tap(230,208);assert(!strcmp(s_pwd.text,"a"));
    assert(!lv_obj_has_state(ui.pwd_next,LV_STATE_DISABLED));
    assert(!strcmp(lv_label_get_text(ui.pwd_text),"a")); /* visible active candidate */
    snapshot("keyboard-pending");
    quick_tap(370,418); /* next character, no 700ms wait */
    assert(s_pwd.last_key<0 && !strcmp(lv_label_get_text(ui.pwd_text),"*"));
    quick_tap(230,208);assert(!strcmp(s_pwd.text,"aa"));
    advance_ms(800);assert(!strcmp(lv_label_get_text(ui.pwd_text),"**"));
    assert(lv_obj_has_state(ui.pwd_next,LV_STATE_DISABLED));
    tap_at(332,124);assert(!s_pwd.mask && !strcmp(lv_label_get_text(ui.pwd_text),"aa"));
    assert((icon_kind_t)(uintptr_t)lv_obj_get_user_data(ui.pwd_eye)==ICON_EYE);
    for(unsigned mode=0;mode<4;++mode) {
        s_pwd.mode=(int)mode;pwd_mode_paint();advance_ms(40);assert_type(ui.pages[MEO_PASSWORD]);
        char name[32];snprintf(name,sizeof(name),"keyboard-mode-%u",mode);snapshot(name);
    }
    /* Width, not a fixed character count, determines the visible password tail. */
    memset(s_pwd.text,'W',64);s_pwd.text[63]='!';s_pwd.text[64]=0;s_pwd.len=64;
    pwd_render();lv_point_t extent;
    lv_text_get_size(&extent,lv_label_get_text(ui.pwd_text),&meo_font_28,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    assert(extent.x<=232 && strchr(lv_label_get_text(ui.pwd_text),'!'));
    char before[65];memcpy(before,s_pwd.text,sizeof(before));
    pwd_tap_digit(2,ui.clock_ms);assert(s_pwd.len==64 && !memcmp(before,s_pwd.text,sizeof(before)));
    /* Rejecting the 65th byte must also conceal the previously active letter,
       not clear its timeout while leaving it visibly unmasked forever. */
    s_pwd.mask=true;s_pwd.mode=PWD_ABC;s_pwd.len=63;s_pwd.text[63]=0;s_pwd.last_key=-1;
    pwd_tap_digit(2,ui.clock_ms);assert(s_pwd.len==64 && s_pwd.last_key==2);
    pwd_tap_digit(3,ui.clock_ms+10);assert(s_pwd.last_key==-1 && s_pwd.len==64);
    assert(!strchr(lv_label_get_text(ui.pwd_text),'a'));
    meo_ui_password_open("Next AP");advance_ms(300);
    assert(s_pwd.mask && s_pwd.len==0);
    assert((icon_kind_t)(uintptr_t)lv_obj_get_user_data(ui.pwd_eye)==ICON_EYE_OFF);
    snapshot("keyboard-ready");
    navigate(MEO_ACCESSIBILITY);unsigned n=feedbacks;
    tap_at(360,280);assert(!ui.model.settings.interaction_sound && feedbacks==n);
    tap_at(230,70);assert(ui.model.page==MEO_SETTINGS && feedbacks==n);
    navigate(MEO_ACCESSIBILITY);tap_at(360,280);
    assert(ui.model.settings.interaction_sound && feedbacks==n+1);
    snapshot("accessibility-sound");assert_type(ui.pages[MEO_ACCESSIBILITY]);
    tap_at(230,70);assert(feedbacks==n+2);
    char gain_summary[12];snprintf(gain_summary,sizeof(gain_summary),"%u%%",(ui.model.settings.mic_gain*100+7)/14);
    assert(!strcmp(lv_label_get_text(ui.mic_summary),gain_summary));
    puts("PASS: native 3-column keypad, exact rotated taps, immediate repeated letters, candidate timeout/masking, visibility reset, 64-byte tail and interaction sound toggle");
}
static void assert_type(lv_obj_t *parent)
{
    if(lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN))return;
    if(lv_obj_check_type(parent,&lv_label_class)) {
        const lv_font_t *font=lv_obj_get_style_text_font(parent,0);
        assert(font==&meo_font_28 || font==&meo_font_44);
        assert(text_renderable(lv_label_get_text(parent),font));
        int content_height=lv_obj_get_height(parent)-lv_obj_get_style_pad_top(parent,0)-lv_obj_get_style_pad_bottom(parent,0);
        if(content_height!=font->line_height)fprintf(stderr,"Unexpected text wrapping: %s height=%d line=%d\n",lv_label_get_text(parent),content_height,font->line_height);
        assert(content_height==font->line_height);
        /* Off-viewport rows are legitimate scroll content, not clipped text.
           Labels must still fit their own cards, including when offscreen. */
        lv_area_t bounds,card;lv_obj_get_coords(parent,&bounds);
        lv_obj_get_coords(lv_obj_get_parent(parent),&card);
        assert(bounds.x1>=card.x1 && bounds.x2<=card.x2 &&
               bounds.y1>=card.y1 && bounds.y2<=card.y2);
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);++i)assert_type(lv_obj_get_child(parent,(int)i));
}

static void assert_pixel_ink(int x,int y,bool ink)
{
    const uint8_t *p=framebuffer+(y*460+x)*3;
    assert((p[0]!=0 || p[1]!=0 || p[2]!=0)==ink);
}

static void test_battery_geometry(void)
{
    assert(ui.model.page==MEO_APPS);
    assert(lv_obj_get_style_text_align(ui.power_text,0)==LV_TEXT_ALIGN_RIGHT);
    /* Test real pixels and real border-relative coordinates, not just sizes. */
    const int levels[]={0,1,5,25,50,99,100,150,0,-1};
    for(unsigned n=0;n<sizeof(levels)/sizeof(levels[0]);++n) {
        int pct=levels[n];meo_ui_set_power(pct,false);
        lv_obj_update_layout(ui.pages[MEO_APPS]);lv_refr_now(NULL);
        if(pct<0) {
            assert(lv_obj_has_flag(ui.power_gauge,LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(ui.power_terminal,LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(ui.power_text,LV_OBJ_FLAG_HIDDEN));
            for(int y=40;y<74;++y)for(int x=262;x<420;++x)assert_pixel_ink(x,y,false);
        } else {
            pct=LV_MIN(100,pct);
            assert(!lv_obj_has_flag(ui.power_gauge,LV_OBJ_FLAG_HIDDEN));
            char text[12];snprintf(text,sizeof(text),"%d%%",pct);
            assert(!strcmp(lv_label_get_text(ui.power_text),text));
            assert(lv_obj_get_height(ui.power_text)==meo_font_28.line_height);
            lv_area_t shell,fill,cap;lv_obj_get_coords(ui.power_gauge,&shell);
            lv_obj_get_coords(ui.power_fill,&fill);lv_obj_get_coords(ui.power_terminal,&cap);
            assert(fill.x1==shell.x1+POWER_BORDER && fill.y1==shell.y1+POWER_BORDER);
            assert(fill.y2==shell.y2-POWER_BORDER && fill.x2<=shell.x2-POWER_BORDER);
            assert(cap.x1==shell.x2); /* Overlap, never an isolated terminal. */
            for(int x=shell.x2-1;x<=cap.x2;++x)assert_pixel_ink(x,58,true);
            assert(lv_obj_has_flag(ui.power_fill,LV_OBJ_FLAG_HIDDEN)==(pct==0 || pct==100));
            if(pct==100) {
                for(int y=50;y<=65;++y)for(int x=360;x<=407;++x)assert_pixel_ink(x,y,true);
                for(int x=shell.x1;x<=cap.x2;++x)assert_pixel_ink(x,58,true);
            } else if(pct==0) {
                for(int y=50;y<=65;++y)for(int x=364;x<=403;++x)assert_pixel_ink(x,y,false);
            } else {
                assert(lv_obj_get_width(ui.power_fill)==LV_MAX(1,(pct*POWER_FILL_W+50)/100));
                assert_pixel_ink(fill.x1,58,true);
                if(fill.x2+1<shell.x2-POWER_BORDER)assert_pixel_ink(fill.x2+1,58,false);
            }
        }
        char name[32];snprintf(name,sizeof(name),"battery-%02u",n);snapshot(name);
    }
    meo_ui_set_power(100,true);lv_refr_now(NULL);
    snapshot("apps-battery-fixed");assert_type(ui.pages[MEO_APPS]);
    puts("PASS: connected battery cap, exact inner fill, seamless 100%, empty 0%, clamped/unknown values and right-aligned 28px percentage");
}

static void test_settings_scroll(void)
{
    assert(ui.model.page==MEO_SETTINGS);
    assert(lv_obj_get_scroll_dir(ui.settings_list)==LV_DIR_VER);
    assert(lv_obj_has_flag(ui.settings_list,LV_OBJ_FLAG_SCROLL_MOMENTUM));
    assert(!lv_obj_has_flag(ui.settings_list,LV_OBJ_FLAG_SCROLL_ELASTIC));
    assert(lv_obj_get_child_count(ui.settings_list)==6);
    assert(lv_obj_get_x(ui.wifi_summary)==188 && lv_obj_get_width(ui.wifi_summary)==160);
    assert(lv_obj_get_x(ui.brightness_summary)==188 && lv_obj_get_width(ui.brightness_summary)==160);
    assert(lv_obj_get_x(ui.motion_summary)==188 && lv_obj_get_width(ui.motion_summary)==160);
    assert(lv_obj_get_x(ui.volume_summary)==188 && lv_obj_get_width(ui.volume_summary)==160);
    assert(lv_obj_get_x(ui.mic_summary)==188 && lv_obj_get_width(ui.mic_summary)==160);
    lv_obj_t *values[]={ui.wifi_summary,ui.brightness_summary,ui.motion_summary,
                       ui.volume_summary,ui.mic_summary};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        assert(lv_obj_get_style_text_align(values[i],0)==LV_TEXT_ALIGN_RIGHT);
        lv_obj_t *row=lv_obj_get_parent(values[i]);
        lv_area_t value,chevron;
        lv_obj_get_coords(values[i],&value);
        lv_obj_get_coords(lv_obj_get_child(row,2),&chevron);
        assert(chevron.x1-value.x2-1==8);
        assert(!lv_obj_has_flag(values[i],LV_OBJ_FLAG_CLICKABLE));
    }
    lv_obj_t *header_button=lv_obj_get_child(ui.pages[MEO_SETTINGS],0);
    lv_area_t header_before,header_after,last;
    lv_obj_get_coords(header_button,&header_before);
    snapshot("settings-top");
    /* The 12px spaces between cards are touchable scroll surface too. */
    drag_at(230,298,230,200);
    assert(ui.model.page==MEO_SETTINGS && lv_obj_get_scroll_y(ui.settings_list)>0);
    drag_at(230,180,230,420);assert(lv_obj_get_scroll_y(ui.settings_list)==0);

    /* A reversal back onto a row is still a drag, never a tap. */
    point(230,260,true);advance_ms(80);
    point(230,205,true);advance_ms(100);
    point(230,260,true);advance_ms(100);
    point(230,260,false);advance_ms(700);
    assert(ui.model.page==MEO_SETTINGS);
    drag_at(230,400,230,164);
    assert(ui.model.page==MEO_SETTINGS && lv_obj_get_scroll_y(ui.settings_list)>0);
    assert(lv_obj_get_scroll_bottom(ui.settings_list)==0);
    lv_obj_get_coords(lv_obj_get_child(ui.settings_list,5),&last);
    assert(last.y1>=104 && last.y2<=443);
    lv_obj_get_coords(header_button,&header_after);
    assert(!memcmp(&header_before,&header_after,sizeof(header_before)));
    snapshot("settings-bottom");assert_type(ui.pages[MEO_SETTINGS]);
    int remembered=lv_obj_get_scroll_y(ui.settings_list);
    drag_at(230,400,230,164);
    assert(ui.model.page==MEO_SETTINGS && lv_obj_get_scroll_y(ui.settings_list)==remembered);
    /* Whitespace, not just the icon/label, dispatches the entire row. */
    lv_area_t speaker;lv_obj_get_coords(lv_obj_get_child(ui.settings_list,3),&speaker);
    tap_at(410,(speaker.y1+speaker.y2)/2);assert(ui.model.page==MEO_SPEAKER);
    tap_at(230,70);assert(ui.model.page==MEO_SETTINGS);
    assert(lv_obj_get_scroll_y(ui.settings_list)==remembered);
    drag_at(230,390,350,390);assert(ui.model.page==MEO_APPS);
    navigate(MEO_SETTINGS);assert(lv_obj_get_scroll_y(ui.settings_list)==remembered);
    drag_at(230,180,230,420);assert(lv_obj_get_scroll_y(ui.settings_list)==0);
    assert(ui.model.page==MEO_SETTINGS);

    /* Interrupt real native inertia; waiting for it to settle while held
       must not launch the row, and the next fresh tap must still work. */
    point(230,385,true);advance_ms(80);
    point(230,373,true);advance_ms(20);
    point(230,361,true);advance_ms(20);
    point(230,361,false);advance_ms(20);
    assert(ui.settings_scrolling && lv_obj_is_scrolling(ui.settings_list));
    point(230,248,true);advance_ms(700);
    assert(ui.press_blocked && ui.model.page==MEO_SETTINGS);
    point(230,248,false);advance_ms(300);
    assert(ui.model.page==MEO_SETTINGS && !ui.pointer_down);
    tap_at(230,70);assert(ui.model.page==MEO_APPS);
    navigate(MEO_SETTINGS);
    drag_at(230,180,230,420);assert(lv_obj_get_scroll_y(ui.settings_list)==0);

    navigate(MEO_ACCESSIBILITY);tap_at(360,193);assert(ui.model.settings.reduced_motion);
    assert(!lv_obj_has_flag(ui.settings_list,LV_OBJ_FLAG_SCROLL_MOMENTUM));
    assert(!lv_obj_has_flag(ui.wifi_list,LV_OBJ_FLAG_SCROLL_MOMENTUM));
    tap_at(230,70);assert(ui.model.page==MEO_SETTINGS);
    drag_at(230,420,230,140);
    assert(ui.model.page==MEO_SETTINGS && lv_obj_get_scroll_bottom(ui.settings_list)==0);
    remembered=lv_obj_get_scroll_y(ui.settings_list);
    advance_ms(600);assert(lv_obj_get_scroll_y(ui.settings_list)==remembered);
    navigate(MEO_ACCESSIBILITY);tap_at(360,193);assert(!ui.model.settings.reduced_motion);
    assert(lv_obj_has_flag(ui.settings_list,LV_OBJ_FLAG_SCROLL_MOMENTUM));
    assert(lv_obj_has_flag(ui.wifi_list,LV_OBJ_FLAG_SCROLL_MOMENTUM));
    tap_at(230,70);assert(ui.model.page==MEO_SETTINGS);
    puts("PASS: settings touch scrolling, fixed header, full-row targets, retained offset, swipe-back, reversal/throw guards and reduced motion");
}

static unsigned mouth_pixels(void)
{
    lv_refr_now(NULL);
    unsigned n=0;
    for(int y=242;y<289;++y)for(int x=192;x<269;++x) {
        const uint8_t *p=framebuffer+(y*460+x)*3;
        if(p[0]>180 && p[0]==p[1] && p[1]==p[2])++n;
    }
    return n;
}

static void test_expression_preview(void)
{
    navigate(MEO_ACCESSIBILITY);
    if(!ui.model.settings.reduced_motion)tap_at(360,193);
    navigate(MEO_SETTINGS);
    drag_at(230,420,230,140);
    lv_area_t row;lv_obj_get_coords(lv_obj_get_child(ui.settings_list,5),&row);
    assert(row.y1>=104 && row.y2<=443);
    meo_ui_set_expression(MEO_EYE_SURPRISED,0);
    meo_ui_feed_gaze((meo_gaze_t){.x=.25f,.y=-.3f,.at_ms=ui.clock_ms,.valid=true});
    unsigned before=saves,starts=mic_starts,scans=scan_calls,plays=previews;
    tap_at(410,(row.y1+row.y2)/2);assert(ui.model.page==MEO_EXPRESSIONS);
    assert(meo_page_parent(MEO_EXPRESSIONS)==MEO_SETTINGS);
    assert(!strcmp(meo_ui_page_name(MEO_EXPRESSIONS),"expressions"));
    assert(!strcmp(meo_ui_page_name(MEO_PAGE_COUNT),"invalid"));
    assert(ui.expression_preview.mouth_on && !ui.expression_preview.autoplay);
    for(unsigned e=0;e<MEO_EYE_COUNT;++e) {
        assert(ui.expression_preview.model.expression==(meo_expression_t)e);
        assert(!strcmp(lv_label_get_text(ui.expression_preview.name),expression_labels[e]));
        assert(ui.expression_preview.model.expression_override);
        assert(ui.model.expression==MEO_EYE_SURPRISED && ui.model.expression_override);
        assert(ui.model.gaze.x==.25f && ui.model.gaze.y==-.3f);
        assert(mouth_pixels()>45);
        char name[48];snprintf(name,sizeof(name),"expression-%u",e);
        snapshot(name);assert_type(ui.pages[MEO_EXPRESSIONS]);
        tap_at(392,346);
    }
    tap_at(392,346);assert(ui.expression_preview.model.expression==MEO_EYE_HAPPY);
    snapshot("expression-strength-100");
    tap_at(230,422);assert(ui.expression_preview.level==30);
    assert(ui.expression_preview.model.expression_intensity==.3f);
    snapshot("expression-strength-30");
    tap_at(230,422);assert(ui.expression_preview.level==60);
    snapshot("expression-strength-60");
    tap_at(230,422);assert(ui.expression_preview.level==100);
    tap_at(68,346);
    assert(ui.expression_preview.model.expression==MEO_EYE_CALM); /* next wraps */
    tap_at(68,346);assert(ui.expression_preview.model.expression==(MEO_EYE_COUNT-1));
    tap_at(92,422);assert(!ui.expression_preview.mouth_on && mouth_pixels()==0);
    snapshot("expression-eyes-only");
    tap_at(92,422);assert(ui.expression_preview.mouth_on && mouth_pixels()>45);
    tap_at(368,422);assert(ui.expression_preview.autoplay);
    advance_ms(FACE_STEP_MS);assert(ui.expression_preview.model.expression==MEO_EYE_CALM);
    tap_at(392,346);assert(!ui.expression_preview.autoplay);
    assert(ui.expression_preview.model.expression==MEO_EYE_HAPPY);
    lv_refr_now(NULL);static uint8_t still[sizeof(framebuffer)];memcpy(still,framebuffer,sizeof(still));
    advance_ms(3000);lv_refr_now(NULL);assert(!memcmp(still,framebuffer,sizeof(still)));
    assert(saves==before && mic_starts==starts && scan_calls==scans && previews==plays);
    /* Cancelled drags do not select a mood; committed back cannot click
       through to the settings row underneath. Offset is preserved. */
    drag_at(200,200,235,200);assert(ui.model.page==MEO_EXPRESSIONS);
    tap_at(368,422);assert(ui.expression_preview.autoplay);
    drag_at(140,190,300,190);assert(ui.model.page==MEO_SETTINGS);
    assert(!ui.expression_preview.autoplay);
    uint64_t frozen=ui.expression_preview.model.now_ms;
    advance_ms(3000);assert(ui.expression_preview.model.now_ms==frozen);
    tap_at(410,(row.y1+row.y2)/2);assert(ui.model.page==MEO_EXPRESSIONS);
    assert(!ui.expression_preview.autoplay && ui.expression_preview.mouth_on);
    tap_at(230,52);assert(ui.model.page==MEO_SETTINGS);
    /* Exercise tweened rendering too, then restore the caller's baseline. */
    navigate(MEO_ACCESSIBILITY);tap_at(360,193);assert(!ui.model.settings.reduced_motion);
    navigate(MEO_EXPRESSIONS);
    for(unsigned e=0;e<MEO_EYE_COUNT;++e) {
        tap_at(392,346);
        assert(isfinite(ui.expression_preview.model.mouth_shape.curve) && isfinite(ui.expression_preview.model.mouth_shape.aperture));
        assert_type(ui.pages[MEO_EXPRESSIONS]);
    }
    navigate(MEO_SETTINGS);meo_ui_auto_expression();
    assert(!ui.model.expression_override);
    puts("PASS: settings expression preview, 16 real-tap moods, mouth pixels/toggle, wrap, autoplay/manual hold, reduced-motion stillness, swipe-back, no IO/NVS or AI/gaze ownership changes");
}

static void test_sentence_face(void)
{
    navigate(MEO_HOME);
    meo_face_pose_t pose;assert(meo_expression_pose(MEO_EYE_HAPPY,.8f,400,true,&pose));
    pose.mouth.aperture=8;
    meo_ui_set_sentence_pose(MEO_EYE_HAPPY,.8f,&pose);advance_ms(100);
    assert(ui.model.expression_media && ui.model.expression_override);
    assert(!lv_obj_has_flag(ui.home_face.mouth[0],LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(ui.home_face.mouth[1],LV_OBJ_FLAG_HIDDEN));
    snapshot("conversation-speaking");
    meo_eye_shape_t held[2];memcpy(held,ui.model.eye_shape,sizeof(held));
    advance_ms(1000);assert(!memcmp(held,ui.model.eye_shape,sizeof(held)));
    pose.mouth.aperture=0;
    meo_ui_set_sentence_pose(MEO_EYE_HAPPY,.8f,&pose);advance_ms(30);
    assert(lv_obj_has_flag(ui.home_face.mouth[1],LV_OBJ_FLAG_HIDDEN));
    meo_ui_auto_expression();advance_ms(30);
    assert(lv_obj_has_flag(ui.home_face.mouth[0],LV_OBJ_FLAG_HIDDEN));
    navigate(MEO_ACCESSIBILITY);tap_at(360,193);assert(ui.model.settings.reduced_motion);
    navigate(MEO_HOME);meo_ui_set_sentence_pose(MEO_EYE_HAPPY,.8f,&pose);advance_ms(30);
    meo_face_pose_t still;meo_expression_pose(MEO_EYE_HAPPY,.8f,0,false,&still);
    assert(!memcmp(ui.model.eye_shape,still.eye,sizeof(still.eye)) && ui.model.mouth_shape.aperture==0);
    meo_ui_auto_expression();
    navigate(MEO_ACCESSIBILITY);tap_at(360,193);assert(!ui.model.settings.reduced_motion);
    navigate(MEO_SETTINGS);
    puts("PASS: shared home speech mouth, held media pose, silence/cancel close, reduced-motion static pose");
}

static void test_wifi_lifecycle(void)
{
    ui.config.wifi_scan=scan_cb;
    meo_ui_set_wifi(MEO_WIFI_READY,NULL,0);
    navigate(MEO_WIFI);
    lv_obj_scroll_to_y(ui.wifi_list,0,LV_ANIM_OFF);advance_ms(100);
    unsigned before=scan_calls;
    meo_ui_set_wifi(MEO_WIFI_ONLINE,"中文热点",-50);
    meo_ui_set_wifi_scan(true,0,10,0);
    assert(ui.wifi_state==MEO_WIFI_ONLINE && ui.wifi_scanning);
    assert(lv_obj_has_state(ui.wifi_scan_button,LV_STATE_DISABLED));
    tap_at(380,44);assert(scan_calls==before);
    char target[33];snprintf(target,sizeof(target),"%s",joined_ssid);
    tap_at(230,164);assert(!strcmp(joined_ssid,target) && ui.model.page==MEO_WIFI);
    meo_ui_set_wifi_scan(false,-1,11,0);
    assert(!lv_obj_has_state(ui.wifi_scan_button,LV_STATE_DISABLED));
    assert(!strcmp(lv_label_get_text(ui.wifi_notice),"扫描失败"));
    assert(lv_obj_has_flag(ui.wifi_page_ssid,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_height(ui.wifi_list)==316);
    meo_ui_set_wifi(MEO_WIFI_ONLINE,"中文热点",-50);
    assert(!strcmp(lv_label_get_text(ui.wifi_notice),"扫描失败"));
    snapshot("wifi-scan-failed");assert_type(ui.pages[MEO_WIFI]);
    advance_ms(4100);
    assert(lv_obj_has_flag(ui.wifi_notice,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(ui.wifi_page_ssid,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(ui.wifi_page_ssid),
                   text_renderable("中文热点",&meo_font_28) ? "中文热点" : "已连接网络"));
    meo_ui_set_scan_list(NULL,NULL,0);
    meo_ui_set_wifi_scan(false,0,12,0);
    assert(!strcmp(lv_label_get_text(ui.wifi_notice),"未找到网络"));
    snapshot("wifi-empty");assert_type(ui.pages[MEO_WIFI]);
    meo_ui_set_wifi_scan(false,0,12,1);
    assert(!strcmp(lv_label_get_text(ui.wifi_notice),"保存失败"));
    ui.config.wifi_scan=NULL;
    meo_ui_set_wifi(MEO_WIFI_UNAVAILABLE,NULL,0);
    meo_ui_set_wifi_scan(false,0,12,0);
    assert(lv_obj_has_state(ui.wifi_scan_button,LV_STATE_DISABLED));
    snapshot("wifi-unavailable");assert_type(ui.pages[MEO_WIFI]);
    ui.config.wifi_scan=scan_cb;
    meo_ui_set_wifi(MEO_WIFI_READY,NULL,0);meo_ui_set_wifi_scan(false,0,12,0);
    navigate(MEO_SETTINGS);
    puts("PASS: online scan independent from association, scan disabled while busy, no busy-password fallback, scan/empty/save errors, explicit offline firmware state");
}

int main(int argc,char **argv)
{
    setbuf(stdout,NULL);if(argc>1)output=argv[1];
    lv_init();lv_display_t*d=lv_display_create(460,460);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(d,drawbuffer,NULL,sizeof(drawbuffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush_cb);lv_indev_t*i=lv_indev_create();
    lv_indev_set_type(i,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(i,read_cb);lv_indev_set_display(i,d);
    meo_ui_config_t c={.settings=meo_default_settings(),.settings_changed=save_cb,
        .wifi_connect=connect_cb,.wifi_join=join_cb,.mic_test=mic_cb,.speaker_test=preview_cb,
        .interaction_sound=feedback_cb};
    meo_ui_create(&c);advance_ms(600);
    meo_ui_set_power(100,true);meo_ui_set_wifi(MEO_WIFI_ONLINE,"Review AP",-55);
    assert(lv_obj_get_child_count(ui.pages[MEO_HOME])==4);
    assert(lv_obj_has_flag(ui.home_face.mouth[0],LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(ui.home_face.mouth[1],LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_parent(ui.wifi_mark)==ui.pages[MEO_APPS]);
    assert(lv_obj_get_parent(ui.power_text)==ui.pages[MEO_APPS]);
    snapshot("home");assert_type(ui.pages[MEO_HOME]);
    navigate(MEO_APPS);snapshot("apps");assert_type(ui.pages[MEO_APPS]);
    test_battery_geometry();
    int before=lv_obj_get_scroll_x(ui.apps.rail);
    drag_at(360,350,110,350);
    assert(ui.model.page==MEO_APPS);
    assert(lv_obj_get_scroll_x(ui.apps.rail)!=before);
    assert(abs(lv_obj_get_scroll_x(ui.apps.rail)-(int)ui.apps.first*GALLERY_STEP)<=1);
    assert(ui.apps.captions_ready);
    puts("PASS: AI has no status; launcher-only one-line 100%; caption swipe snaps without launch");

    meo_ui_password_open("Review AP");advance_ms(500);
    quick_tap(230,208);quick_tap(370,208);quick_tap(404,124);quick_tap(370,208);
    assert(!strcmp(s_pwd.text,"ad"));
    tap_at(380,44);assert(connects==1 && ui.model.page==MEO_PASSWORD);
    assert(!strcmp(s_pwd.text,"ad") && !strcmp(lv_label_get_text(ui.pwd_text),"**"));
    assert(!lv_obj_has_flag(ui.pwd_notice,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_parent(ui.pwd_notice)==ui.pages[MEO_PASSWORD]);
    snapshot("password-error");assert_type(ui.pages[MEO_PASSWORD]);
    advance_ms(4100);assert(lv_obj_has_flag(ui.pwd_notice,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(ui.pwd_ssid,LV_OBJ_FLAG_HIDDEN));
    /* Every printable ASCII character must be enterable, including punctuation. */
    bool covered[127]={false};
    for(int mode=0;mode<4;++mode) {
        s_pwd.mode=mode;pwd_mode_paint();lv_obj_update_layout(ui.pages[MEO_PASSWORD]);
        for(unsigned k=0;k<PWD_KEY_COUNT;++k)
            if(ui.pwd_key_labels[k]) assert(lv_obj_get_height(ui.pwd_key_labels[k])==meo_font_28.line_height);
        for(int digit=0;digit<10;++digit) {
            unsigned length=mode==PWD_DIGIT ? 1 : (unsigned)strlen(mode==PWD_SYM ? pwd_symbols[digit] : pwd_t9[digit]);
            s_pwd.len=0;s_pwd.text[0]=0;s_pwd.last_key=-1;
            for(unsigned n=0;n<length;++n) {
                pwd_tap_digit(digit,ui.clock_ms+n*50);assert(s_pwd.len==1);
                covered[(unsigned char)s_pwd.text[0]]=true;
            }
        }
    }
    for(int ascii=32;ascii<=126;++ascii)assert(covered[ascii]);
    meo_ui_password_open("Review AP");advance_ms(300);
    accept_connection=true;tap_at(380,44); /* open network: empty key allowed */
    assert(ui.model.page==MEO_WIFI && connected_pass[0]==0 && s_pwd.len==0);
    /* T9 is time-aware: a second tap inside the window cycles the current
       character, while a later tap starts the next character. Delete clears
       the cycle so the next press is deterministic. */
    s_pwd.mode=PWD_ABC;s_pwd.len=0;s_pwd.text[0]=0;s_pwd.last_key=-1;
    pwd_tap_digit(2,100);pwd_tap_digit(2,200);assert(!strcmp(s_pwd.text,"b"));
    pwd_tap_digit(2,1000);assert(!strcmp(s_pwd.text,"ba"));
    pwd_backspace();assert(!strcmp(s_pwd.text,"b"));
    puts("PASS: deletion commits T9, rejected password retained with local error; all 95 ASCII characters and open networks");
    test_keyboard_and_feedback();
    ui.config.wifi_scan=scan_cb;
    meo_ui_set_wifi(MEO_WIFI_READY,NULL,0); /* explicit join finished/failed */
    navigate(MEO_WIFI);

    const char names[6][33]={"AP-one","AP-two","AP-three","AP-four","AP-five","AP-six"};
    const int rssi[6]={-40,-50,-60,-65,-70,-80};
    meo_ui_set_scan_list(names,rssi,6);advance_ms(100);assert(ui.wifi_ap_visible==6);
    assert(lv_obj_get_height(ui.wifi_list)==316 && lv_obj_get_width(ui.wifi_list)==412);
    assert(!lv_obj_has_flag(ui.wifi_list,LV_OBJ_FLAG_SCROLL_ELASTIC));
    assert(!lv_obj_has_flag(ui.wifi_list,LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_area_t viewport,header_before,header_after,scan_bounds;
    lv_obj_get_coords(ui.wifi_list,&viewport);
    lv_obj_get_coords(lv_obj_get_child(ui.pages[MEO_WIFI],0),&header_before);
    lv_obj_get_coords(ui.wifi_scan_button,&scan_bounds);
    assert(viewport.y1==128 && viewport.y2==443);
    assert(header_before.x2<scan_bounds.x1 && scan_bounds.y2<viewport.y1);
    assert(lv_obj_get_height(ui.wifi_scan_button)==64 && lv_obj_get_width(ui.wifi_scan_button)==112);
    assert(lv_obj_get_child_count(ui.wifi_scan_button)==1);
    assert(lv_obj_get_child(ui.wifi_scan_button,0)==ui.wifi_scan_label);
    assert(lv_obj_get_style_bg_opa(ui.wifi_scan_button,0)==LV_OPA_TRANSP);
    lv_area_t scan_text,title_text;
    lv_obj_get_coords(ui.wifi_scan_label,&scan_text);
    lv_obj_get_coords(lv_obj_get_child(lv_obj_get_child(ui.pages[MEO_WIFI],0),1),&title_text);
    assert(scan_text.y1==title_text.y1); /* same native font and optical row */
    /* Transparent padding, not just the two glyphs, must still receive taps. */
    meo_ui_set_wifi_scan(false,0,13,0);advance_ms(60);
    unsigned scan_before=scan_calls;
    snapshot("wifi-scan-idle");
    point(scan_bounds.x1+8,44,true);advance_ms(80);
    assert(lv_obj_has_state(ui.wifi_scan_button,LV_STATE_PRESSED));
    assert(lv_obj_get_style_bg_opa(ui.wifi_scan_button,0)==LV_OPA_COVER);
    snapshot("wifi-scan-pressed");
    point(scan_bounds.x1+8,44,false);advance_ms(120);
    if (scan_calls!=scan_before+1 || !ui.wifi_scanning)
        fprintf(stderr,"scan padding: calls=%u before=%u scanning=%d state=%d moved=%d blocked=%d navigating=%d scroll=%d\n",
                scan_calls,scan_before,ui.wifi_scanning,ui.wifi_state,ui.tap_moved,
                ui.press_blocked,ui.navigating,lv_obj_is_scrolling(ui.wifi_list));
    assert(scan_calls==scan_before+1 && ui.wifi_scanning);
    assert(lv_obj_has_state(ui.wifi_scan_button,LV_STATE_DISABLED));
    assert(lv_obj_get_style_bg_opa(ui.wifi_scan_button,0)==LV_OPA_TRANSP);
    assert(lv_color_eq(lv_obj_get_style_text_color(ui.wifi_scan_label,0),color(DIM)));
    tap_at(380,44);assert(scan_calls==scan_before+1);
    for(unsigned n=0;n<4;++n) {
        lv_area_t bounds;lv_obj_get_coords(ui.wifi_rows[n],&bounds);
        assert(bounds.y1>=viewport.y1 && bounds.y2<=viewport.y2);
        assert(lv_obj_get_height(ui.wifi_rows[n])>=72);
        char signal[32];snprintf(signal,sizeof(signal),"%d dBm",rssi[n]);
        assert(!strcmp(lv_label_get_text(ui.wifi_row_rssi[n]),signal));
    }
    /* The action keeps the stable verb "扫描"; only the shared status rail
       may say "扫描中". This guards against two competing progress labels
       after the radio/UI poll races. */
    meo_ui_set_wifi_scan(true,0,13,0);advance_ms(60);
    assert(!strcmp(lv_label_get_text(ui.wifi_scan_label),"扫描"));
    assert(!strcmp(lv_label_get_text(ui.wifi_notice),"扫描中"));
    assert(lv_obj_has_flag(ui.wifi_page_ssid,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(ui.wifi_page_state,LV_OBJ_FLAG_HIDDEN));
    assert(visible_text_count(ui.pages[MEO_WIFI],"扫描中")==1);
    snapshot("wifi-scanning");assert_type(ui.pages[MEO_WIFI]);
    meo_ui_set_wifi(MEO_WIFI_READY,NULL,0);
    advance_ms(21000); /* progress cannot expire while the radio is still busy */
    assert(visible_text_count(ui.pages[MEO_WIFI],"扫描中")==1);
    meo_ui_set_wifi_scan(false,0,14,0);
    assert(strcmp(lv_label_get_text(ui.wifi_scan_label),"扫描中")!=0);
    snapshot("wifi");
    meo_ui_set_wifi(MEO_WIFI_ONLINE,"Home AP",-54);advance_ms(40);
    assert(!strcmp(lv_label_get_text(ui.wifi_page_state),"已接入"));
    assert(!strcmp(lv_label_get_text(ui.wifi_page_ssid),"Home AP"));
    lv_area_t left,right,first_left,first_right;
    lv_obj_get_coords(ui.wifi_page_ssid,&left);lv_obj_get_coords(ui.wifi_page_state,&right);
    lv_obj_get_coords(ui.wifi_row_ssid[0],&first_left);lv_obj_get_coords(ui.wifi_row_rssi[0],&first_right);
    assert(left.x1==first_left.x1 && right.x2==first_right.x2);
    assert(left.y1==right.y1 && first_left.y1==first_right.y1);
    snapshot("wifi-online");assert_type(ui.pages[MEO_WIFI]);
    meo_ui_set_wifi_scan(true,0,14,0);meo_ui_set_wifi(MEO_WIFI_ONLINE,"Home AP",-54);
    assert(visible_text_count(ui.pages[MEO_WIFI],"扫描中")==1);
    assert(visible_text_count(ui.pages[MEO_WIFI],"已接入")==0);
    snapshot("wifi-online-scanning");
    meo_ui_set_wifi_scan(false,0,15,0);
    /* Dragging in the gap and the wide row padding both scroll, never join. */
    drag_at(230,204,230,150);
    assert(ui.model.page==MEO_WIFI && lv_obj_get_scroll_y(ui.wifi_list)>0);
    drag_at(420,420,420,160);
    assert(ui.model.page==MEO_WIFI);
    assert(lv_obj_get_scroll_bottom(ui.wifi_list)==0);
    lv_obj_get_coords(lv_obj_get_child(ui.pages[MEO_WIFI],0),&header_after);
    assert(!memcmp(&header_before,&header_after,sizeof(header_before)));
    snapshot("wifi-bottom");
    lv_area_t row;lv_obj_get_coords(ui.wifi_rows[5],&row);
    assert(row.y1>=128 && row.y2<444);
    tap_at(200,(row.y1+row.y2)/2);assert(!strcmp(joined_ssid,"AP-six"));
    assert(ui.model.page==MEO_PASSWORD);
    navigate(MEO_WIFI);
    drag_at(230,160,230,420);assert(lv_obj_get_scroll_y(ui.wifi_list)==0);
    point(230,300,true);advance_ms(80);
    point(230,288,true);advance_ms(20);point(230,276,true);advance_ms(20);
    point(230,276,false);advance_ms(20);
    assert(ui.wifi_scrolling && lv_obj_is_scrolling(ui.wifi_list));
    point(230,248,true);advance_ms(700);
    assert(ui.press_blocked && ui.model.page==MEO_WIFI);
    point(230,248,false);advance_ms(300);
    assert(ui.model.page==MEO_WIFI && !ui.pointer_down);
    tap_at(160,44);assert(ui.model.page==MEO_SETTINGS);
    meo_ui_settings_saved(false);assert(!lv_obj_has_flag(ui.save_notice,LV_OBJ_FLAG_HIDDEN));
    assert_type(lv_layer_top());advance_ms(5100);assert(lv_obj_has_flag(ui.save_notice,LV_OBJ_FLAG_HIDDEN));
    snapshot("settings");assert_type(ui.pages[MEO_SETTINGS]);
    puts("PASS: four full Wi-Fi rows, fixed compact actions, gap/padding scroll, no throw-stop join, sixth SSID dispatch, timed save failure");
    test_wifi_lifecycle();
    test_settings_scroll();
    test_expression_preview();
    test_sentence_face();

    lv_area_t mic_row;lv_obj_get_coords(lv_obj_get_child(ui.settings_list,4),&mic_row);
    tap_at(410,(mic_row.y1+mic_row.y2)/2);
    assert(ui.model.page==MEO_MIC && mic_starts==1 && mic_stops==0);
    for(int n=0;n<20;++n)tap_at(385,382);
    assert(ui.model.settings.mic_gain==14 && lv_slider_get_value(ui.mic_gain_slider)==14);
    tap_at(75,382);assert(ui.model.settings.mic_gain==13);
    float level[2]={0.65f,0.32f};meo_ui_set_mic(level,true);advance_ms(300);
    snapshot("microphone");assert_type(ui.pages[MEO_MIC]);
    assert(ui.mic_shown[0]>.64f && ui.mic_shown[1]>.31f);
    drag_at(40,220,250,220);assert(ui.model.page==MEO_SETTINGS && mic_stops==1);
    assert(lv_obj_get_scroll_bottom(ui.settings_list)==0);
    navigate(MEO_MIC);assert(mic_starts==2);
    tap_at(200,70);assert(ui.model.page==MEO_SETTINGS && mic_stops==2);
    assert(saves>0);
    puts("PASS: microphone capture scoped to page including swipe-back; real gain slider and +/- bounds; independent stereo meters");

    /* The AI dial also owns the live microphone indicator.  Its face update
       must not starve the independent waveform/VU tick. */
    navigate(MEO_HOME); meo_ui_set_mic((float[2]){0.02f,0.02f}, true);
    advance_ms(120);
    int quiet_height = ui.home_wave_height[3];
    meo_ui_set_mic((float[2]){0.95f,0.75f}, true);
    advance_ms(120);
    assert(ui.home_capture_active && !lv_obj_has_flag(ui.home_capture_ring, LV_OBJ_FLAG_HIDDEN));
    assert(ui.home_wave_height[3] > quiet_height);
    meo_ui_capture_view_t capture;
    meo_ui_capture_view(&capture);
    assert(capture.visible && !capture.ack && capture.wave_seq>0);
    uint32_t old_wave_seq=capture.wave_seq;
    snapshot("wake-listening");
    meo_ui_set_wake_state(true);advance_ms(120);meo_ui_capture_view(&capture);
    assert(capture.visible && capture.ack && capture.ink==ACCENT_AMBER);
    assert(capture.wave_seq>old_wave_seq);
    snapshot("wake-ack");
    meo_ui_set_wake_state(false);
    meo_ui_set_mic((float[2]){0.0f,0.0f}, false); advance_ms(120);
    assert(lv_obj_has_flag(ui.home_capture_ring, LV_OBJ_FLAG_HIDDEN));
    puts("PASS: AI home microphone indicator remains visible and waveform animates independently of eyes");

    navigate(MEO_SPEAKER);
    tap_at(386,244);assert(ui.model.settings.volume==60);
    tap_at(230,364);assert(previews==1 && played_volume==60);
    tap_at(230,364);assert(previews==1);
    meo_audio_view_t audio={0};meo_ui_set_audio(&audio);
    for(int n=0;n<6;++n)tap_at(75,244);
    assert(ui.model.settings.volume==0);
    tap_at(230,364);assert(previews==2 && played_volume==0);
    audio.playback_error=-1;meo_ui_set_audio(&audio);
    assert(!strcmp(lv_label_get_text(ui.audio_result),"播放失败"));
    audio.playback_error=0;meo_ui_set_audio(&audio);tap_at(386,244);tap_at(386,244);tap_at(386,244);
    snapshot("speaker");assert_type(ui.pages[MEO_SPEAKER]);
    puts("PASS: speaker preview uses user volume including mute; busy prevents double playback; error feedback; 28/44px type");

    navigate(MEO_ACCESSIBILITY);tap_at(360,193);assert(ui.model.settings.reduced_motion);
    navigate(MEO_WIFI);wifi_notice_for("扫描中",5000);advance_ms(100);
    int opacity=lv_obj_get_style_text_opa(ui.wifi_notice,0);advance_ms(500);
    assert(opacity==255 && lv_obj_get_style_text_opa(ui.wifi_notice,0)==255);
    meo_ui_set_shutdown(3);advance_ms(50);lv_refr_now(NULL);
    static uint8_t static_frame[sizeof(framebuffer)];memcpy(static_frame,framebuffer,sizeof(framebuffer));
    advance_ms(500);lv_refr_now(NULL);assert(!memcmp(static_frame,framebuffer,sizeof(framebuffer)));
    meo_ui_set_shutdown(-1);assert(lv_screen_active()==ui.pages[MEO_WIFI]);
    navigate(MEO_ACCESSIBILITY);tap_at(360,193);assert(!ui.model.settings.reduced_motion);
    navigate(MEO_HOME);meo_ui_set_shutdown(3);
    int first=ui.shutdown_progress;advance_ms(100);assert(ui.shutdown_progress<first);
    meo_ui_set_shutdown(3);assert(ui.shutdown_progress<first); /* no restart */
    snapshot("shutdown");assert_type(ui.shutdown_layer);
    for(int frame=0;frame<58;++frame) {
        char name[40];snprintf(name,sizeof(name),"shutdown-%02d",frame);snapshot(name);
        advance_ms(50);
        if(frame==17)meo_ui_set_shutdown(2);
        if(frame==37)meo_ui_set_shutdown(1);
    }
    assert(ui.shutdown_progress==0);meo_ui_set_shutdown(0);meo_ui_set_shutdown(-1);
    assert(lv_screen_active()==ui.pages[MEO_HOME]);
    navigate(MEO_SETTINGS);
    point(40,200,true);advance_ms(80);point(150,200,true);advance_ms(100);
    assert(ui.back_dragging);
    meo_ui_set_shutdown(3);point(150,200,false);advance_ms(300);
    assert(lv_screen_active()==ui.shutdown_layer && !ui.back_dragging && !ui.back_settling);
    meo_ui_set_shutdown(-1);advance_ms(250);
    assert(ui.model.page==MEO_SETTINGS && lv_obj_get_x(ui.pages[MEO_SETTINGS])==0);
    meo_ui_navigate(MEO_WIFI);advance_ms(40);meo_ui_set_shutdown(3);advance_ms(300);
    assert(lv_screen_active()==ui.shutdown_layer);
    meo_ui_set_shutdown(-1);advance_ms(250);
    assert(lv_screen_active()==ui.pages[MEO_WIFI] && !ui.navigating);
    puts("PASS: shutdown safely interrupts page transitions and unfinished back gestures");
    puts("PASS: reduced motion freezes notice/countdown pixels; smooth monotonic 3-second ring, cancellation restores original page");
    navigate(MEO_BREATHE);snapshot("breathe");
    puts("ALL INTERACTION REGRESSIONS PASSED (desktop LVGL, not physical acceptance)");
    return 0;
}
