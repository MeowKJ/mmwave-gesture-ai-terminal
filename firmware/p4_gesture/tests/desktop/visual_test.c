/* Uses the exact firmware UI and LVGL, not a separate HTML approximation.
 * Outputs logical orientation PPMs. ESP port pixel rotation is a separate HW
 * check; real LVGL pointer rotation is exercised here by raw 459-x/y taps. */
#include "meoave_ui.h"
#include "src/misc/lv_text_private.h" /* use this pinned LVGL's real UTF-8 decoder */
LV_FONT_DECLARE(meo_font_28);
LV_FONT_DECLARE(meo_font_44);
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned char framebuffer[460 * 460 * 3], drawbuffer[460 * 460 * 3];
static lv_indev_data_t pointer_data;
static unsigned saves;
/* A modal overlay dims the page; colour counting is meaningless there. */
static bool color_check_disabled;
static bool save_pending;
static uint32_t save_at;
static unsigned flushes;
static unsigned wifi_scan_calls, wifi_join_calls, wifi_connect_calls;
static char wifi_connect_ssid[33], wifi_connect_pass[65];
static char wifi_join_ssid[33];
static bool wifi_join_result = true;

/* The network page must actually dispatch taps: recording the callbacks is the
 * only way to prove the wiring without a human finger. */
static bool wifi_scan_cb(void) { wifi_scan_calls++; return true; }
static bool wifi_connect_cb(const char *ssid, const char *pass)
{
    wifi_connect_calls++;
    snprintf(wifi_connect_ssid, sizeof(wifi_connect_ssid), "%s", ssid ? ssid : "");
    snprintf(wifi_connect_pass, sizeof(wifi_connect_pass), "%s", pass ? pass : "");
    return true;
}

static bool wifi_join_cb(const char *ssid)
{
    wifi_join_calls++;
    snprintf(wifi_join_ssid, sizeof(wifi_join_ssid), "%s", ssid ? ssid : "");
    return wifi_join_result;
}

static void saved(const meo_settings_t *settings)
{ assert(settings->brightness >= 15); saves++; save_at=lv_tick_get()+600; save_pending=true; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    flushes++;
    int width = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(framebuffer + (y * 460 + area->x1) * 3,
               pixels + (y - area->y1) * width * 3, (size_t)width * 3);
    }
    lv_display_flush_ready(display);
}
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{ (void)indev; *data = pointer_data; }
static void advance(unsigned ms)
{
    for (unsigned n = 0; n < ms;) {
        unsigned step = ms-n < 5 ? ms-n : 5;
        n += step;
        lv_tick_inc(step); lv_timer_handler();
        if (save_pending && lv_tick_get()>=save_at) { meo_ui_settings_saved(true); save_pending=false; }
    }
}
static void tap(int x, int y)
{
    pointer_data.point.x = 459-x;
    pointer_data.point.y = 459-y;
    pointer_data.state = LV_INDEV_STATE_PRESSED;
    advance(80);
    pointer_data.state = LV_INDEV_STATE_RELEASED;
    advance(350);
}

static void swipe(int x1,int y1,int x2,int y2)
{
    pointer_data.point.x=459-x1; pointer_data.point.y=459-y1;
    pointer_data.state=LV_INDEV_STATE_PRESSED; advance(80);
    for (int step=1;step<=10;step++) {
        pointer_data.point.x=459-(x1+(x2-x1)*step/10);
        pointer_data.point.y=459-(y1+(y2-y1)*step/10);
        advance(40);
    }
    pointer_data.state=LV_INDEV_STATE_RELEASED; advance(350);
}

static void slow_drag(bool return_to_origin)
{
    pointer_data.point.x=459-150; pointer_data.point.y=459-210;
    pointer_data.state=LV_INDEV_STATE_PRESSED; advance(80);
    for (int x=152;x<=250;x+=2) { pointer_data.point.x=459-x; advance(40); }
    if (return_to_origin)
        for (int x=248;x>=150;x-=2) { pointer_data.point.x=459-x; advance(40); }
    pointer_data.state=LV_INDEV_STATE_RELEASED; advance(350);
}
static void snapshot_pixels(const char *directory, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n460 460\n255\n");
    for (unsigned i = 0; i < sizeof(framebuffer); i += 3) {
        fputc(framebuffer[i+2], file); fputc(framebuffer[i+1], file); fputc(framebuffer[i], file);
    }
    fclose(file);
}

/* Check the actual font on every visible label, including dynamic text. */
static void assert_visible_glyphs(lv_obj_t *parent)
{
    if (lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(parent,&lv_label_class)) {
        const lv_font_t *font=lv_obj_get_style_text_font(parent,0);
        const char *text=lv_label_get_text(parent);
        /* Every product page, not just settings, must use native 28px+ type. */
        assert(font==&meo_font_28 || font==&meo_font_44);
        uint32_t at=0, letter;
        while ((letter=lv_text_encoded_next(text,&at))!=0) {
            lv_font_glyph_dsc_t glyph;
            bool present=lv_font_get_glyph_dsc(font,&glyph,letter,0);
            if (!present || glyph.is_placeholder)
                fprintf(stderr,"Missing visible glyph U+%04X in %s\n",(unsigned)letter,text);
            assert(present && !glyph.is_placeholder);
            assert(letter<0xF000 || letter>0xF8FF); /* icons are not text */
        }
    }
    for (uint32_t i=0;i<lv_obj_get_child_count(parent);++i)
        assert_visible_glyphs(lv_obj_get_child(parent,(int32_t)i));
}

/* A glyph descriptor alone cannot prove rendering. Compare the real RGB888
   frame with/without this icon, without advancing time or changing navigation. */
static unsigned assert_art_pixels(lv_obj_t *art)
{
    static unsigned char shown[sizeof(framebuffer)];
    assert(!lv_obj_has_flag(art,LV_OBJ_FLAG_HIDDEN));
    lv_refr_now(NULL);
    memcpy(shown,framebuffer,sizeof(shown));
    lv_obj_add_flag(art,LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    unsigned changed=0;
    for (unsigned i=0;i<sizeof(framebuffer);i+=3)
        if (memcmp(shown+i,framebuffer+i,3)) changed++;
    lv_obj_remove_flag(art,LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    assert(changed>=24); /* no invisible glyphs, zero opacity or total clipping */
    if (memcmp(shown,framebuffer,sizeof(shown))) {
        fprintf(stderr,"Icon restore mismatch page=%s type=%s changed=%u\n",
                meo_ui_page_name(meo_ui_model()->page),
                lv_obj_check_type(art,&lv_label_class) ? lv_label_get_text(art) : "native", changed);
        unsigned mismatched=0;
        for (unsigned i=0;i<sizeof(framebuffer);i+=3) if (memcmp(shown+i,framebuffer+i,3)) {
            if (mismatched++<6) fprintf(stderr,"at %u,%u before=%02x%02x%02x after=%02x%02x%02x\n",
                (i/3)%460,(i/3)/460,shown[i],shown[i+1],shown[i+2],framebuffer[i],framebuffer[i+1],framebuffer[i+2]);
        }
        fprintf(stderr,"mismatched pixels=%u\n",mismatched);
        assert(!"icon pixels must restore");
    }
    return changed;
}

static unsigned assert_native_icon_pixels(lv_obj_t *parent)
{
    unsigned count=0;
    if (lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN)) return 0;
    lv_area_t viewport; lv_obj_get_coords(parent,&viewport);
    if (viewport.x1>459 || viewport.x2<0 || viewport.y1>459 || viewport.y2<0) return 0;
    if (lv_obj_has_flag(parent,LV_OBJ_FLAG_USER_1)) {
        assert(!lv_obj_has_flag(parent,LV_OBJ_FLAG_CLICKABLE));
        assert(lv_obj_get_style_transform_scale_x(parent,0)==256);
        assert(lv_obj_get_style_transform_scale_y(parent,0)==256);
        lv_area_t area,bounds;
        lv_obj_get_coords(parent,&area);
        lv_obj_get_coords(lv_obj_get_parent(parent),&bounds);
        /* Only the two complete App icons are actionable artwork. A clipped
           edge hint may legitimately contain just its card contour. */
        if (meo_ui_model()->page==MEO_APPS && (bounds.x1<0 || bounds.x2>459)) return 0;
        assert(area.x1>=bounds.x1 && area.x2<=bounds.x2);
        assert(area.y1>=bounds.y1 && area.y2<=bounds.y2);
        assert_art_pixels(parent);
        count++;
    }
    for (uint32_t i=0;i<lv_obj_get_child_count(parent);++i)
        count+=assert_native_icon_pixels(lv_obj_get_child(parent,(int32_t)i));
    return count;
}

static void assert_icons(unsigned expected)
{
    lv_obj_t *screen=lv_screen_active();
    lv_obj_update_layout(screen);
    assert_visible_glyphs(screen);
    unsigned actual=assert_native_icon_pixels(screen);
    if (actual!=expected) fprintf(stderr,"Icon count: page=%s expected=%u actual=%u\n",
        meo_ui_page_name(meo_ui_model()->page),expected,actual);
    assert(actual==expected);
    if (meo_ui_model()->page==MEO_WIFI) {
        /* The Wi-Fi mark must include its bottom dot, not just the arcs: a
           zero-length rounded line is discarded by the SW renderer. Probe a
           small window because the dot is antialiased at this size. */
        bool found=false;
        for (int y=164; y<=178 && !found; ++y)
            for (int x=54; x<=74 && !found; ++x) {
                const unsigned char *p=framebuffer+(y*460+x)*3;
                if (p[0]==0xFF && p[1]==0xB5 && p[2]==0x78) found=true; /* BGR accent blue */
            }
        assert(found);
    }
}

static void snapshot(const char *directory, const char *name)
{
    lv_refr_now(NULL);
    assert_visible_glyphs(lv_screen_active());
    /* Black canvas does not mean a grayscale product. Check actual rendered
       accents, while the home dial must remain strictly monochrome. */
    lv_color_t background=lv_obj_get_style_bg_color(lv_screen_active(),0);
    assert(background.red==0 && background.green==0 && background.blue==0);
    const unsigned char *margin=framebuffer+(10*460+230)*3;
    assert(margin[0]==0 && margin[1]==0 && margin[2]==0);
    unsigned colored=0;
    for (unsigned i=0; i<sizeof(framebuffer); i+=3) {
        int lo=framebuffer[i], hi=lo;
        for (unsigned j=1; j<3; ++j) {
            if (framebuffer[i+j]<lo) lo=framebuffer[i+j];
            if (framebuffer[i+j]>hi) hi=framebuffer[i+j];
        }
        if (hi-lo>16) colored++;
        if (meo_ui_model()->page==MEO_HOME)
            assert(framebuffer[i]==framebuffer[i+1] && framebuffer[i+1]==framebuffer[i+2]);
    }
    if (color_check_disabled) { /* modal overlay: dimmed pixels are not judged */ }
    else if (meo_ui_model()->page==MEO_HOME) assert(colored==0);
    else if (meo_ui_model()->page!=MEO_ACCESSIBILITY || meo_ui_model()->settings.reduced_motion)
        assert(colored>200);
    snapshot_pixels(directory,name);
}

static unsigned assert_settings_labels(lv_obj_t *parent)
{
    unsigned count=0;
    lv_area_t bounds; lv_obj_get_coords(parent,&bounds);
    for (uint32_t i=0;i<lv_obj_get_child_count(parent);++i) {
        lv_obj_t *child=lv_obj_get_child(parent,(int32_t)i);
        if (lv_obj_has_flag(child,LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(child,&lv_label_class)) {
            const lv_font_t *font=lv_obj_get_style_text_font(child,0);
            const char *text=lv_label_get_text(child);
            uint32_t at=0, letter;
            while ((letter=lv_text_encoded_next(text,&at))!=0) {
                lv_font_glyph_dsc_t glyph;
                bool present=lv_font_get_glyph_dsc(font,&glyph,letter,0);
                if (!present || glyph.is_placeholder)
                    fprintf(stderr,"Missing settings glyph U+%04X in %s\n",(unsigned)letter,text);
                assert(present && !glyph.is_placeholder);
            }
            /* Icons are not labels. All settings copy is native 28px+. */
            if (font!=&meo_font_28 && font!=&meo_font_44)
                fprintf(stderr,"Small settings text: %s\n",lv_label_get_text(child));
            assert(font==&meo_font_28 || font==&meo_font_44);
            count++;
            assert(lv_obj_get_height(child)==font->line_height); /* no wrapping */
            lv_area_t text_area; lv_obj_get_coords(child,&text_area);
            assert(text_area.x1>=bounds.x1 && text_area.x2<=bounds.x2);
            assert(text_area.y1>=bounds.y1 && text_area.y2<=bounds.y2);
        }
        count+=assert_settings_labels(child);
    }
    return count;
}

static void assert_settings_typography(void)
{
    meo_page_t page=meo_ui_model()->page;
    assert(page==MEO_SETTINGS || page==MEO_DISPLAY || page==MEO_ACCESSIBILITY || page==MEO_WIFI);
    lv_obj_update_layout(lv_screen_active());
    assert(assert_settings_labels(lv_screen_active())>=2);
}

static lv_obj_t *breath_rate(void)
{
    assert(meo_ui_model()->page==MEO_BREATHE);
    return lv_obj_get_child(lv_screen_active(),1);
}

static void assert_minimal_breathe(void)
{
    assert(meo_ui_model()->page==MEO_BREATHE);
    lv_obj_t *screen=lv_screen_active();
    lv_obj_update_layout(screen);
    assert(lv_obj_get_child_count(screen)==2); /* circles and optional rate, no header */
    lv_obj_t *bloom=lv_obj_get_child(screen,0);
    lv_obj_t *rate=breath_rate();
    assert(lv_obj_has_flag(bloom,LV_OBJ_FLAG_USER_2));
    assert(!lv_obj_has_flag(bloom,LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_get_width(bloom)==320 && lv_obj_get_height(bloom)==320);
    assert(lv_obj_get_x(bloom)==70 && lv_obj_get_y(bloom)==70);
    assert(lv_obj_get_style_text_font(rate,0)==&meo_font_44);
    assert(lv_obj_has_flag(rate,LV_OBJ_FLAG_HIDDEN)==!meo_ui_model()->rate_live);
    assert_art_pixels(bloom);
}

static void assert_monochrome_home(bool open)
{
    assert(meo_ui_model()->page == MEO_HOME);
    lv_refr_now(NULL);
    /* Home starts with the two eye objects (pupils are their children); the
       status strip (Wi-Fi, percentage, gauge) follows them. */
    assert(lv_obj_get_child_count(lv_screen_active()) >= 2);
    unsigned whites[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *eye = lv_obj_get_child(lv_screen_active(), i);
        assert(lv_obj_check_type(eye, &lv_line_class));
        assert(lv_obj_get_child_count(eye) == 1);
        assert(!lv_obj_has_flag(eye, LV_OBJ_FLAG_CLICKABLE));
    }
    for (int y = 0; y < 460; ++y) for (int x = 0; x < 460; ++x) {
        const unsigned char *p = framebuffer + (y * 460 + x) * 3;
        assert(p[0] == p[1] && p[1] == p[2]);
        /* The face area stays pure black apart from the eyes. The status band
           at the top is the single place where UI ink may appear. */
        bool eye_region = x >= 65 && x <= 395 && y >= 125 && y <= 325;
        bool status_band = y <= 120 && x >= 200;
        if (!eye_region && !status_band) assert(p[0] == 0);
        if (p[0] == 255 && y < 275) whites[x >= 230]++;
    }
    assert(whites[0] > (open ? 450u : 100u) && whites[1] > (open ? 450u : 100u));
}

static uint32_t home_hash(void)
{
    uint32_t hash = 2166136261u;
    for (unsigned i=0; i<sizeof(framebuffer); ++i) hash = (hash ^ framebuffer[i])*16777619u;
    return hash;
}

static lv_obj_t *gallery_rail(void)
{ assert(meo_ui_model()->page==MEO_APPS); return lv_obj_get_child(lv_screen_active(),0); }

static lv_obj_t *gallery_caption(unsigned slot)
{ assert(slot<2); return lv_obj_get_child(lv_screen_active(),1+(int)slot); }

static unsigned gallery_first(void)
{ return (unsigned)((lv_obj_get_scroll_x(gallery_rail())+95)/190); }

static unsigned launcher_index(void)
{ return gallery_first()%3; }

static void assert_launcher(void)
{
    assert(meo_ui_model()->page==MEO_APPS);
    lv_obj_t *screen=lv_screen_active(), *rail=gallery_rail();
    lv_obj_update_layout(screen);
    assert(lv_obj_get_child_count(screen)==6); /* rail, 2 captions, 3 indicators; no header */
    assert(lv_obj_get_scroll_dir(rail)==LV_DIR_HOR);
    assert(lv_obj_get_scroll_snap_x(rail)==LV_SCROLL_SNAP_START);
    assert(lv_obj_get_child_count(rail)==9); /* 3 real apps, cloned for seamless wrap */
    unsigned first=gallery_first(); assert(first>=2 && first<=5);
    assert(abs(lv_obj_get_scroll_x(rail)-(int)first*190)<=1);
    unsigned complete=0, partial=0;
    for (int i=0; i<9; ++i) {
        lv_obj_t *tile=lv_obj_get_child(rail,i);
        assert(lv_obj_has_flag(tile,LV_OBJ_FLAG_CLICKABLE));
        assert(lv_obj_get_width(tile)==158 && lv_obj_get_height(tile)==158);
        assert(lv_obj_get_style_radius(tile,0)==56);
        lv_area_t area; lv_obj_get_coords(tile,&area);
        if (area.x1>=0 && area.x2<460) {
            assert(area.x1==56+(int)complete*190);
            /* Semantic colors survive scrolling, clone rebasing and reduced
               motion; the old scroll callback used to force gray styles. */
            static const uint32_t surfaces[]={0x10151E,0x181613,0x101A1C};
            static const uint32_t accents[]={0x78B5FF,0xFFF1DB,0x78E4DE};
            lv_color_t bg=lv_obj_get_style_bg_color(tile,0);
            lv_color_t ink=lv_obj_get_style_line_color(lv_obj_get_child(tile,0),0);
            uint32_t expected_bg=surfaces[i%3], expected_ink=accents[i%3];
            assert(bg.red==(expected_bg>>16) && bg.green==((expected_bg>>8)&255) && bg.blue==(expected_bg&255));
            assert(ink.red==(expected_ink>>16) && ink.green==((expected_ink>>8)&255) && ink.blue==(expected_ink&255));
            assert_art_pixels(lv_obj_get_child(tile,0)); complete++;
        } else if (area.x1<460 && area.x2>=0) {
            int visible=LV_MIN(459,area.x2)-LV_MAX(0,area.x1)+1;
            assert(visible==24); partial++;
        }
        assert(lv_obj_get_child_count(tile)==1);
    }
    assert(complete==2 && partial==2);
    static const char *names[]={"设置","AI","呼吸"};
    for (unsigned i=0; i<2; ++i) {
        lv_obj_t *caption=gallery_caption(i), *text=lv_obj_get_child(caption,0);
        assert(lv_obj_get_parent(caption)==screen);
        if (i==1) assert(lv_obj_get_x(gallery_caption(0))+lv_obj_get_width(gallery_caption(0))<lv_obj_get_x(caption));
        assert(lv_obj_get_width(caption)==184 && lv_obj_get_height(caption)==80);
        assert(lv_obj_get_x(caption)==43+(int)i*190 && lv_obj_get_y(caption)==306);
        assert(lv_obj_get_style_text_font(text,0)==&meo_font_44);
        assert(lv_obj_get_style_text_opa(text,0)==255);
        assert(!strcmp(lv_label_get_text(text),names[(first+i)%3]));
        assert(lv_obj_get_height(text)==meo_font_44.line_height);
    }
}

static void launcher_select(unsigned index)
{
    assert(index<3 && meo_ui_model()->page==MEO_APPS);
    for (unsigned tries=0; tries<6 && launcher_index()!=index; ++tries) {
        swipe(325,230,135,230); advance(500);
        assert(meo_ui_model()->page==MEO_APPS);
    }
    assert(launcher_index()==index); advance(200); assert_launcher();
}

static void launcher_open(unsigned index, meo_page_t page)
{
    launcher_select(index); tap(135,230);
    assert(meo_ui_model()->page==page);
}

static double wall_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    return counter.QuadPart*1000.0/frequency.QuadPart;
#else
    return clock()*1000.0/CLOCKS_PER_SEC;
#endif
}

static int compare_double(const void *a, const void *b)
{ double d=*(const double *)a-*(const double *)b; return d<0 ? -1 : d>0; }

static void test_carousel_motion(const char *folder)
{
    launcher_select(0);
    double samples[180],total=0; unsigned rendered=0,n=0;
    for (unsigned pass=0; pass<3; ++pass) {
        int start=pass%2==0 ? 325 : 135, end=pass%2==0 ? 135 : 325;
        pointer_data.point.x=459-start; pointer_data.point.y=459-230;
        pointer_data.state=LV_INDEV_STATE_PRESSED; advance(32);
        for (unsigned frame=0; frame<60; ++frame) {
            if (frame<30) pointer_data.point.x=459-(start+(end-start)*(int)(frame+1)/30);
            else pointer_data.state=LV_INDEV_STATE_RELEASED;
            unsigned before=flushes;
            double begin=wall_ms(); advance(16); lv_refr_now(NULL); samples[n]=wall_ms()-begin;
            total+=samples[n]; rendered+=flushes>before;
            char name[48]; snprintf(name,sizeof(name),"carousel-%03u",n++);
            snapshot_pixels(folder,name);
            assert(meo_ui_model()->page==MEO_APPS);
            for (unsigned slot=0; slot<2; ++slot) {
                lv_obj_t *caption=gallery_caption(slot);
                assert(lv_obj_get_x(caption)==43+(int)slot*190 && lv_obj_get_y(caption)==306);
                if (frame>=3 && frame<29)
                    assert(lv_obj_get_style_text_opa(lv_obj_get_child(caption,0),0)==0);
            }
        }
        advance(400); assert(launcher_index()==(pass%2==0 ? 1u : 0u)); assert_launcher();
    }
    assert(rendered>=90); qsort(samples,n,sizeof(samples[0]),compare_double);
    printf("DESKTOP_ONLY: soft islands 16ms samples active=%u/%u; mean=%.3fms p95=%.3fms max=%.3fms (not panel FPS)\n",
           rendered,n,total/n,samples[n*95/100],samples[n-1]);
    /* Cross both seams repeatedly. Four populated cards remain, no dead end. */
    for (unsigned direction=0; direction<2; ++direction) for (unsigned i=0;i<9;++i) {
        unsigned expected=(launcher_index()+(direction ? 2 : 1))%3;
        swipe(direction ? 135 : 325,230,direction ? 325 : 135,230);advance(400);
        assert(launcher_index()==expected); assert_launcher();
    }
}

static void test_carousel_interruption(void)
{
    launcher_select(0);
    lv_obj_t *rail=gallery_rail();
    int initial=lv_obj_get_scroll_x(rail);
    lv_obj_scroll_to_x(rail,initial+190,LV_ANIM_ON); advance(70);
    assert(lv_obj_is_scrolling(rail));
    pointer_data.point.x=459-135; pointer_data.point.y=459-230;
    pointer_data.state=LV_INDEV_STATE_PRESSED; advance(500);
    pointer_data.state=LV_INDEV_STATE_RELEASED; advance(500);
    assert(meo_ui_model()->page==MEO_APPS && launcher_index()==1); assert_launcher();
    /* Both the generous label whitespace and the right core tile launch. */
    tap(135,370); assert(meo_ui_model()->page==MEO_HOME);
    tap(230,220); assert_launcher();
    launcher_select(0); tap(325,230); assert(meo_ui_model()->page==MEO_HOME);
    tap(230,220); assert_launcher();
    /* Edge peeks communicate more content, not small ambiguous app targets. */
    tap(12,230); assert(meo_ui_model()->page==MEO_APPS);
    tap(448,230); assert(meo_ui_model()->page==MEO_APPS);
    tap(230,370); assert(meo_ui_model()->page==MEO_APPS); /* no overlapping captions */
    tap(50,370); assert(meo_ui_model()->page==MEO_SETTINGS); /* transparent caption padding */
    tap(230,50); assert(meo_ui_model()->page==MEO_APPS); assert_launcher();
}

static void test_caption_fade_reversal(void)
{
    launcher_select(0);
    lv_obj_t *rail=gallery_rail();
    int initial=lv_obj_get_scroll_x(rail);
    lv_obj_scroll_to_x(rail,initial+190,LV_ANIM_ON); advance(60);
    for (unsigned slot=0; slot<2; ++slot)
        assert(lv_obj_get_style_text_opa(lv_obj_get_child(gallery_caption(slot),0),0)==0);
    for (unsigned elapsed=0; lv_obj_is_scrolling(rail) && elapsed<1000; elapsed+=5) advance(5);
    assert(!lv_obj_is_scrolling(rail));
    lv_obj_t *title=lv_obj_get_child(gallery_caption(0),0);
    assert(!strcmp(lv_label_get_text(title),"AI"));
    advance(60); assert(lv_obj_get_style_text_opa(title,0)>0 && lv_obj_get_style_text_opa(title,0)<255);
    lv_obj_scroll_to_x(rail,initial,LV_ANIM_ON); advance(35);
    assert(lv_obj_get_style_text_opa(title,0)==0); /* old fade was cancelled */
    advance(600); assert(launcher_index()==0); assert_launcher();
    puts("PASS: fixed 44px captions hidden in transit, fade only after settle, reverse cancels old fade");
}

static void copy_frame(unsigned char *copy)
{
    lv_refr_now(NULL);
    memcpy(copy, framebuffer, sizeof(framebuffer));
}

static void assert_frame_equal(const unsigned char *expected)
{
    assert(!memcmp(expected, framebuffer, sizeof(framebuffer)));
}

static void settle_page(unsigned ms)
{
    advance(ms);
    lv_refr_now(NULL);
    lv_obj_update_layout(lv_screen_active());
}

static void open_page_for_back_drag(meo_page_t page)
{
    meo_ui_navigate(page);
    settle_page(350);
    assert(meo_ui_model()->page == page);
}

static void pointer_press(int x, int y)
{
    pointer_data.point.x = 459 - x;
    pointer_data.point.y = 459 - y;
    pointer_data.state = LV_INDEV_STATE_PRESSED;
    advance(32);
}

static void pointer_move(int x, int y, unsigned ms)
{
    pointer_data.point.x = 459 - x;
    pointer_data.point.y = 459 - y;
    pointer_data.state = LV_INDEV_STATE_PRESSED;
    advance(ms);
}

static void pointer_release(unsigned ms)
{
    pointer_data.state = LV_INDEV_STATE_RELEASED;
    advance(ms);
}

static int screen_x(void)
{
    lv_obj_update_layout(lv_screen_active());
    return lv_obj_get_x(lv_screen_active());
}

static void assert_parent_preview(const unsigned char *parent_frame, int child_dx)
{
    lv_refr_now(NULL);
    int root_dx = screen_x();
    assert(child_dx > 0 && child_dx < 460);
    assert(abs(root_dx - child_dx) <= 1);
    /* The child root is translated to +dx. Its exposed left strip is the
       right edge of the native parent screen translated underneath it. */
    for (int y = 0; y < 460; ++y) {
        for (int x = 0; x < root_dx; ++x) {
            const unsigned char *actual = framebuffer + (y * 460 + x) * 3;
            const unsigned char *expected = parent_frame +
                (y * 460 + (460 - root_dx + x)) * 3;
            assert(!memcmp(actual, expected, 3));
        }
    }
}

static void assert_cancelled_drag(int start_x, int start_y, int end_x, int end_y,
                                  const char *label)
{
    static unsigned char before[sizeof(framebuffer)];
    open_page_for_back_drag(MEO_DISPLAY);
    copy_frame(before);
    int initial_x = screen_x();
    assert(initial_x == 0);
    pointer_press(start_x, start_y);
    pointer_move(end_x, end_y, 48);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    pointer_release(16);
    settle_page(350);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    assert(screen_x() == initial_x);
    assert_frame_equal(before);
    open_page_for_back_drag(MEO_DISPLAY);
    assert(screen_x() == 0);
    printf("PASS: back drag cancelled (%s)\n", label);
}

static void assert_committed_drag(meo_page_t page, meo_page_t parent,
                                  int start_x, int start_y, int end_x, int end_y,
                                  const char *label)
{
    open_page_for_back_drag(page);
    pointer_press(start_x, start_y);
    pointer_move(end_x, end_y, 64);
    assert(meo_ui_model()->page == page);
    pointer_release(16);
    settle_page(350);
    assert(meo_ui_model()->page == parent);
    printf("PASS: back drag committed (%s)\n", label);
}

static void assert_header_clicks(const char *folder)
{
    static const struct { int x, y; const char *label; } points[] = {
        {230, 72, "title label"},
        {380, 72, "header blank"},
        { 52, 72, "arrow area"},
    };
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        open_page_for_back_drag(MEO_DISPLAY);
        if (i == 0) snapshot_pixels(folder, "back-header-example");
        tap(points[i].x, points[i].y);
        assert(meo_ui_model()->page == MEO_SETTINGS);
        printf("PASS: full header click (%s)\n", points[i].label);
    }
}

static void assert_control_drag_isolation(void)
{
    uint8_t brightness_before = meo_ui_model()->settings.brightness;
    open_page_for_back_drag(MEO_DISPLAY);
    pointer_press(230, 232); /* slider thumb/track: back gesture is excluded */
    pointer_move(350, 232, 64);
    pointer_release(16);
    settle_page(250);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    while (meo_ui_model()->settings.brightness > brightness_before) tap(80, 235);
    while (meo_ui_model()->settings.brightness < brightness_before) tap(375, 235);

    bool reduced_before = meo_ui_model()->settings.reduced_motion;
    open_page_for_back_drag(MEO_ACCESSIBILITY);
    pointer_press(348, 193); /* switch: back gesture is excluded */
    pointer_move(420, 193, 64);
    pointer_release(16);
    settle_page(250);
    assert(meo_ui_model()->page == MEO_ACCESSIBILITY);
    if (meo_ui_model()->settings.reduced_motion != reduced_before) tap(348, 193);
    assert(meo_ui_model()->settings.reduced_motion == reduced_before);
    printf("PASS: slider/switch drag starts stay on their controls\n");
}

static void assert_animation_press_is_swallowed(void)
{
    meo_ui_navigate(MEO_SETTINGS);
    settle_page(350);
    /* The press begins during the screen transition to DISPLAY. It must not
       become a later back gesture after the target screen settles. */
    meo_ui_navigate(MEO_DISPLAY);
    pointer_press(230, 180);
    pointer_move(350, 180, 48);
    pointer_release(16);
    settle_page(350);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    printf("PASS: screen-animation press swallowed\n");
}

static void assert_back_drag_frames(const char *folder)
{
    static unsigned char parent_frame[sizeof(framebuffer)];
    open_page_for_back_drag(MEO_SETTINGS);
    copy_frame(parent_frame);

    meo_ui_navigate(MEO_DISPLAY);
    settle_page(350);
    int initial_x = screen_x();
    pointer_press(230, 180);
    for (unsigned frame = 0; frame < 16; ++frame) {
        int dx = (int)frame * 8;
        pointer_move(230 + dx, 180, 16);
        assert(meo_ui_model()->page == MEO_DISPLAY);
        /* The recognizer deliberately ignores the first 12px slop. */
        int expected_dx = dx <= 12 ? 0 : dx;
        assert(abs(screen_x() - initial_x - expected_dx) <= 1);
        if (dx == 40) assert_parent_preview(parent_frame, dx);
        {
            char name[48];
            snprintf(name, sizeof(name), "back-drag-%03u", frame);
            snapshot_pixels(folder, name);
        }
    }
    snapshot_pixels(folder, "back-drag-example");
    assert(meo_ui_model()->page == MEO_DISPLAY);
    pointer_release(16);
    settle_page(350);
    assert(meo_ui_model()->page == MEO_SETTINGS);
    printf("PASS: 16ms back-drag frames, root-screen follow, native parent preview\n");
}

static void assert_no_back_on_root_pages(void)
{
    open_page_for_back_drag(MEO_APPS);
    pointer_press(230, 230);
    pointer_move(350, 230, 64);
    pointer_release(16);
    settle_page(350);
    assert(meo_ui_model()->page == MEO_APPS);

    open_page_for_back_drag(MEO_HOME);
    pointer_press(230, 230);
    pointer_move(350, 230, 64);
    pointer_release(16);
    settle_page(350);
    assert(meo_ui_model()->page == MEO_HOME);
    printf("PASS: AI/home and app root do not back-navigate\n");
}

static void assert_drag_reversal_and_directions(void)
{
    assert_cancelled_drag(230, 180, 309, 180, "short dx=79");
    assert_cancelled_drag(230, 180, 241, 180, "12px slop");
    assert_cancelled_drag(230, 180, 230, 300, "vertical");
    assert_cancelled_drag(230, 180, 330, 260, "vertical-dominant diagonal");
    assert_cancelled_drag(230, 180, 110, 180, "leftward");

    static unsigned char before[sizeof(framebuffer)];
    open_page_for_back_drag(MEO_DISPLAY);
    copy_frame(before);
    int initial_x = screen_x();
    pointer_press(230, 180);
    pointer_move(350, 180, 48);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    assert(abs(screen_x() - initial_x - 120) <= 1);
    pointer_move(250, 180, 48);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    assert(abs(screen_x() - initial_x - 20) <= 1);
    pointer_release(16);
    settle_page(350);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    assert(screen_x() == initial_x);
    assert_frame_equal(before);
    printf("PASS: drag reversal cancels and restores exact pixels/position\n");

    assert_committed_drag(MEO_DISPLAY, MEO_SETTINGS, 230, 180, 310, 180,
                          "exact threshold dx=80");
}

static void assert_reduced_motion_back_drag(bool reduced)
{
    open_page_for_back_drag(MEO_DISPLAY);
    int initial_x = screen_x();
    assert(initial_x == 0);
    pointer_press(230, 180);
    pointer_move(309, 180, 64);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    if (reduced) assert(screen_x() == initial_x);
    else assert(abs(screen_x() - initial_x - 79) <= 1);
    pointer_release(16);
    settle_page(30);
    assert(meo_ui_model()->page == MEO_DISPLAY);

    open_page_for_back_drag(MEO_DISPLAY);
    initial_x = screen_x();
    assert(initial_x == 0);
    pointer_press(230, 180);
    pointer_move(310, 180, 64);
    assert(meo_ui_model()->page == MEO_DISPLAY);
    if (reduced) assert(screen_x() == initial_x);
    else assert(abs(screen_x() - initial_x - 80) <= 1);
    pointer_release(16);
    settle_page(reduced ? 30 : 350);
    assert(meo_ui_model()->page == MEO_SETTINGS);
    assert(screen_x() == 0);
    printf("PASS: reduced-motion back decision/settle (%s)\n",
           reduced ? "enabled" : "disabled");
}

static void assert_child_start_positions(void)
{
    static const struct {
        meo_page_t page, parent;
        int y;
        const char *label;
    } cases[] = {
        {MEO_DISPLAY,       MEO_SETTINGS, 150, "display upper"},
        {MEO_DISPLAY,       MEO_SETTINGS, 300, "display center"},
        {MEO_DISPLAY,       MEO_SETTINGS, 350, "display lower"},
        {MEO_WIFI,          MEO_SETTINGS, 150, "wifi upper"},
        {MEO_WIFI,          MEO_SETTINGS, 300, "wifi lower"},
        {MEO_ACCESSIBILITY, MEO_SETTINGS, 140, "accessibility upper"},
        {MEO_ACCESSIBILITY, MEO_SETTINGS, 300, "accessibility lower"},
        {MEO_BREATHE,       MEO_APPS,     180, "breathe upper"},
        {MEO_BREATHE,       MEO_APPS,      40, "breathe top"},
        {MEO_BREATHE,       MEO_APPS,     280, "breathe center"},
        {MEO_BREATHE,       MEO_APPS,     350, "breathe lower"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert_committed_drag(cases[i].page, cases[i].parent, 230, cases[i].y,
                              310, cases[i].y, cases[i].label);
    }
}

static void assert_breathe_back_behavior(void)
{
    open_page_for_back_drag(MEO_BREATHE);
    meo_ui_feed_respiration((meo_resp_t){.waveform=.4f,.rate_bpm=12.4f,.valid=true,.rate_valid=true,.at_ms=lv_tick_get()});
    advance(80); assert(meo_ui_model()->respiration_live);
    pointer_press(230,280); pointer_move(290,280,64); pointer_release(16); settle_page(350);
    assert(meo_ui_model()->page==MEO_BREATHE && meo_ui_model()->respiration_live);
    pointer_press(230,280); pointer_move(310,280,64); pointer_release(16); settle_page(350);
    assert(meo_ui_model()->page==MEO_APPS && meo_ui_model()->respiration_live);
    printf("PASS: breathing back gesture preserves sensor data on cancel and commit\n");
}

static void restore_back_drag_settings(meo_settings_t saved, meo_page_t saved_page)
{
    meo_ui_navigate(MEO_DISPLAY);
    settle_page(350);
    while (meo_ui_model()->settings.brightness > saved.brightness) tap(80, 235);
    while (meo_ui_model()->settings.brightness < saved.brightness) tap(375, 235);

    meo_ui_navigate(MEO_ACCESSIBILITY);
    settle_page(350);
    if (meo_ui_model()->settings.reduced_motion != saved.reduced_motion)
        tap(348, 193);

    meo_ui_navigate(saved_page);
    settle_page(350);
}

static void test_back_drag_and_header(const char *folder)
{
    meo_settings_t saved_settings = meo_ui_model()->settings;
    meo_page_t saved_page = meo_ui_model()->page;

    /* Exercise both animation policies while keeping the release threshold
       identical. The current setting is restored after all new coverage. */
    if (saved_settings.reduced_motion) {
        assert_reduced_motion_back_drag(true);
        meo_ui_navigate(MEO_ACCESSIBILITY); settle_page(350); tap(348, 193);
        assert_reduced_motion_back_drag(false);
    } else {
        assert_reduced_motion_back_drag(false);
        meo_ui_navigate(MEO_ACCESSIBILITY); settle_page(350); tap(348, 193);
        assert_reduced_motion_back_drag(true);
    }

    assert_header_clicks(folder);
    assert_control_drag_isolation();
    assert_animation_press_is_swallowed();
    assert_drag_reversal_and_directions();
    assert_child_start_positions();
    /* The frame-by-frame preview assertions intentionally exercise the
       animated policy; restore it after the reduced-motion branch above. */
    if (meo_ui_model()->settings.reduced_motion) {
        meo_ui_navigate(MEO_ACCESSIBILITY);
        settle_page(350);
        tap(348, 193);
    }
    assert_back_drag_frames(folder);
    assert_no_back_on_root_pages();

    assert_committed_drag(MEO_SETTINGS, MEO_APPS, 230, 150, 310, 150,
                          "settings parent");
    assert_committed_drag(MEO_WIFI, MEO_SETTINGS, 230, 250, 310, 250,
                          "wifi parent");
    assert_committed_drag(MEO_ACCESSIBILITY, MEO_SETTINGS, 230, 300, 310, 300,
                          "accessibility parent");
    assert_breathe_back_behavior();

    restore_back_drag_settings(saved_settings, saved_page);
    puts("PASS: parameterized rotated-pointer back drag, header clicks, parent hierarchy,");
    puts("      cancellation/reversal/direction guards, control isolation, preview pixels and PPM frames");
}


static void test_expression_visuals(const char *folder)
{
    meo_ui_navigate(MEO_ACCESSIBILITY); advance(250);
    tap(348, 188); assert(meo_ui_model()->settings.reduced_motion);
    meo_ui_navigate(MEO_HOME); advance(250);
    advance(2000);
    uint32_t hashes[MEO_EYE_COUNT];
    static const char *names[] = {"17-calm", "18-happy", "19-curious", "20-sleepy",
                                  "21-surprised", "22-wink", "23-sad"};
    for (int e = 0; e < MEO_EYE_COUNT; ++e) {
        meo_ui_set_expression((meo_expression_t)e, 0); advance(80);
        assert(meo_ui_model()->expression == (meo_expression_t)e);
        assert_monochrome_home(true); snapshot(folder, names[e]);
        hashes[e] = home_hash();
        for (int previous = 0; previous < e; ++previous) assert(hashes[previous] != hashes[e]);
        advance(1000); lv_refr_now(NULL); assert(home_hash() == hashes[e]);
    }
    meo_ui_auto_expression(); advance(80); assert_monochrome_home(true);
    assert(home_hash() == hashes[MEO_EYE_CALM]);
    meo_ui_navigate(MEO_ACCESSIBILITY); advance(80); tap(348, 188);
    assert(!meo_ui_model()->settings.reduced_motion);
    meo_ui_navigate(MEO_HOME); advance(250);
    for (unsigned n = 0; n < 100; ++n) {
        if (n % 20 == 0) meo_ui_set_expression((meo_expression_t)(n / 20), 0);
        advance(80); assert_monochrome_home(false);
        char name[40]; snprintf(name, sizeof(name), "eyes-motion-%03u", n); snapshot(folder, name);
    }
    meo_ui_auto_expression(); advance(1000);
}

static int real_main(int argc, char **argv);

int main(int argc, char **argv)
{
#ifdef STEADY_ONLY
    (void)argc; (void)argv;
    lv_init();
    lv_display_t *disp = lv_display_create(460, 460);
    static uint32_t buf[460 * 460];
    /* Without a flush callback LVGL never sees flush_ready and the first
       lv_timer_handler() spins forever - this branch was unusable. */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);
    meo_ui_config_t cfg = {.settings = meo_default_settings()};
    fprintf(stderr, "STEADY cfg ready\n");
    meo_ui_create(&cfg);
    fprintf(stderr, "STEADY created\n");
    for (int i = 0; i < 10; ++i) { lv_tick_inc(33); lv_timer_handler(); usleep(33000); }
    fprintf(stderr, "STEADY home OK\n");
    meo_ui_navigate(MEO_SETTINGS);
    for (int i = 0; i < 10; ++i) { lv_tick_inc(33); lv_timer_handler(); usleep(33000); }
    fprintf(stderr, "STEADY settings OK\n");
    meo_ui_navigate(MEO_SPEAKER);
    for (int i = 0; i < 20; ++i) { lv_tick_inc(33); lv_timer_handler(); usleep(33000); }
    fprintf(stderr, "STEADY speaker OK\n");
    fprintf(stderr, "STEADY DONE\n");
    (void)real_main;
    return 0;
#else
    return real_main(argc, argv);
#endif
}
static int real_main(int argc, char **argv)
{
    setbuf(stdout,NULL);
    assert(argc == 2);
    lv_init();
    lv_display_t *display = lv_display_create(460, 460);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(display, drawbuffer, NULL, sizeof(drawbuffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_display(pointer, display);
    meo_ui_config_t config = {.settings=meo_default_settings(), .settings_changed=saved,
                              .wifi_scan=wifi_scan_cb, .wifi_join=wifi_join_cb, .wifi_connect=wifi_connect_cb};
    meo_ui_create(&config);
    assert(lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_180);
    advance(1000);
    snapshot(argv[1], "01-eyes");
    assert_monochrome_home(true);
    lv_obj_t *eye=lv_obj_get_child(lv_screen_active(),0);
    int initial_x=lv_obj_get_x(eye), initial_y=lv_obj_get_y(eye);
    /* Explicit simulated input only; no claim that the hardware radar is live. */
    meo_ui_feed_gaze((meo_gaze_t){.x=1,.y=1,.valid=true,.at_ms=lv_tick_get()});
    advance(400); assert(meo_ui_model()->gaze_live);
    assert(lv_obj_get_x(eye)>initial_x+7 && lv_obj_get_y(eye)>initial_y+5);
    assert_monochrome_home(true);
    meo_ui_feed_gaze((meo_gaze_t){.valid=false,.at_ms=lv_tick_get()});
    advance((unsigned)(meo_ui_model()->next_blink_ms-lv_tick_get())+110);
    assert(meo_ui_model()->openness<.35f);
    assert_monochrome_home(false);
    advance(250); assert_monochrome_home(true);
    test_expression_visuals(argv[1]);
    assert(meo_ui_model()->page == MEO_HOME);
    tap(230, 220); assert(meo_ui_model()->page == MEO_APPS);
    snapshot(argv[1], "02-apps");
    assert_launcher();
    tap(230,24); assert(meo_ui_model()->page == MEO_APPS); /* no header/back */
    swipe(24,230,180,230); assert(meo_ui_model()->page == MEO_APPS); /* root */
    launcher_open(1,MEO_HOME); /* AI is a peer app, not a back destination */
    assert_monochrome_home(true);
    tap(230,220); assert_launcher();
    /* Every page now shares the same true-black canvas. */
    const unsigned char *app_bg=framebuffer+(10*460+230)*3;
    assert(app_bg[0]==0 && app_bg[1]==0 && app_bg[2]==0);
    test_carousel_motion(argv[1]);
    test_carousel_interruption();
    test_caption_fade_reversal();
    launcher_select(1);
    /* A 24px peek is a scroll affordance, not a tiny launch target. */
    tap(448,230); advance(400);
    assert(meo_ui_model()->page==MEO_APPS && launcher_index()==1);
    launcher_select(2);
    snapshot(argv[1],"25-apps-breathe");
    launcher_select(0); snapshot(argv[1],"24-apps-settings");
    /* Slow drags, reversal, and vertical travel do not launch a card. */
    swipe(150,210,300,210); assert(meo_ui_model()->page==MEO_APPS);
    slow_drag(false); assert(meo_ui_model()->page==MEO_APPS);
    slow_drag(true); assert(meo_ui_model()->page==MEO_APPS);
    swipe(230,160,230,290); assert(meo_ui_model()->page==MEO_APPS);
    launcher_open(0,MEO_SETTINGS);
    snapshot(argv[1], "03-settings");
    assert_settings_typography();
    assert_icons(meo_ui_model()->page==MEO_SETTINGS ? 10 : meo_ui_model()->page==MEO_DISPLAY ? 3 : meo_ui_model()->page==MEO_WIFI ? 2 : 1);
    meo_ui_navigate(MEO_DISPLAY);
    meo_ui_navigate(MEO_WIFI); assert(meo_ui_model()->page==MEO_DISPLAY); /* transition guard */
    advance(230); meo_ui_navigate(MEO_SETTINGS); advance(230);
    tap(230,237); assert(meo_ui_model()->page==MEO_DISPLAY);
    snapshot(argv[1],"15-display-brightness");
    assert_settings_typography();
    assert_icons(meo_ui_model()->page==MEO_SETTINGS ? 10 : meo_ui_model()->page==MEO_DISPLAY ? 3 : meo_ui_model()->page==MEO_WIFI ? 2 : 1);
    tap(375,235); assert(meo_ui_model()->settings.brightness==90);
    tap(375,235); assert(meo_ui_model()->settings.brightness==100);
    assert_settings_typography();
    assert_icons(meo_ui_model()->page==MEO_SETTINGS ? 10 : meo_ui_model()->page==MEO_DISPLAY ? 3 : meo_ui_model()->page==MEO_WIFI ? 2 : 1);
    snapshot(argv[1],"26-display-brightness-100");
    unsigned boundary_saves=saves;
    tap(375,235); assert(saves==boundary_saves);
    swipe(310,235,150,235); assert(meo_ui_model()->page==MEO_DISPLAY);
    assert(meo_ui_model()->settings.brightness<=35);
    swipe(150,235,310,235); assert(meo_ui_model()->page==MEO_DISPLAY);
    assert(meo_ui_model()->settings.brightness>=85); /* slider movement, not back */
    /* Return from the display subpage through the large header hit area before
       opening the accessibility row. */
    tap(230,50); assert(meo_ui_model()->page==MEO_SETTINGS);
    tap(230,280); assert(meo_ui_model()->page==MEO_ACCESSIBILITY);
    snapshot(argv[1],"16-accessibility");
    assert_settings_typography();
    assert_icons(meo_ui_model()->page==MEO_SETTINGS ? 10 : meo_ui_model()->page==MEO_DISPLAY ? 3 : meo_ui_model()->page==MEO_WIFI ? 2 : 1);
    tap(360,193); assert(meo_ui_model()->settings.reduced_motion); assert(saves > 0);
    advance(250); snapshot(argv[1],"30-accessibility-enabled");
    lv_obj_t *old_screen=lv_screen_active();
    meo_ui_navigate(MEO_SETTINGS); assert(lv_screen_active()!=old_screen); /* true 0ms reduced-motion pop */
    meo_ui_navigate(MEO_ACCESSIBILITY); /* immediate next navigation remains usable */
    assert(meo_ui_model()->page==MEO_ACCESSIBILITY);
    assert_settings_typography();
    assert_icons(meo_ui_model()->page==MEO_SETTINGS ? 10 : meo_ui_model()->page==MEO_DISPLAY ? 3 : meo_ui_model()->page==MEO_WIFI ? 2 : 1);
    tap(94,86); assert(meo_ui_model()->page==MEO_SETTINGS);
    assert_settings_typography();
    assert_icons(meo_ui_model()->page==MEO_SETTINGS ? 10 : meo_ui_model()->page==MEO_DISPLAY ? 3 : meo_ui_model()->page==MEO_WIFI ? 2 : 1);
    snapshot(argv[1],"27-settings-reduced");
    tap(230,151); assert(meo_ui_model()->page == MEO_WIFI);
    snapshot(argv[1], "04-wifi-unavailable");
    {   /* Scan list + shutdown overlay are new surfaces: render them. */
        const char aps[3][33] = {"MEOWPAS", "MEO-IoT", "TP-LINK_5G"};
        const int rssi[3] = {-58, -71, -83};
        meo_ui_set_wifi(MEO_WIFI_ONLINE, "MEOWPAS", -58);
        meo_ui_set_power(93, true);
        meo_ui_set_scan_list((const char (*)[33])aps, rssi, 3);
        advance(50);
        snapshot(argv[1], "04a-wifi-scan-list");
        meo_ui_set_wifi(MEO_WIFI_CONNECTING, "MEOWPAS", 0);
        advance(900);                    /* past the notice pulse valley */
        snapshot(argv[1], "04i-wifi-connecting-state");
        color_check_disabled = true;
        meo_ui_set_shutdown(5);
        advance(60);
        snapshot(argv[1], "04b0-shutdown-fade-in");   /* mid fade: proves the entry */
        advance(400);
        snapshot(argv[1], "04b-shutdown-countdown");  /* settled: must be opaque */
        meo_ui_set_shutdown(0);
        advance(50);
        snapshot(argv[1], "04c-shutdown-final");
        meo_ui_set_shutdown(-1);
        color_check_disabled = false;
        meo_ui_set_wifi(MEO_WIFI_OFF, NULL, 0);
        meo_ui_set_power(-1, false);
        meo_ui_set_scan_list(aps, rssi, 0);
        advance(50);
    }
    {   /* Taps must dispatch: entering the page scans, the button scans again,
           and a row tap joins exactly the SSID that was rendered. */
        unsigned scans = wifi_scan_calls, joins = wifi_join_calls;
        meo_ui_navigate(MEO_HOME); advance(250);
        meo_ui_navigate(MEO_WIFI); advance(300);
        assert(meo_ui_model()->page == MEO_WIFI);
        assert(wifi_scan_calls == scans + 1);          /* auto-scan on entry */
        tap(230, 258);
        assert(wifi_scan_calls == scans + 2);          /* explicit scan button */
        advance(640);                                  /* pulse valley */
        snapshot(argv[1], "04h-notice-dim");
        const char one[3][33] = {"MEOWPAS", "", ""};
        const int one_rssi[3] = {-58, 0, 0};
        meo_ui_set_scan_list((const char (*)[33])one, one_rssi, 1);
        advance(60);
        tap(230, 331);
        assert(wifi_join_calls == joins + 1);
        assert(strcmp(wifi_join_ssid, "MEOWPAS") == 0);
        advance(60);
        snapshot(argv[1], "04d-wifi-tap-joined");
        wifi_join_result = false;                      /* no saved password path */
        tap(230, 331);
        assert(wifi_join_calls == joins + 2);
        advance(60);
        snapshot(argv[1], "04e-wifi-tap-no-password");
        wifi_join_result = true;
        meo_ui_set_scan_list((const char (*)[33])one, one_rssi, 0);
        meo_ui_navigate(MEO_WIFI); advance(300);  /* leave the flow where it was */
    }
    {   /* On-screen password entry: a row tap with no saved key must open it,
           multi-tap must build "AB", 123 mode must append digits, and OK must
           hand the exact SSID and password to the adapter. */
        /* keys: 99x64 at (24,240), 10 px gaps -> row y 272/348/424,
           col x 73/182/291/400; OK is the last key at (400,424). */
        const char one[3][33] = {"MEOWPAS", "", ""};
        const int one_rssi[3] = {-58, 0, 0};
        meo_ui_set_scan_list((const char (*)[33])one, one_rssi, 1);
        advance(60);
        unsigned connects = wifi_connect_calls;
        wifi_join_result = false;
        tap(230, 331);
        assert(meo_ui_model()->page == MEO_PASSWORD);
        advance(200);
        tap(380, 72);                      /* abc -> ABC */
        tap(182, 272);                     /* 2 -> A */
        advance(200);
        snapshot(argv[1], "04g-password-letters");
        advance(600);                      /* commit */
        tap(182, 272);                     /* A again */
        tap(182, 272);                     /* same key quickly -> B */
        advance(800);
        tap(380, 72);                      /* ABC -> 123 */
        tap(73, 272);                      /* 1 */
        tap(182, 272);                     /* 2 */
        advance(200);
        snapshot(argv[1], "04f-password-entry");
        tap(400, 424);                     /* OK */
        assert(wifi_connect_calls == connects + 1);
        assert(strcmp(wifi_connect_ssid, "MEOWPAS") == 0);
        assert(strcmp(wifi_connect_pass, "AB12") == 0);
        wifi_join_result = true;
        advance(250);
        meo_ui_navigate(MEO_WIFI); advance(250);
    }
    {   /* Microphone meter: the level must actually drive the bars. */
        meo_ui_navigate(MEO_MIC); advance(300);
        assert(meo_ui_model()->page == MEO_MIC);
        const float both[2] = {0.62f, 0.28f};
        meo_ui_set_mic(both, true);
        advance(500);
        snapshot(argv[1], "04j-mic-level");
        const float quiet[2] = {0.0f, 0.0f};
        meo_ui_set_mic(quiet, true);
        advance(700);
        /* A silent meter is legitimately monochrome (dim segments, grey text):
           the "every page carries accent ink" rule does not apply to it. */
        color_check_disabled = true;
        snapshot(argv[1], "04k-mic-silence");
        color_check_disabled = false;
        meo_ui_navigate(MEO_WIFI); advance(250);
    }
    assert_settings_typography();
    assert_icons(meo_ui_model()->page==MEO_SETTINGS ? 10 : meo_ui_model()->page==MEO_DISPLAY ? 3 : meo_ui_model()->page==MEO_WIFI ? 2 : 1);
    tap(94, 86); assert(meo_ui_model()->page == MEO_SETTINGS);
    tap(230,280); tap(360,193); assert(!meo_ui_model()->settings.reduced_motion);
    tap(94,86); assert(meo_ui_model()->page==MEO_SETTINGS);
    swipe(24,230,38,230); assert(meo_ui_model()->page==MEO_SETTINGS); /* cancel */
    swipe(24,230,70,310); assert(meo_ui_model()->page==MEO_SETTINGS); /* vertical/diagonal */
    swipe(24,230,180,230);
    assert(meo_ui_model()->page==MEO_APPS);
    assert(launcher_index()==0); /* retain focused card on return */
    launcher_open(2,MEO_BREATHE);
    assert(meo_ui_model()->mode==MEO_RADAR && !meo_ui_model()->running);
    assert(!meo_ui_model()->respiration_live && !meo_ui_model()->rate_live);
    snapshot(argv[1],"05-breathe-waiting");
    assert_minimal_breathe(); assert_icons(0);
    tap(94,86); assert(meo_ui_model()->page==MEO_BREATHE); /* no hidden toolbar */
    /* No demo oscillator or timer when sensor input is absent. */
    static unsigned char waiting_frame[sizeof(framebuffer)];
    lv_refr_now(NULL); memcpy(waiting_frame,framebuffer,sizeof(waiting_frame));
    advance(3000); lv_refr_now(NULL);
    assert(!memcmp(waiting_frame,framebuffer,sizeof(waiting_frame)));
    tap(324,402); assert(!meo_ui_model()->running); /* no session action */
    meo_ui_set_breath_mode(MEO_GUIDED); assert(meo_ui_model()->mode==MEO_RADAR);

    /* Synthetic samples are injected only by this desktop test, not firmware. */
    meo_ui_feed_respiration((meo_resp_t){.waveform=-1,.rate_bpm=12.4f,.at_ms=lv_tick_get(),.valid=true,.rate_valid=false});
    advance(300); assert(meo_ui_model()->respiration_live && !meo_ui_model()->rate_live);
    snapshot(argv[1],"06-simulated-wave-only"); assert_minimal_breathe();
    static unsigned char low_frame[sizeof(framebuffer)];
    memcpy(low_frame,framebuffer,sizeof(low_frame));
    meo_ui_feed_respiration((meo_resp_t){.waveform=1,.rate_bpm=12.4f,.at_ms=lv_tick_get(),.valid=true,.rate_valid=false});
    advance(500); lv_refr_now(NULL); assert(memcmp(low_frame,framebuffer,sizeof(low_frame)));
    snapshot(argv[1],"07-simulated-wave-peak");
    meo_ui_feed_respiration((meo_resp_t){.waveform=.5f,.rate_bpm=12.4f,.at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
    advance(150); assert(meo_ui_model()->rate_live);
    assert(!lv_obj_has_flag(breath_rate(),LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(breath_rate()),"12.4 / min"));
    snapshot(argv[1],"09-simulated-valid-rate"); assert_minimal_breathe();
    for (unsigned i=0; i<100; ++i) {
        meo_ui_feed_respiration((meo_resp_t){.waveform=sinf(i*2*3.14159265f/50),.rate_bpm=12.4f,
                                             .at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
        advance(100); char name[32]; snprintf(name,sizeof(name),"simulated-motion-%03u",i); snapshot(argv[1],name);
    }
    meo_ui_feed_respiration((meo_resp_t){.waveform=.2f,.rate_bpm=NAN,.at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
    advance(80); assert(meo_ui_model()->respiration_live && !meo_ui_model()->rate_live);
    assert(lv_obj_has_flag(breath_rate(),LV_OBJ_FLAG_HIDDEN));
    meo_ui_feed_respiration((meo_resp_t){.waveform=NAN,.rate_bpm=12.4f,.at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
    advance(80); assert(!meo_ui_model()->respiration_live && !meo_ui_model()->rate_live);
    assert(lv_obj_has_flag(breath_rate(),LV_OBJ_FLAG_HIDDEN));
    meo_ui_feed_respiration((meo_resp_t){.waveform=.2f,.rate_bpm=11.8f,.at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
    advance(80); assert(meo_ui_model()->rate_live);
    advance(2100); assert(!meo_ui_model()->rate_live && !meo_ui_model()->respiration_live);
    snapshot(argv[1],"10-signal-expired");
    assert(!memcmp(waiting_frame,framebuffer,sizeof(waiting_frame)));
    /* A fresh sample while away must be reflected on re-entry, while stale
       data must never leave yesterday's rate visible on the first frame. */
    swipe(100,230,260,230); assert(meo_ui_model()->page==MEO_APPS);
    meo_ui_feed_respiration((meo_resp_t){.waveform=.2f,.rate_bpm=11.8f,.at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
    advance(100); launcher_open(2,MEO_BREATHE); assert(meo_ui_model()->rate_live);
    assert(!strcmp(lv_label_get_text(breath_rate()),"11.8 / min"));
    swipe(100,230,260,230); advance(2100); launcher_open(2,MEO_BREATHE);
    assert(!meo_ui_model()->rate_live && lv_obj_has_flag(breath_rate(),LV_OBJ_FLAG_HIDDEN));
    swipe(100,230,260,230); launcher_open(0,MEO_SETTINGS);
    tap(230,280); tap(348,188); assert(meo_ui_model()->settings.reduced_motion);
    tap(94,86); tap(94,86); launcher_open(2,MEO_BREATHE);
    meo_ui_feed_respiration((meo_resp_t){.waveform=-1,.rate_bpm=12,.at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
    advance(200); snapshot(argv[1],"12-reduced-motion");
    lv_refr_now(NULL); static unsigned char reduced_frame[sizeof(framebuffer)];
    memcpy(reduced_frame,framebuffer,sizeof(reduced_frame));
    meo_ui_feed_respiration((meo_resp_t){.waveform=1,.rate_bpm=12,.at_ms=lv_tick_get(),.valid=true,.rate_valid=true});
    advance(400); lv_refr_now(NULL); assert(!memcmp(reduced_frame,framebuffer,sizeof(reduced_frame)));
    snapshot(argv[1],"13-reduced-motion-live");
    swipe(100,230,260,230); assert(meo_ui_model()->page == MEO_APPS);
    assert_launcher();
    assert(!lv_obj_has_flag(gallery_rail(),LV_OBJ_FLAG_SCROLL_MOMENTUM));
    assert(!lv_obj_has_flag(gallery_rail(),LV_OBJ_FLAG_SCROLL_ELASTIC));
    for (unsigned i=0;i<6;++i) {
        unsigned expected=(launcher_index()+1)%3;
        swipe(325,230,135,230); advance(100);
        assert(launcher_index()==expected); assert_launcher();
    }
    tap(230,24); assert(meo_ui_model()->page == MEO_APPS); /* no back, also reduced motion */
    launcher_open(1,MEO_HOME);
    assert_monochrome_home(true); /* Selecting the dial restores pure black. */
    assert(meo_ui_frames() > 100);
    puts("PASS: monochrome-only home pixels, black margins, gaze/blink and return from dark pages;");
    puts("PASS: seven pupil-free expressions, reduced-motion static pixels, gaze/blink and expression morphs;");
    puts("PASS: square-panel soft islands, two core cards + 24px peeks, cyclic native drag/inertia/snap,");
    puts("      no header/back, retained focus and reduced-motion scrolling; AI is a selectable dial;");
    puts("PASS: exact UI, rotated taps/edge swipes, hierarchical settings, slider isolation,");
    puts("      no swipe click-through, view retention, signal expiry, reduced-motion pixels");
    puts("PASS: all visible page/state glyphs and actual icon pixels (hide/show differential)");
    puts("PASS: settings names/states/subpages at native 28px+, 44px brightness, no missing glyphs/wrapping/clipping");
    puts("PASS: true-black canvases with rendered color accents, strictly monochrome home, no visible type below 28px;");
    puts("      breathing: static waiting, waveform-following circles, valid live rate only");
    test_back_drag_and_header(argv[1]);
    return 0;
}
