/* Meoave / Quiet companion. Code-native LVGL 9 visuals, no bitmap mockups.
 * Logical coordinates use the complete 460x460 square panel. The proven ESP display
 * port owns 180-degree pixel rotation and LVGL owns pointer rotation once. */
#include "meoave_ui.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "fls2_display_diag.h"
#define perf_now_us() esp_timer_get_time()
#else
/* Desktop render harness: it measures pixels, not time. */
#define perf_now_us() ((int64_t)0)
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

LV_FONT_DECLARE(meo_font_20);
LV_FONT_DECLARE(meo_font_28);
LV_FONT_DECLARE(meo_font_44);

#define BG       0x000000 /* One quiet black canvas across every page. */
#define CARD     0x151515
#define WHITE    0xF5F5F5 /* primary ink; retained name for existing call sites */
#define MUTED    0xA8A8A8
#define DIM      0x666666
#define ACCENT_BLUE   0x78B5FF
#define ACCENT_CYAN   0x78E4DE
#define ACCENT_IVORY  0xFFF1DB
#define ACCENT_AMBER  0xFFCA78
#define TILE_BLUE     0x10151E
#define TILE_IVORY    0x181613
#define TILE_CYAN     0x101A1C
#define HOME_BG  0x000000
#define HOME_EYE 0xFFFFFF
#define EYE_POINTS 17
#define MIC_SEGS 13   /* segments per microphone channel */
#define PETALS   6
#define PI       3.14159265f

/* The 460x460 panel is a high-density ~2-inch display.  Do not treat a
 * logical pixel like a desktop CSS pixel: user-facing text below 28 px and
 * controls below 64 px become visibly small at the normal viewing distance.
 * These tokens keep the product UI deliberately generous without changing
 * the proven 460px square logical canvas. */
#define UI_BODY_FONT       (&meo_font_28)
/* Settings are read at arm's length on the small physical panel. Keep their
 * names AND real states at 28px; do not shrink states to fit a crowded row. */
#define UI_SETTINGS_FONT   (&meo_font_28)
#define UI_BREATH_CX       230
#define UI_BREATH_CY       230
#define UI_BREATH_SIZE     320
#define APP_COUNT          3
#define GALLERY_COUNT      (APP_COUNT * 3)
#define GALLERY_VISIBLE    2
#define GALLERY_TILE_W     158
#define GALLERY_TILE_H     158
#define GALLERY_STEP       190
#define GALLERY_MARGIN     56
#define BACK_DRAG_SLOP     12
#define BACK_COMMIT_PX     80

enum { ACT_HOME=1, ACT_APPS, ACT_SETTINGS, ACT_WIFI, ACT_BREATHE,
       ACT_DISPLAY, ACT_ACCESSIBILITY, ACT_DIM, ACT_BRIGHT,
       ACT_SLOT_LEFT, ACT_SLOT_RIGHT, ACT_WIFI_SCAN,
       ACT_WIFI_AP0, ACT_WIFI_AP1, ACT_SPEAKER, ACT_VOL_DOWN, ACT_VOL_UP,
       ACT_SPK_TEST, ACT_AUDIO_LOOP,
       ACT_PWD_KEY0 = 64,                 /* +0..9 digits, +10 delete, +11 ok */
       ACT_PWD_MODE = 80, ACT_PWD_EYE, ACT_MIC, ACT_MIC_TEST };
#define WIFI_ROWS 2
typedef struct {
    lv_obj_t *rail, *tiles[GALLERY_COUNT], *captions[GALLERY_VISIBLE];
    lv_obj_t *titles[GALLERY_VISIBLE], *dots[APP_COUNT];
    const char *const *names;
    const int *actions;
    unsigned first;
    bool moving, captions_ready, recentering;
} gallery_t;
#define PWD_MAX        64
#define PWD_KEY_COUNT  12
#define PWD_TAP_MS     700
enum { PWD_ABC = 0, PWD_UPPER, PWD_DIGIT, PWD_SYM };
static const char *const pwd_t9[10] = {"", "", "abc", "def", "ghi", "jkl",
                                       "mno", "pqrs", "tuv", "wxyz"};
static const char pwd_sym[10] = {'_', '-', '.', '@', '!', '#', '$', '%', '&', '*'};
static const int pwd_key_digit[PWD_KEY_COUNT] = {1,2,3,4,5,6,7,8,9,0,-1,-2};
static struct {
    char ssid[33];
    char text[PWD_MAX + 1];
    unsigned len;
    int mode;
    bool mask;
    int last_key;
    int tap;
    uint64_t last_ms;
} s_pwd = {.mode = PWD_ABC, .mask = true, .last_key = -1};


static struct {
    meo_model_t model;
    meo_ui_config_t config;
    lv_obj_t *pages[MEO_PAGE_COUNT], *eye[2], *pupil[2];
    lv_point_precise_t eye_points[2][EYE_POINTS];
    lv_obj_t *brightness, *slider, *save_labels[3], *motion, *brightness_summary, *motion_summary;
    /* Speaker page: big percentage, slider, and the summary on the settings row. */
    lv_obj_t *volume, *volume_slider, *volume_summary;
    /* Live acoustic self-test readout, updated only when the text changes. */
    lv_obj_t *audio_result, *audio_meta;
    char audio_result_text[40], audio_meta_text[40];
    /* Status strip on the home canvas: Wi-Fi mark, percentage, battery gauge. */
    lv_obj_t *wifi_mark, *power_text, *power_gauge, *power_fill;
    lv_obj_t *wifi_summary, *wifi_page_mark, *wifi_page_state, *wifi_page_ssid;
    lv_obj_t *wifi_rows[WIFI_ROWS], *wifi_row_ssid[WIFI_ROWS], *wifi_row_rssi[WIFI_ROWS];
    lv_obj_t *wifi_hint, *wifi_notice, *wifi_scan_label;
    lv_obj_t *pwd_field, *pwd_text, *pwd_ssid, *pwd_mode, *pwd_eye;
    lv_obj_t *pwd_keys[12], *pwd_key_labels[12];
    lv_obj_t *mic_seg[2][MIC_SEGS], *mic_db[2], *mic_state;
    lv_obj_t *mic_switch, *mic_knob;
    int8_t mic_lit[2][MIC_SEGS];   /* -1 = not painted yet */
    int16_t mic_db_shown[2];
    int last_rate_tenths;          /* -1 = label cleared */
    float mic_target[2], mic_shown[2];
    char wifi_ap_ssid[WIFI_ROWS][33];
    unsigned wifi_ap_visible;
    lv_obj_t *shutdown_layer, *shutdown_number, *shutdown_title, *shutdown_arc, *shutdown_hint, *shutdown_glow;
    int battery_percent;
    bool charging;
    meo_wifi_state_t wifi_state;
    gallery_t apps;
    lv_obj_t *bloom, *number;
    float bloom_level;
    bool bloom_live;
    uint32_t frames;
    uint32_t last_tick;
    uint64_t clock_ms;
    lv_point_t gesture_start;
    meo_page_t gesture_page;
    lv_indev_t *gesture_indev;
    int back_offset;
    bool pointer_down, back_candidate, back_dragging, back_settling, back_commit;
    bool navigating, tap_moved, gallery_press_moving, press_blocked;
    bool ready;
} ui;

static int s_key_number_shown = -1;   /* last digit pushed to the overlay */
static bool s_overlay_up;             /* shutdown screen is the active screen */
static uint64_t s_notice_hide_at;     /* 0 = sticky; else hide when clock passes */
static uint32_t s_wifi_ink = 0xFFFFFFFFu;  /* last Wi-Fi mark colour painted */
static lv_color_t color(uint32_t c) { return lv_color_hex(c); }

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width,
                       const lv_font_t *font, uint32_t ink, bool center)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    if (width) lv_obj_set_width(o, width);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, color(ink), 0);
    lv_obj_set_style_text_align(o, center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(o, 6, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* LVGL 9 label setters always free/reallocate the string and invalidate the
 * whole widget - even when the text did not change. Status producers poll at
 * 1-2 Hz and gallery captions update on every scroll event, so without this
 * guard a page that should be perfectly static repaints several labels per
 * second. Only call the setter when the rendered bytes actually differ. */
static void label_text_if_changed(lv_obj_t *o, const char *text)
{
    if (!o || !text) return;
    const char *cur = lv_label_get_text(o);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(o, text);
}

/* Icons are native draw primitives, not private-use text glyphs. Keep their
 * geometry inside a 32-unit canvas and render at the requested pixel size;
 * never scale a label through a temporary transformed layer. USER_1 marks
 * passive icon objects for real-render regression checks. */
typedef enum { ICON_LEFT=1, ICON_RIGHT, ICON_WIFI, ICON_SUN, ICON_EYE,
               ICON_MINUS, ICON_PLUS, ICON_GEAR, ICON_FACE, ICON_FLOWER,
               ICON_SPEAKER } icon_kind_t;

typedef struct {
    lv_layer_t *layer;
    lv_area_t area;
    lv_color_t ink;
    int size, stroke;
} icon_draw_t;

static lv_point_precise_t icon_point(const icon_draw_t *d, float x, float y)
{
    return (lv_point_precise_t){d->area.x1+x*d->size/32.0f,
                                d->area.y1+y*d->size/32.0f};
}

static void icon_line(const icon_draw_t *d, float x1,float y1,float x2,float y2)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.p1=icon_point(d,x1,y1); line.p2=icon_point(d,x2,y2);
    line.color=d->ink; line.opa=LV_OPA_COVER; line.width=d->stroke;
    line.round_start=line.round_end=1;
    lv_draw_line(d->layer,&line);
}

static void icon_arc(const icon_draw_t *d, float x,float y,float radius,int start,int end)
{
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    lv_point_precise_t center=icon_point(d,x,y);
    arc.center=(lv_point_t){(int32_t)center.x,(int32_t)center.y};
    arc.radius=(uint16_t)lroundf(radius*d->size/32.0f);
    arc.width=d->stroke; arc.color=d->ink; arc.opa=LV_OPA_COVER;
    arc.rounded=1; arc.start_angle=start; arc.end_angle=end;
    lv_draw_arc(d->layer,&arc);
}

/* Launcher mark: six opaque, curved, variable-width strokes. Rounded
 * segments overlap within each leaf, so the software rasterizer cannot expose
 * triangle-AA seams. No alpha-circle pile, texture, vector engine or image
 * allocation. The breathing page uses its own softer overlapping circles. */
static lv_point_precise_t petal_point(const icon_draw_t *d, float x,float y,float angle)
{
    return icon_point(d,16+x*cosf(angle)-y*sinf(angle),
                        16+x*sinf(angle)+y*cosf(angle));
}

static void draw_bloom(const icon_draw_t *source, float opening,float twist)
{
    icon_draw_t d=*source;
    d.size=(int)lroundf(source->size*(.72f+.28f*opening));
    d.area.x1+=(source->size-d.size)/2;
    d.area.y1+=(source->size-d.size)/2;
    for (int n=0; n<PETALS; ++n) {
        float angle=-PI/2+n*PI/3+twist;
        /* Keep each leaf solid and clean, with three related lightnesses.
           Fake per-segment gradients look banded on this integer renderer. */
        lv_color_t leaf=n%3==0 ? lv_color_mix(color(0xEAFFF7),d.ink,55) :
                        n%3==1 ? d.ink : lv_color_mix(d.ink,color(0x286D89),190);
        float root=2.2f+1.2f*opening, reach=13.3f+.6f*opening-root;
        /* 12 overlapping strokes instead of 32: each stroke is ~6 px wide and
           the petal spans only ~11 px, so the outline is indistinguishable
           while this icon costs 2.7x less to rasterise. Full detail is drawn
           again as soon as a screen transition settles. */
        for (int step=0; step<12; ++step) {
            float a=step/12.0f, b=(step+1)/12.0f, mid=(a+b)*.5f;
            lv_draw_line_dsc_t stroke; lv_draw_line_dsc_init(&stroke);
            stroke.p1=petal_point(&d,root+reach*a,1.25f*sinf(a*PI),angle);
            stroke.p2=petal_point(&d,root+reach*b,1.25f*sinf(b*PI),angle);
            stroke.width=LV_MAX(2,(int)lroundf(d.size*6.8f*sinf(mid*PI)/32));
            stroke.color=leaf;
            stroke.opa=LV_OPA_COVER;
            stroke.round_start=stroke.round_end=1;
            lv_draw_line(d.layer,&stroke);
        }
    }
}

static void draw_breath_bloom(lv_event_t *event)
{
    lv_obj_t *o=lv_event_get_target_obj(event);
    lv_area_t bounds; lv_obj_get_coords(o,&bounds);
    lv_layer_t *layer=lv_event_get_layer(event);
    float level=ui.bloom_level;
    int cx=(bounds.x1+bounds.x2)/2, cy=(bounds.y1+bounds.y2)/2;
    /* Two sets of soft, translucent circles share the same center. Their
       orbit and size follow the actual signal; no autonomous oscillator. */
    static const uint32_t shades[6]={0x5DDCFA,0x70BBFF,0x9CA3FF,
                                     0xCA9DF6,0x82B6FF,0x5ED9EE};
    float turn=-PI/2+(level-.5f)*.14f;
    for (int layer_index=0; layer_index<2; ++layer_index) {
        int radius=(int)((layer_index ? 35+level*8 : 51+level*12)*1.25f);
        float orbit=(layer_index ? 12+level*17 : 24+level*20)*1.25f;
        for (int n=0; n<6; ++n) {
            float angle=turn+n*PI/3+(layer_index ? PI/6 : 0);
            int x=cx+(int)lroundf(cosf(angle)*orbit);
            int y=cy+(int)lroundf(sinf(angle)*orbit);
            lv_area_t circle={x-radius,y-radius,x+radius,y+radius};
            lv_draw_rect_dsc_t dsc; lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color=ui.bloom_live ? color(shades[n]) :
                         lv_color_mix(color(shades[n]),color(BG),155);
            dsc.bg_opa=layer_index ? 56 : 73;
            dsc.radius=LV_RADIUS_CIRCLE;
            lv_draw_rect(layer,&dsc,&circle);
        }
    }
}

/* Level-of-detail switch. A screen transition redraws BOTH screen trees on
 * every frame; a single ICON_FLOWER is 192 antialiased line draws, ICON_EYE
 * 24, the Wi-Fi mark four arcs. On apps/settings that measured 80-90 ms per
 * frame (13-15 fps). During the 180 ms swipe a soft dot reads the same at
 * speed and keeps rasterisation under a millisecond. */
#define ICON_REGISTRY_MAX 96
#define ICON_RESTORE_BATCH 8
static lv_obj_t *s_icons[ICON_REGISTRY_MAX];
static unsigned s_icon_count;
static unsigned s_icon_restore_at;
static bool     s_icon_restore_active, s_icon_restore_pending;
static uint64_t s_lod_until_ms;        /* safety timeout for a lost event */
static uint64_t s_lod_restore_at_ms;

/* Enter low detail for the whole transition. The flag lives on each icon so
   restoring can be spread over several frames. */
static void icon_lod_begin(void)
{
#ifdef ESP_PLATFORM
    /* Desktop visual regression renders pixel-exact screenshots immediately
       after a navigation; low-detail dots would break its icon assertions. */
    for (unsigned i=0; i<s_icon_count; ++i)
        if (s_icons[i]) lv_obj_add_flag(s_icons[i], LV_OBJ_FLAG_USER_3);
#endif
    s_icon_restore_active = s_icon_restore_pending = false;
    s_icon_restore_at = 0;
}

/* LVGL invalidates the whole screen when a load animation completes. Start
   restoring full detail only AFTER that frame, so it still draws the cheap
   icons; otherwise the settle frame costs ~40 ms on the launcher page. */
static void icon_lod_restore(void)
{
    s_icon_restore_pending = true;
    s_lod_restore_at_ms = ui.clock_ms + 60;
}

/* One small batch per tick: instead of one 40-70 ms "restore the whole page"
   frame, eight 28x28 icon squares are repainted each frame (~1 ms). */
static void icon_lod_service(void)
{
    if (s_icon_restore_pending) {
        if (ui.clock_ms < s_lod_restore_at_ms) return;
        s_icon_restore_pending = false;
        s_icon_restore_active = true;
        s_icon_restore_at = 0;
    }
    if (!s_icon_restore_active) {
        if (s_lod_until_ms && ui.clock_ms > s_lod_until_ms) {
            s_lod_until_ms = 0;       /* SCREEN_LOADED never arrived */
            icon_lod_restore();
        }
        return;
    }
    unsigned n = 0;
    while (s_icon_restore_at < s_icon_count && n < ICON_RESTORE_BATCH) {
        lv_obj_t *o = s_icons[s_icon_restore_at++];
        if (o && lv_obj_has_flag(o, LV_OBJ_FLAG_USER_3)) {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_USER_3);
            lv_obj_invalidate(o);
        }
        ++n;
    }
    if (s_icon_restore_at >= s_icon_count) s_icon_restore_active = false;
}

static void draw_icon(lv_event_t *event)
{
    lv_obj_t *o=lv_event_get_target_obj(event);
    icon_draw_t d={.layer=lv_event_get_layer(event),
        .ink=lv_obj_get_style_line_color(o,0), .size=lv_obj_get_width(o)};
    lv_obj_get_coords(o,&d.area);
    d.stroke=(d.size*3+16)/32;
    if (d.stroke<2) d.stroke=2;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_3)) {
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color=d.ink; dot.bg_opa=LV_OPA_COVER; dot.radius=LV_RADIUS_CIRCLE;
        int r=LV_MAX(3,d.size/8);
        int cx=(d.area.x1+d.area.x2)/2, cy=(d.area.y1+d.area.y2)/2;
        lv_area_t a={cx-r,cy-r,cx+r,cy+r};
        lv_draw_rect(d.layer,&dot,&a);
        return;
    }
    switch ((icon_kind_t)(uintptr_t)lv_obj_get_user_data(o)) {
    case ICON_LEFT:
        icon_line(&d,21,6,11,16); icon_line(&d,11,16,21,26); break;
    case ICON_RIGHT:
        icon_line(&d,11,6,21,16); icon_line(&d,21,16,11,26); break;
    case ICON_WIFI:
        icon_arc(&d,16,28,22,225,315);
        icon_arc(&d,16,28,14,225,315);
        icon_arc(&d,16,28,6,225,315);
        icon_arc(&d,16,28,2,0,360); break;
    case ICON_SUN:
        icon_arc(&d,16,16,7,0,360);
        for (int i=0;i<8;++i) {
            float a=i*PI/4;
            icon_line(&d,16+cosf(a)*11,16+sinf(a)*11,
                         16+cosf(a)*14,16+sinf(a)*14);
        }
        break;
    case ICON_EYE:
        for (int i=0;i<12;++i) {
            float a=i*PI/12, b=(i+1)*PI/12;
            icon_line(&d,3+26.0f*i/12,16-8*sinf(a),3+26.0f*(i+1)/12,16-8*sinf(b));
            icon_line(&d,3+26.0f*i/12,16+8*sinf(a),3+26.0f*(i+1)/12,16+8*sinf(b));
        }
        icon_arc(&d,16,16,4,0,360); break;
    case ICON_MINUS: case ICON_PLUS:
        icon_line(&d,7,16,25,16);
        if ((icon_kind_t)(uintptr_t)lv_obj_get_user_data(o)==ICON_PLUS)
            icon_line(&d,16,7,16,25);
        break;
    case ICON_GEAR:
        d.stroke=LV_MAX(2,d.size*5/32);
        for (int n=0; n<8; ++n) {
            float a=n*PI/4;
            icon_line(&d,16+cosf(a)*9,16+sinf(a)*9,
                         16+cosf(a)*12,16+sinf(a)*12);
        }
        d.stroke=LV_MAX(2,d.size*5/32);
        icon_arc(&d,16,16,10,0,360); break;
    case ICON_FACE:
        /* A friendly, solid smile-eye mark, not two generic pill containers.
         * Warm ivory belongs to the App mark only. Home remains black/white. */
        d.stroke=LV_MAX(2,d.size*5/32);
        for (int eye=0; eye<2; ++eye) {
            float x=eye ? 19 : 3, arch=eye ? 4.8f : 4.0f;
            for (int step=0; step<12; ++step) {
                float a=step/12.0f, b=(step+1)/12.0f;
                icon_line(&d,x+10*a,19-arch*sinf(a*PI),
                             x+10*b,19-arch*sinf(b*PI));
            }
        }
        break;
    case ICON_FLOWER:
        draw_bloom(&d,1,-.10f); break;
    case ICON_SPEAKER:
        /* Body + cone on the left, sound waves drawn as three short strokes.
           Deliberately arc-free: the page shows this icon twice at once, and
           eight lv_draw_arc masks in one frame was enough to exhaust the
           render buffers on the board (the page reset the chip on entry). */
        icon_line(&d,3,13,8,13);
        icon_line(&d,8,13,14,6);
        icon_line(&d,14,6,14,26);
        icon_line(&d,14,26,8,19);
        icon_line(&d,8,19,3,19);
        icon_line(&d,3,19,3,13);
        icon_line(&d,18,13,18,19);
        icon_line(&d,22,10,22,22);
        icon_line(&d,26,7,26,25);
        break;
    }
}

static lv_obj_t *icon(lv_obj_t *parent, icon_kind_t kind,int size,uint32_t ink)
{
    lv_obj_t *o=box(parent,0,0,size,size,0,0);
    lv_obj_set_style_bg_opa(o,LV_OPA_TRANSP,0);
    lv_obj_set_style_line_color(o,color(ink),0);
    lv_obj_add_flag(o,LV_OBJ_FLAG_USER_1);
    lv_obj_set_user_data(o,(void *)(uintptr_t)kind);
    lv_obj_add_event_cb(o,draw_icon,LV_EVENT_DRAW_MAIN,NULL);
    if (s_icon_count < ICON_REGISTRY_MAX) s_icons[s_icon_count++]=o;
    return o;
}

static void action(lv_event_t *event);
void meo_ui_fps_start(uint32_t window_ms, const char *tag);
static void warmup_abort(void);
static void track_pointer_motion(lv_event_t *event);
static void update_breathe(void);
static void shutdown_create(void);
static void anim_text_opa(void *obj, int32_t value);
static void glow_tick(void);
static void mic_switch_paint(bool on);
static void pwd_tap_digit(int digit, uint64_t now);
static void pwd_backspace(void);
static void pwd_submit(void);
static void pwd_mode_paint(void);
static void pwd_render(void);
static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h,
                        uint32_t bg, const char *text, int id)
{
    lv_obj_t *o = box(parent, x, y, w, h, bg, h / 2);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_bg_color(o, color(0x2A2A2A), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(o, 2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(o, action, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_add_event_cb(o, track_pointer_motion, LV_EVENT_PRESSING, NULL);
    if (text) {
        lv_obj_t *t = label(o, text, 0, 0, w, UI_BODY_FONT, WHITE, true);
        lv_obj_center(t);
    }
    return o;
}


/* The font is a curated subset, but SSIDs are arbitrary UTF-8. A missing glyph
 * renders as a placeholder box, which looks like a bug; detect it and fall back
 * to a generated label instead of drawing boxes. */
static bool text_renderable(const char *text, const lv_font_t *font)
{
    if (!text) return false;
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        uint32_t cp;
        unsigned char b = *s++;
        if (b < 0x80) cp = b;
        else if ((b & 0xe0) == 0xc0) {
            if (!s[0]) return false;
            cp = ((uint32_t)(b & 0x1f) << 6) | (s[0] & 0x3f);
            s += 1;
        } else if ((b & 0xf0) == 0xe0) {
            if (!s[0] || !s[1]) return false;
            cp = ((uint32_t)(b & 0x0f) << 12) | ((uint32_t)(s[0] & 0x3f) << 6) | (s[1] & 0x3f);
            s += 2;
        } else return false;   /* 4-byte planes are outside this font */
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0) || dsc.is_placeholder) return false;
    }
    return true;
}

static lv_obj_t *header(lv_obj_t *page, const char *name, int back)
{
    /* One generous target: arrow, title AND surrounding whitespace return.
       Reuse the existing title band instead of adding another toolbar. */
    lv_obj_t *b = button(page, 32, 32, 396, 80, CARD, NULL, back);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_translate_y(b, 0, LV_STATE_PRESSED);
    lv_obj_t *chevron = icon(b, ICON_LEFT, 40, WHITE);
    lv_obj_set_pos(chevron, 12, 20);
    if (!name || !*name) return NULL;
    lv_obj_t *title = label(b, name, 72, 0, 288, &meo_font_28, WHITE, false);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 72, 0);
    return title;
}

static void save_status(const char *text)
{
    for (unsigned i=0;i<3;i++) if (ui.save_labels[i]) lv_label_set_text(ui.save_labels[i],text);
}

static void changed(void)
{
    if (ui.config.settings_changed) ui.config.settings_changed(&ui.model.settings);
    save_status(ui.config.settings_changed ? "正在保存设置" : "本次生效 · 尚未保存");
}

static void refresh_settings(void)
{
    lv_label_set_text_fmt(ui.brightness, "%u%%", ui.model.settings.brightness);
    lv_label_set_text_fmt(ui.brightness_summary, "%u%%", ui.model.settings.brightness);
    lv_label_set_text(ui.motion_summary, ui.model.settings.reduced_motion ? "减少" : "标准");
    if (ui.volume) lv_label_set_text_fmt(ui.volume, "%u%%", ui.model.settings.volume);
    if (ui.volume_summary) lv_label_set_text_fmt(ui.volume_summary, "%u%%", ui.model.settings.volume);
}

static void gallery_present(gallery_t *gallery, bool animate);

static gallery_t *current_gallery(void)
{
    if (ui.model.page==MEO_APPS) return &ui.apps;
    return NULL;
}

static void refresh_apps(void)
{
    gallery_t *g=&ui.apps;
    if (!g->rail) return;
    if (ui.model.settings.reduced_motion)
        lv_obj_remove_flag(g->rail, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    else lv_obj_add_flag(g->rail, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    if (!lv_obj_is_scrolling(g->rail)) gallery_present(g,false);
}

void meo_ui_navigate(meo_page_t page)
{
    if (s_overlay_up) return;   /* countdown owns the whole panel */
    if (!ui.ready || page < MEO_HOME || page >= MEO_PAGE_COUNT || page == ui.model.page ||
        ui.navigating || ui.back_dragging || ui.back_settling) return;
    /* An early tap during bring-up wins: drop the warm-up veil immediately. */
    warmup_abort();
    /* Defensive: a NULL slot must never reach lv_screen_load_anim(). */
    if (!ui.pages[page]) return;
    meo_page_t previous = ui.model.page;
    ui.model.page = page;
    if (page==MEO_BREATHE) update_breathe();
    /* Entering the network page asks the radio for a fresh scan; the callback
       only queues a request, so this never blocks the GUI task. */
    if (page==MEO_WIFI && ui.config.wifi_scan) ui.config.wifi_scan();
    ui.pointer_down = ui.back_candidate = false;
    bool back = page == meo_page_parent(previous);
    ui.navigating = true;
    char tag[16];
    snprintf(tag, sizeof(tag), "nav-%s", meo_ui_page_name(page));
    /* Sample the 180 ms swipe plus its settle frame, nothing more: a stray
       repaint 100 ms later must not be charged against the animation. */
    meo_ui_fps_start(ui.model.settings.reduced_motion ? 120 : 220, tag);
    if (!ui.model.settings.reduced_motion) {
        icon_lod_begin();
        s_lod_until_ms = ui.clock_ms + 400;
    }
    lv_screen_load_anim(ui.pages[page], ui.model.settings.reduced_motion ? LV_SCR_LOAD_ANIM_NONE
                        : back ? LV_SCR_LOAD_ANIM_MOVE_RIGHT : LV_SCR_LOAD_ANIM_MOVE_LEFT,
                        ui.model.settings.reduced_motion ? 0 : 180, 0, false);
}

static void screen_loaded(lv_event_t *event)
{
    if (lv_event_get_target_obj(event)==ui.pages[ui.model.page]) {
        ui.navigating=false;
        icon_lod_restore();
        s_lod_until_ms=0;
    }
}

static void update_tap_distance(lv_indev_t *indev)
{
    if (indev!=ui.gesture_indev) return;
    lv_point_t p; lv_indev_get_point(indev,&p);
    int dx=p.x-ui.gesture_start.x, dy=p.y-ui.gesture_start.y;
    /* Latched maximum excursion: dragging back to the origin is not a tap. */
    if (dx>12 || dx< -12 || dy>12 || dy< -12) ui.tap_moved=true;
}

static void draw_back_parent(lv_event_t *event)
{
    if ((!ui.back_dragging && !ui.back_settling) || ui.model.settings.reduced_motion) return;
    lv_layer_t *layer = lv_event_get_layer(event);
    int32_t exposed = ui.back_offset;
    if (exposed <= 0) return;
    lv_area_t old_clip = layer->_clip_area;
    lv_area_t strip = old_clip;
    strip.x1 = LV_MAX(strip.x1, 0);
    strip.x2 = LV_MIN(strip.x2, exposed - 1);
    if (strip.x1 <= strip.x2) {
        layer->_clip_area = strip;
        lv_draw_rect_dsc_t background;
        lv_draw_rect_dsc_init(&background);
        background.bg_color = lv_color_hex(meo_page_parent(ui.gesture_page) == MEO_HOME ? HOME_BG : BG);
        background.bg_opa = LV_OPA_COVER;
        lv_area_t full = {0, 0, 459, 459};
        lv_draw_rect(layer, &background, &full);
        lv_obj_t *parent = ui.pages[meo_page_parent(ui.gesture_page)];
        int32_t parent_x = exposed - 460;
        lv_obj_set_x(parent, parent_x);
        lv_obj_update_layout(parent);
        for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
            lv_obj_redraw(layer, lv_obj_get_child(parent, (int32_t)i));
        lv_obj_set_x(parent, 0);
        layer->_clip_area = old_clip;
    }
}

static void invalidate_back_frame(void)
{
    /* Translating the active screen exposes a strip that is not covered by
       the active screen's new coordinates. Invalidate the complete panel so
       the bottom-layer parent preview is rendered there as well, including
       rounded-card antialias pixels. */
    lv_area_t full = {0, 0, 459, 459};
    lv_obj_invalidate_area(lv_layer_bottom(), &full);
}

static void back_preview_begin(void)
{
    invalidate_back_frame();
}

static void back_preview_clear(void)
{
    invalidate_back_frame();
}

static void back_set_offset(void *object, int32_t offset)
{
    (void)object;
    ui.back_offset = offset;
    if (!ui.model.settings.reduced_motion) {
        lv_obj_set_x(ui.pages[ui.gesture_page], offset);
        lv_obj_invalidate(ui.pages[ui.gesture_page]);
        invalidate_back_frame();
    }
}

static void back_finish(lv_anim_t *animation)
{
    (void)animation;
    meo_page_t from = ui.gesture_page;
    bool commit = ui.back_commit;
    /* Restore the old screen before re-entry; a cancelled drag must also
       leave no translated objects or active parent-preview surface. */
    lv_obj_set_x(ui.pages[from], 0);
    ui.back_dragging = ui.back_settling = ui.back_candidate = false;
    ui.back_offset = 0;
    if (commit) {
        ui.model.page = meo_page_parent(from);
        ui.navigating = true;
        lv_screen_load(ui.pages[ui.model.page]);
    }
    else {
        back_preview_clear();
    }
    invalidate_back_frame();
    lv_obj_invalidate(lv_screen_active());
}

static void back_release(lv_indev_t *indev)
{
    lv_point_t point; lv_indev_get_point(indev, &point);
    int dx = point.x-ui.gesture_start.x, dy = point.y-ui.gesture_start.y;
    ui.back_commit = dx >= BACK_COMMIT_PX && dx > abs(dy)*2;
    ui.back_dragging = false;
    ui.back_settling = true;
    ui.pointer_down = ui.back_candidate = false;
    /* Consume this release before the source object's class can synthesize
       a click/toggle. A fresh touch during settling is separately rejected. */
    lv_indev_stop_processing(indev);
    lv_obj_t *target = lv_indev_get_active_obj();
    if (target) lv_obj_remove_state(target, LV_STATE_PRESSED);
    if (ui.model.settings.reduced_motion || (!ui.back_commit && ui.back_offset == 0)) {
        back_finish(NULL);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ui.pages[ui.gesture_page]);
    lv_anim_set_exec_cb(&anim, back_set_offset);
    lv_anim_set_values(&anim, ui.back_offset, ui.back_commit ? 460 : 0);
    lv_anim_set_duration(&anim, ui.back_commit ? 160 : 120);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, back_finish);
    lv_anim_start(&anim);
}

static void track_back_drag(lv_indev_t *indev)
{
    if (s_overlay_up) return;
    if (indev != ui.gesture_indev || !ui.back_candidate || ui.navigating ||
        ui.back_settling || ui.gesture_page != ui.model.page) return;
    lv_point_t point; lv_indev_get_point(indev, &point);
    int dx = point.x-ui.gesture_start.x, dy = point.y-ui.gesture_start.y;
    if (!ui.back_dragging) {
        if (abs(dx) <= BACK_DRAG_SLOP && abs(dy) <= BACK_DRAG_SLOP) return;
        /* Axis is chosen once. Vertical/left travel cannot turn into an
           accidental back later; recognises slow drags without velocity. */
        if (dx <= BACK_DRAG_SLOP || dx <= abs(dy)*2 || lv_indev_get_scroll_obj(indev)) {
            ui.back_candidate = false;
            return;
        }
        ui.back_dragging = true;
        lv_obj_t *target = lv_indev_get_active_obj();
        if (target) lv_obj_remove_state(target, LV_STATE_PRESSED);
        lv_obj_update_layout(ui.pages[meo_page_parent(ui.gesture_page)]);
        back_preview_begin();
    }
    back_set_offset(NULL, dx < 0 ? 0 : dx > 460 ? 460 : dx);
}

static bool owns_horizontal_input(lv_obj_t *target)
{
    for (lv_obj_t *object=target; object; object=lv_obj_get_parent(object)) {
        if (lv_obj_check_type(object, &lv_slider_class) || lv_obj_check_type(object, &lv_switch_class))
            return true;
        if (lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) &&
            (lv_obj_get_scroll_dir(object) & LV_DIR_HOR)) return true;
    }
    return false;
}

static void track_pointer_motion(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev=lv_indev_active();
    if (indev && ui.pointer_down) {
        update_tap_distance(indev);
        /* LVGL delivers PRESSING to objects, not to the indev callback. */
        track_back_drag(indev);
    }
}

static void pointer_navigation(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && !ui.pointer_down) {
        ui.pointer_down = true;
        ui.tap_moved = false;
        ui.press_blocked = ui.navigating || ui.back_settling;
        /* Latch at press time, not release: holding a moving card until it
           settles must never turn a scroll-interruption into an app launch. */
        gallery_t *gallery=current_gallery();
        ui.gallery_press_moving = gallery && (gallery->moving || !gallery->captions_ready ||
                                               lv_obj_is_scrolling(gallery->rail));
        ui.gesture_indev = indev;
        lv_indev_get_point(indev,&ui.gesture_start); /* Already rotated by LVGL. */
        ui.gesture_page = ui.model.page;
        ui.back_candidate = !ui.press_blocked && ui.model.page != MEO_HOME &&
                            ui.model.page != MEO_APPS &&
                            !owns_horizontal_input(lv_indev_get_active_obj());
        if (ui.press_blocked) {
            lv_indev_stop_processing(indev);
            lv_indev_wait_release(indev);
        }
    } else if (indev == ui.gesture_indev &&
               (code == LV_EVENT_RELEASED || code == LV_EVENT_GESTURE)) {
        update_tap_distance(indev);
        if (code == LV_EVENT_RELEASED) {
            track_back_drag(indev);
            if (ui.back_dragging) { back_release(indev); return; }
            ui.pointer_down = ui.back_candidate = false;
        }
    }
}

/* A scan can take ~10 s while associated; the notice must outlive it, and any
 * other tap message must clear itself instead of sticking forever. */
static bool s_notice_pulsing;     /* the infinite opacity animation is live */
static void notice_pulse(bool on)
{
    if (!ui.wifi_notice || s_notice_pulsing == on) return;
    s_notice_pulsing = on;
    lv_anim_delete(ui.wifi_notice, anim_text_opa);
    if (!on) { lv_obj_set_style_text_opa(ui.wifi_notice, LV_OPA_COVER, 0); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui.wifi_notice);
    lv_anim_set_exec_cb(&a, anim_text_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, 185);
    lv_anim_set_duration(&a, 620);
    lv_anim_set_playback_duration(&a, 620);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void wifi_notice_for(const char *text, uint32_t hold_ms)
{
    if (!ui.wifi_notice) return;
    if (text && *text) {
        label_text_if_changed(ui.wifi_notice, text);
        lv_obj_remove_flag(ui.wifi_notice, LV_OBJ_FLAG_HIDDEN);
        s_notice_hide_at = hold_ms ? ui.clock_ms + hold_ms : 0;
        notice_pulse(true);      /* motion proves the tap was received */
    } else {
        lv_obj_add_flag(ui.wifi_notice, LV_OBJ_FLAG_HIDDEN);
        s_notice_hide_at = 0;
        notice_pulse(false);
    }
}
static void wifi_notice(const char *text) { wifi_notice_for(text, 4000); }

static void action(lv_event_t *event)
{
    if (s_overlay_up) return;   /* no taps during the power-key countdown */
    lv_indev_t *indev=lv_indev_active();
    if (ui.navigating || ui.back_dragging || ui.back_settling || ui.press_blocked) return;
    /* A drag across an app/row is not a tap, including a non-edge swipe. */
    if (indev && lv_indev_get_type(indev)==LV_INDEV_TYPE_POINTER &&
        (ui.tap_moved || lv_indev_get_gesture_dir(indev)!=LV_DIR_NONE)) return;
    int id = (int)(uintptr_t)lv_event_get_user_data(event);
    if (id >= ACT_PWD_KEY0 && id <= ACT_PWD_KEY0 + PWD_KEY_COUNT - 1) {
        int index = id - ACT_PWD_KEY0;
        int digit = pwd_key_digit[index];
        if (digit >= 0) pwd_tap_digit(digit, ui.clock_ms);
        else if (digit == -1) pwd_backspace();
        else pwd_submit();
        return;
    }
    if (id == ACT_PWD_MODE) {
        s_pwd.mode = (s_pwd.mode + 1) & 3;
        s_pwd.last_key = -1;
        pwd_mode_paint();
        return;
    }
    if (id == ACT_PWD_EYE) {
        s_pwd.mask = !s_pwd.mask;
        lv_obj_set_style_line_color(ui.pwd_eye, color(s_pwd.mask ? WHITE : ACCENT_BLUE), 0);
        pwd_render();
        return;
    }
    gallery_t *gallery=current_gallery();
    if (gallery) {
        lv_obj_t *target = lv_event_get_target_obj(event);
        bool item_target=id==ACT_SLOT_LEFT || id==ACT_SLOT_RIGHT;
        int item=-1;
        for (unsigned i=0; i<GALLERY_COUNT; ++i) if (target==gallery->tiles[i]) {
            item_target=true; item=(int)i;
        }
        if (item_target) {
            if (ui.gallery_press_moving || gallery->moving || !gallery->captions_ready ||
                lv_obj_is_scrolling(gallery->rail)) return;
            if (abs(lv_obj_get_scroll_x(gallery->rail)-(int)gallery->first*GALLERY_STEP)>1) return;
            if (item>=0 && (item<(int)gallery->first || item>(int)gallery->first+1)) return;
            if (id==ACT_SLOT_LEFT || id==ACT_SLOT_RIGHT)
                id=gallery->actions[(gallery->first+(id==ACT_SLOT_RIGHT ? 1 : 0))%APP_COUNT];
        }
    }
    switch (id) {
    case ACT_HOME: meo_ui_navigate(MEO_HOME); break;
    case ACT_APPS: meo_ui_navigate(MEO_APPS); break;
    case ACT_SETTINGS: meo_ui_navigate(MEO_SETTINGS); break;
    case ACT_WIFI: meo_ui_navigate(MEO_WIFI); break;
    case ACT_BREATHE: meo_ui_navigate(MEO_BREATHE); break;
    case ACT_DISPLAY: meo_ui_navigate(MEO_DISPLAY); break;
    case ACT_ACCESSIBILITY: meo_ui_navigate(MEO_ACCESSIBILITY); break;
    case ACT_MIC: meo_ui_navigate(MEO_MIC); break;
    case ACT_SPEAKER: meo_ui_navigate(MEO_SPEAKER); break;
    case ACT_VOL_DOWN: case ACT_VOL_UP: {
        int value = ui.model.settings.volume + (id==ACT_VOL_DOWN ? -10 : 10);
        value = value < 0 ? 0 : value > 100 ? 100 : value;
        if (value==ui.model.settings.volume) break;
        ui.model.settings.volume = (uint8_t)value;
        lv_slider_set_value(ui.volume_slider, ui.model.settings.volume, LV_ANIM_OFF);
        refresh_settings(); changed(); break;
    }
    case ACT_SPK_TEST:
        /* Fire and forget: the adapter plays the tone on its own task. */
        if (ui.config.speaker_test) ui.config.speaker_test();
        break;
    case ACT_AUDIO_LOOP:
        /* Physical DAC->PA->air->MIC->ADC self test, run off the GUI task. */
        if (ui.config.audio_loop) ui.config.audio_loop();
        break;
    case ACT_MIC_TEST: {
        /* The switch is the single owner of capture: on = start, off = stop. */
        bool on = !ui.model.settings.mic_test;
        ui.model.settings.mic_test = on;
        mic_switch_paint(on);
        if (ui.config.mic_test) ui.config.mic_test(on);
        changed();
        break;
    }
    case ACT_DIM: case ACT_BRIGHT: {
        int value = ui.model.settings.brightness + (id==ACT_DIM ? -10 : 10);
        value = value < 15 ? 15 : value > 100 ? 100 : value;
        if (value==ui.model.settings.brightness) break;
        ui.model.settings.brightness = (uint8_t)value;
        lv_slider_set_value(ui.slider,ui.model.settings.brightness,LV_ANIM_OFF);
        refresh_settings(); changed(); break;
    }
    case ACT_WIFI_SCAN: {
        bool down = ui.wifi_state == MEO_WIFI_ERROR;
        wifi_notice_for(down ? "重试中" : "扫描中", 20000);
        bool ok = ui.config.wifi_scan ? ui.config.wifi_scan() : false;
        if (!ok) wifi_notice_for("Wi-Fi 未就绪", 5000);
        break;
    }
    case ACT_WIFI_AP0: case ACT_WIFI_AP1: {
        unsigned index = (unsigned)(id - ACT_WIFI_AP0);
        if (index < ui.wifi_ap_visible) {
            /* A tap must never look dead: say why nothing happened. */
            bool ok = ui.config.wifi_join && ui.config.wifi_join(ui.wifi_ap_ssid[index]);
            /* Success needs no notice: the state line above already turns into
               "Wi-Fi 连接中". Repeating it looked like doubled characters. */
            if (!ok) meo_ui_password_open(ui.wifi_ap_ssid[index]);
        }
        break;
    }
    }
}

static void settings_event(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (target == ui.motion) ui.model.settings.reduced_motion = lv_obj_has_state(target, LV_STATE_CHECKED);
    else if (target == ui.volume_slider) ui.model.settings.volume = (uint8_t)lv_slider_get_value(target);
    else {
        ui.model.settings.brightness = (uint8_t)lv_slider_get_value(target);
    }
    refresh_settings();
    refresh_apps();
    changed();
}

static void home_create(void)
{
    lv_obj_t *p = ui.pages[MEO_HOME];
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, action, LV_EVENT_CLICKED, (void *)(uintptr_t)ACT_APPS);
    /* The home page is only the face: two large white eyes and simple black pupils. */
    for (int i = 0; i < 2; ++i) {
        ui.eye[i] = lv_line_create(p);
        lv_obj_remove_style_all(ui.eye[i]);
        lv_obj_set_size(ui.eye[i], 140, 160);
        lv_obj_set_style_line_color(ui.eye[i], color(HOME_EYE), 0);
        lv_obj_set_style_line_opa(ui.eye[i], LV_OPA_COVER, 0);
        lv_obj_set_style_line_rounded(ui.eye[i], true, 0);
        lv_obj_remove_flag(ui.eye[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        ui.pupil[i] = box(ui.eye[i], 58, 68, 24, 24, HOME_BG, 12);
    }
    /* Status strip sits above the eyes and never covers the tap-to-apps area.
       Passive objects only: the page itself stays the single tap target. */
    ui.wifi_mark = icon(p, ICON_WIFI, 30, DIM);
    lv_obj_set_pos(ui.wifi_mark, 232, 44);
    ui.power_text = label(p, "--", 272, 40, 76, &meo_font_28, WHITE, true);
    ui.power_gauge = box(p, 356, 44, 56, 28, HOME_BG, 8);
    lv_obj_set_style_border_width(ui.power_gauge, 2, 0);
    lv_obj_set_style_border_color(ui.power_gauge, color(MUTED), 0);
    lv_obj_set_style_border_opa(ui.power_gauge, LV_OPA_COVER, 0);
    ui.power_fill = box(ui.power_gauge, 3, 3, 50, 22, WHITE, 5);
    box(p, 414, 53, 4, 10, MUTED, 2);
}

/* The icon rail moves; its two caption targets are siblings on the screen.
 * Names never scroll with cards. Only a settled, complete pair may launch. */
static void caption_opacity(void *object, int32_t opacity)
{ lv_obj_set_style_text_opa(object,(lv_opa_t)opacity,0); }

static void gallery_hide_captions(gallery_t *g)
{
    g->captions_ready=false;
    for (unsigned i=0; i<GALLERY_VISIBLE; ++i) {
        lv_anim_delete(g->titles[i],caption_opacity);
        caption_opacity(g->titles[i],LV_OPA_TRANSP);
    }
}

/* Color identifies the application, not the page canvas. Cloned carousel
 * items share one semantic palette; scrolling only attenuates edge peeks. */
static const uint32_t app_accents[APP_COUNT]={ACCENT_BLUE,ACCENT_IVORY,ACCENT_CYAN};
static const uint32_t app_surfaces[APP_COUNT]={TILE_BLUE,TILE_IVORY,TILE_CYAN};

static void gallery_position(gallery_t *g)
{
    int scroll=lv_obj_get_scroll_x(g->rail);
    for (unsigned i=0; i<GALLERY_COUNT; ++i) {
        int center=GALLERY_MARGIN+GALLERY_TILE_W/2+(int)i*GALLERY_STEP-scroll;
        int distance=LV_CLAMP(0,abs(center-230)-GALLERY_STEP/2,GALLERY_STEP);
        uint8_t strength=(uint8_t)(255-distance*100/GALLERY_STEP);
        lv_color_t bg=lv_color_mix(color(app_surfaces[i%APP_COUNT]),color(BG),strength);
        lv_obj_set_style_bg_color(g->tiles[i],bg,0);
        lv_obj_set_style_bg_color(g->tiles[i],lv_color_mix(color(app_accents[i%APP_COUNT]),bg,28),LV_STATE_PRESSED);
        lv_obj_set_style_border_color(g->tiles[i],lv_color_mix(color(app_accents[i%APP_COUNT]),color(BG),strength/5),0);
        lv_obj_set_style_line_color(lv_obj_get_child(g->tiles[i],0),
                                   lv_color_mix(color(app_accents[i%APP_COUNT]),color(BG),strength),0);
        lv_obj_set_style_translate_y(g->tiles[i],ui.model.settings.reduced_motion ? 0 : distance*8/GALLERY_STEP,0);
    }
}

static void gallery_present(gallery_t *g, bool animate)
{
    int scroll=lv_obj_get_scroll_x(g->rail);
    int first=(scroll+GALLERY_STEP/2)/GALLERY_STEP;
    first=LV_CLAMP(1,first,GALLERY_COUNT-GALLERY_VISIBLE-1);
    if (abs(scroll-first*GALLERY_STEP)>1) {
        gallery_hide_captions(g);
        return;
    }
    /* Identical copies keep both edge peeks populated. Rebase only after
       native momentum/snap has ended; never rewrite the live drag position. */
    int rebased=first<=1 ? first+APP_COUNT : first>=6 ? first-APP_COUNT : first;
    if (rebased!=first) {
        g->recentering=true;
        lv_obj_scroll_to_x(g->rail,rebased*GALLERY_STEP,LV_ANIM_OFF);
        g->recentering=false;
        first=rebased;
    }
    g->first=(unsigned)first;
    /* This pinned LVGL's START/SCROLL_ONE treats the current zero-distance
       snap as an edge and can predict two items with momentum. Restrict the
       eligible snap targets to the adjacent pair positions, not the drag. */
    for (unsigned i=0; i<GALLERY_COUNT; ++i) {
        if (i>=1 && i<=GALLERY_COUNT-GALLERY_VISIBLE-1 && abs((int)i-first)<=1)
            lv_obj_add_flag(g->tiles[i],LV_OBJ_FLAG_SNAPPABLE);
        else lv_obj_remove_flag(g->tiles[i],LV_OBJ_FLAG_SNAPPABLE);
    }
    gallery_position(g);
    for (unsigned i=0; i<GALLERY_VISIBLE; ++i) {
        label_text_if_changed(g->titles[i],g->names[(g->first+i)%APP_COUNT]);
        lv_anim_delete(g->titles[i],caption_opacity);
        if (animate && !ui.model.settings.reduced_motion) {
            lv_anim_t a; lv_anim_init(&a);
            lv_anim_set_var(&a,g->titles[i]);
            lv_anim_set_exec_cb(&a,caption_opacity);
            lv_anim_set_values(&a,0,255);
            lv_anim_set_duration(&a,160);
            lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
            lv_anim_start(&a);
        } else caption_opacity(g->titles[i],LV_OPA_COVER);
    }
    for (unsigned i=0; i<APP_COUNT; ++i)
        lv_obj_set_style_bg_color(g->dots[i],color(i==g->first%APP_COUNT ? app_accents[i] : DIM),0);
    g->captions_ready=true;
}

static void gallery_scroll(lv_event_t *event)
{
    gallery_t *g=lv_event_get_user_data(event);
    if (g->recentering) return;
    lv_event_code_t code=lv_event_get_code(event);
    if (code==LV_EVENT_SCROLL_BEGIN) {
        g->moving=true;
        gallery_hide_captions(g);
        lv_anim_t *anim=lv_event_get_param(event);
        if (anim) {
            lv_anim_set_duration(anim,ui.model.settings.reduced_motion ? 1 : 240);
            lv_anim_set_path_cb(anim,lv_anim_path_ease_out);
        }
    } else if (code==LV_EVENT_SCROLL) {
        gallery_hide_captions(g);
        gallery_position(g);
    } else if (code==LV_EVENT_SCROLL_END) {
        g->moving=false;
        gallery_present(g,true);
    }
}

static void gallery_create(lv_obj_t *page, gallery_t *g, int y, int caption_y,
                           const char *const *names, const int *actions,
                           const icon_kind_t *symbols)
{
    g->names=names; g->actions=actions;
    g->rail=box(page,0,y,460,190,BG,0);
    lv_obj_add_flag(g->rail,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_remove_flag(g->rail,LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(g->rail,LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(g->rail,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(g->rail,LV_SCROLL_SNAP_START);
    lv_obj_set_flex_flow(g->rail,LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(g->rail,GALLERY_MARGIN,0);
    lv_obj_set_style_pad_ver(g->rail,16,0);
    lv_obj_set_style_pad_column(g->rail,GALLERY_STEP-GALLERY_TILE_W,0);
    lv_obj_set_style_anim_duration(g->rail,240,0);
    for (unsigned i=0; i<GALLERY_COUNT; ++i) {
        g->tiles[i]=button(g->rail,0,0,GALLERY_TILE_W,GALLERY_TILE_H,0x1E1E1E,NULL,actions[i%APP_COUNT]);
        lv_obj_set_style_radius(g->tiles[i],56,0);
        lv_obj_set_style_border_width(g->tiles[i],1,0);
        lv_obj_set_style_border_opa(g->tiles[i],LV_OPA_COVER,0);
        lv_obj_set_style_translate_y(g->tiles[i],0,LV_STATE_PRESSED);
        /* Reserve a complete predecessor and successor for both edge peeks. */
        if (i>=1 && i<=GALLERY_COUNT-GALLERY_VISIBLE-1)
            lv_obj_add_flag(g->tiles[i],LV_OBJ_FLAG_SNAPPABLE);
        else lv_obj_remove_flag(g->tiles[i],LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_t *art=icon(g->tiles[i],symbols[i%APP_COUNT],128,WHITE);
        lv_obj_center(art);
    }
    for (unsigned i=0; i<GALLERY_VISIBLE; ++i) {
        g->captions[i]=button(page,43+i*GALLERY_STEP,caption_y,184,80,BG,NULL,
                              i ? ACT_SLOT_RIGHT : ACT_SLOT_LEFT);
        lv_obj_set_style_bg_opa(g->captions[i],LV_OPA_TRANSP,0);
        lv_obj_set_style_bg_opa(g->captions[i],LV_OPA_TRANSP,LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(g->captions[i],0,LV_STATE_PRESSED);
        g->titles[i]=label(g->captions[i],names[i],0,0,184,&meo_font_44,WHITE,true);
        lv_label_set_long_mode(g->titles[i],LV_LABEL_LONG_CLIP);
        lv_obj_center(g->titles[i]);
    }
    for (unsigned i=0; i<APP_COUNT; ++i) g->dots[i]=box(page,196+i*26,426,16,4,DIM,2);
    lv_obj_update_layout(page);
    lv_obj_scroll_to_x(g->rail,4*GALLERY_STEP,LV_ANIM_OFF);
    gallery_present(g,false);
    lv_obj_add_event_cb(g->rail,gallery_scroll,LV_EVENT_ALL,g);
}

static void apps_create(void)
{
    static const char *const names[]={"设置","AI","呼吸"};
    static const int actions[]={ACT_SETTINGS,ACT_HOME,ACT_BREATHE};
    static const icon_kind_t symbols[]={ICON_GEAR,ICON_FACE,ICON_FLOWER};
    gallery_create(ui.pages[MEO_APPS],&ui.apps,129,306,names,actions,symbols);
    refresh_apps();
}

/* Settings switch: a pill track with a knob. Painted only on change, so the
 * row costs nothing while it sits still. */
static void mic_switch_paint(bool on)
{
    if (!ui.mic_switch || !ui.mic_knob) return;
    lv_obj_set_style_bg_color(ui.mic_switch, color(on ? 0x1E4A46 : 0x2A2A2A), 0);
    lv_obj_set_style_bg_color(ui.mic_knob, color(on ? ACCENT_CYAN : 0x8A8A8A), 0);
    lv_obj_set_x(ui.mic_knob, on ? 35 : 3);
}

static lv_obj_t *setting_row(lv_obj_t *page, int y, const char *name, icon_kind_t symbol,
                             uint32_t tint, int action_id, bool chevron)
{
    /* Settings are component cards rather than compact phone list rows.
     * Five rows have to fit above the 460 px canvas, so the card is 64 px
     * tall (badge 44, type still 28 px) instead of the old 78. */
    int w = 396, h = 64;
    lv_obj_t *row = button(page, (460-w)/2, y, w, h, CARD, NULL, action_id);
    lv_obj_set_style_radius(row, 22, 0);
    lv_obj_t *badge = box(row, 14, (h-44)/2, 44, 44, tint, 14);
    lv_obj_t *mark = icon(badge, symbol, 28, 0xFFFFFF);
    lv_obj_center(mark);
    label(row, name, 76, (h-34)/2, 150, UI_SETTINGS_FONT, WHITE, false);
    if (chevron) {
        lv_obj_t *mark_right = icon(row, ICON_RIGHT, 24, DIM);
        lv_obj_set_pos(mark_right, w - 40, (h - 26) / 2);
    }
    return row;
}

static void settings_create(void)
{
    lv_obj_t *p = ui.pages[MEO_SETTINGS];
    header(p, "设置", ACT_APPS);
    lv_obj_t *row = setting_row(p, 104, "Wi-Fi", ICON_WIFI, 0x2873CB, ACT_WIFI, true);
    ui.wifi_summary = label(row, "未接入", 196, 15, 140, UI_SETTINGS_FONT, MUTED, true);
    row = setting_row(p, 176, "显示与亮度", ICON_SUN, 0xA96724, ACT_DISPLAY, true);
    ui.brightness_summary = label(row, "80%", 246, 15, 88, UI_SETTINGS_FONT, MUTED, true);
    row = setting_row(p, 248, "辅助功能", ICON_EYE, 0x7952BE, ACT_ACCESSIBILITY, true);
    ui.motion_summary = label(row, "标准", 246, 15, 88, UI_SETTINGS_FONT, MUTED, true);
    /* Speaker: opens the volume page. The row itself shows the current level,
       so the most-used state is visible without entering. */
    row = setting_row(p, 320, "扬声器", ICON_SPEAKER, 0x2E7D6B, ACT_SPEAKER, true);
    ui.volume_summary = label(row, "50%", 246, 15, 88, UI_SETTINGS_FONT, MUTED, true);
    /* Microphone: the row opens the meter page, the switch starts/stops the
       capture itself. Tapping the switch must not navigate (no event bubble). */
    row = setting_row(p, 392, "麦克风测试", ICON_FLOWER, 0xBE5272, ACT_MIC, false);
    ui.mic_switch = box(row, 396 - 96, (64 - 40) / 2, 72, 40, 0x2A2A2A, 20);
    lv_obj_add_flag(ui.mic_switch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui.mic_switch, action, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)ACT_MIC_TEST);
    ui.mic_knob = box(ui.mic_switch, 3, 3, 34, 34, 0x8A8A8A, 17);
    lv_obj_remove_flag(ui.mic_knob, LV_OBJ_FLAG_CLICKABLE);
    mic_switch_paint(ui.model.settings.mic_test);
}

static void speaker_create(void)
{
    lv_obj_t *p = ui.pages[MEO_SPEAKER];
    header(p, "扬声器", ACT_SETTINGS);
    lv_obj_t *card = box(p, 42, 124, 376, 168, CARD, 25);
    label(card, "音量", 24, 14, 180, UI_SETTINGS_FONT, WHITE, false);
    ui.volume = label(card, "50%", 224, 6, 128, &meo_font_44, ACCENT_CYAN, true);
    lv_obj_t *slider = lv_slider_create(card);
    ui.volume_slider = slider;
    lv_obj_set_pos(slider, 80, 92);
    lv_obj_set_size(slider, 216, 12);
    lv_obj_set_ext_click_area(slider, 24);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, ui.model.settings.volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, color(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, color(ACCENT_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color(WHITE), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, settings_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *down = button(card, 6, 56, 64, 64, 0x262626, NULL, ACT_VOL_DOWN);
    lv_obj_t *up = button(card, 298, 56, 64, 64, 0x262626, NULL, ACT_VOL_UP);
    lv_obj_center(icon(down, ICON_MINUS, 28, WHITE));
    lv_obj_center(icon(up, ICON_PLUS, 28, WHITE));

    /* Two independent actions: a quick audible tone, and the full physical
       loopback self test (DAC/PA A-B checks). Both text strings use glyphs
       already present in the shipped font. */
    lv_obj_t *test = button(p, 32, 306, 190, 62, 0x1E4A46, NULL, ACT_SPK_TEST);
    lv_obj_t *tmark = icon(test, ICON_SPEAKER, 26, WHITE);
    lv_obj_set_pos(tmark, 10, 18);
    label(test, "播放测试音", 44, 15, 142, UI_SETTINGS_FONT, WHITE, false);
    lv_obj_t *loop = button(p, 238, 306, 190, 62, 0x2A3A5E, NULL, ACT_AUDIO_LOOP);
    label(loop, "回路测试", 0, 14, 190, UI_SETTINGS_FONT, WHITE, true);

    ui.audio_result = label(p, "WAIT", 32, 380, 396, UI_SETTINGS_FONT, MUTED, false);
    ui.audio_meta = label(p, "MCLK -- MIC --/--", 32, 414, 396,
                          UI_SETTINGS_FONT, MUTED, false);
    memset(ui.audio_result_text, 0, sizeof(ui.audio_result_text));
    memset(ui.audio_meta_text, 0, sizeof(ui.audio_meta_text));
}

static void display_settings_create(void)
{
    lv_obj_t *p = ui.pages[MEO_DISPLAY];
    header(p, "显示与亮度", ACT_SETTINGS);
    lv_obj_t *card = box(p, 42, 130, 376, 188, CARD, 25);
    label(card, "亮度", 24, 20, 180, UI_SETTINGS_FONT, WHITE, false);
    ui.brightness = label(card, "80%", 224, 12, 128, &meo_font_44, ACCENT_AMBER, true);
    lv_obj_t *slider = lv_slider_create(card);
    ui.slider = slider;
    lv_obj_set_pos(slider, 80, 102);
    lv_obj_set_size(slider, 216, 12);
    lv_obj_set_ext_click_area(slider, 24);
    lv_slider_set_range(slider, 15, 100);
    lv_slider_set_value(slider, ui.model.settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, color(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, color(ACCENT_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color(WHITE), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, settings_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *dim = button(card, 6, 70, 72, 72, 0x262626, NULL, ACT_DIM);
    lv_obj_t *bright = button(card, 298, 70, 72, 72, 0x262626, NULL, ACT_BRIGHT);
    lv_obj_center(icon(dim,ICON_MINUS,32,WHITE));
    lv_obj_center(icon(bright,ICON_PLUS,32,WHITE));
}

static void accessibility_create(void)
{
    lv_obj_t *p = ui.pages[MEO_ACCESSIBILITY];
    header(p, "辅助功能", ACT_SETTINGS);
    lv_obj_t *card = box(p, 48, 148, 364, 92, CARD, 23);
    label(card, "减少动态效果", 22, 29, 230, UI_SETTINGS_FONT, WHITE, false);
    ui.motion = lv_switch_create(card);
    lv_obj_set_pos(ui.motion, 266, 24);
    lv_obj_set_size(ui.motion, 68, 38);
    lv_obj_set_ext_click_area(ui.motion, 13);
    lv_obj_set_style_bg_color(ui.motion, color(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.motion, color(0x387CE0), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ui.motion, color(WHITE), LV_PART_KNOB);
    lv_obj_set_style_bg_color(ui.motion, color(WHITE), LV_PART_KNOB | LV_STATE_CHECKED);
    if (ui.model.settings.reduced_motion) lv_obj_add_state(ui.motion, LV_STATE_CHECKED);
    lv_obj_add_event_cb(ui.motion, settings_event, LV_EVENT_VALUE_CHANGED, NULL);
}

static void wifi_create(void)
{
    lv_obj_t *p = ui.pages[MEO_WIFI];
    header(p, "网络", ACT_SETTINGS);
    /* Page identity stays an icon, not a colour block: same mark as settings. */
    ui.wifi_page_mark = icon(p, ICON_WIFI, 56, ACCENT_BLUE);
    lv_obj_set_pos(ui.wifi_page_mark, 34, 118);
    ui.wifi_page_state = label(p, "Wi-Fi 未接入", 106, 116, 314, UI_SETTINGS_FONT, WHITE, false);
    ui.wifi_page_ssid = label(p, "—", 106, 152, 314, UI_SETTINGS_FONT, MUTED, false);
    /* Transient feedback for tap actions: every tap must visibly do something. */
    ui.wifi_notice = label(p, "", 106, 186, 314, UI_SETTINGS_FONT, ACCENT_AMBER, false);
    lv_obj_add_flag(ui.wifi_notice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *scan = button(p, 110, 226, 240, 64, 0x16283C, NULL, ACT_WIFI_SCAN);
    ui.wifi_scan_label = label(scan, "扫描附近网络", 0, 0, 240, UI_SETTINGS_FONT, WHITE, true);
    lv_obj_center(ui.wifi_scan_label);
    ui.wifi_hint = label(p, "点扫描找网络", 40, 306, 380, UI_SETTINGS_FONT, MUTED, true);
    static const int rows[WIFI_ROWS] = {ACT_WIFI_AP0, ACT_WIFI_AP1};
    for (unsigned i = 0; i < WIFI_ROWS; ++i) {
        lv_obj_t *row = button(p, 32, 304 + (int)i * 58, 396, 54, CARD, NULL, rows[i]);
        lv_obj_set_style_radius(row, 18, 0);
        ui.wifi_rows[i] = row;
        ui.wifi_row_ssid[i] = label(row, "", 18, 10, 230, UI_SETTINGS_FONT, WHITE, false);
        ui.wifi_row_rssi[i] = label(row, "", 244, 10, 132, UI_SETTINGS_FONT, MUTED, true);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}


/* ---------------- on-screen password entry ----------------
 * Design constraints for a 460x460 panel: pointer only, no long-press help, at
 * least 64 px targets and 28 px type. A full QWERTY cannot satisfy that (10+
 * columns), so this is a feature-phone T9 pad: 12 keys of 99x78, one mode chip,
 * multi-tap with a 700 ms commit, and a masked field that always shows the tail. */

static void pwd_render(void)
{
    if (!ui.pwd_text) return;
    /* Show the tail: typing appends, so the newest characters must stay visible. */
    const unsigned keep = 10;
    char display[PWD_MAX + 8] = {0};
    unsigned start = s_pwd.len > keep ? s_pwd.len - keep : 0;
    unsigned n = 0;
    if (start) display[n++] = '\xe2', display[n++] = '\x80', display[n++] = '\xa6'; /* … */
    for (unsigned i = start; i < s_pwd.len; ++i)
        display[n++] = s_pwd.mask ? '*' : s_pwd.text[i];
    display[n] = 0;
    lv_label_set_text(ui.pwd_text, display);
    lv_obj_set_style_text_color(ui.pwd_text, color(s_pwd.len ? WHITE : DIM), 0);
    if (!s_pwd.len) lv_label_set_text(ui.pwd_text, "密码");
}

static void pwd_mode_paint(void)
{
    static const char *const names[4] = {"abc", "ABC", "123", "!@#"};
    if (ui.pwd_mode) lv_label_set_text(ui.pwd_mode, names[s_pwd.mode & 3]);
    char label[8];
    for (unsigned i = 0; i < PWD_KEY_COUNT; ++i) {
        if (!ui.pwd_key_labels[i]) continue;
        int digit = pwd_key_digit[i];
        if (digit < 0) continue;                       /* delete / ok keep their text */
        if (s_pwd.mode == PWD_DIGIT) { label[0] = (char)('0' + digit); label[1] = 0; }
        else if (s_pwd.mode == PWD_SYM) { label[0] = pwd_sym[digit]; label[1] = 0; }
        else {
            const char *letters = pwd_t9[digit];
            if (!letters[0]) snprintf(label, sizeof(label), "%s", digit == 1 ? "." : "-");
            else {
                char shown[8];
                snprintf(shown, sizeof(shown), "%s", letters);
                if (s_pwd.mode == PWD_UPPER)
                    for (char *c = shown; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
                snprintf(label, sizeof(label), "%s", shown);
            }
        }
        lv_label_set_text(ui.pwd_key_labels[i], label);
    }
}

static void pwd_push(const char *add)
{
    size_t n = strlen(add);
    if (!n || s_pwd.len + n > PWD_MAX) return;
    memcpy(s_pwd.text + s_pwd.len, add, n);
    s_pwd.len += (unsigned)n;
    s_pwd.text[s_pwd.len] = 0;
    pwd_render();
}

static void pwd_backspace(void)
{
    if (!s_pwd.len) return;
    s_pwd.text[--s_pwd.len] = 0;
    pwd_render();
}

static void pwd_tap_digit(int digit, uint64_t now)
{
    if (s_pwd.mode == PWD_DIGIT) { char c[2] = {(char)('0' + digit), 0}; pwd_push(c); return; }
    if (s_pwd.mode == PWD_SYM) { char c[2] = {pwd_sym[digit], 0}; pwd_push(c); return; }
    const char *letters = pwd_t9[digit];
    if (!letters[0]) { pwd_push(digit == 1 ? "." : "-"); return; }  /* 1/0 = punctuation */
    bool chain = s_pwd.last_key == digit && now - s_pwd.last_ms < PWD_TAP_MS &&
                 s_pwd.len && s_pwd.text[s_pwd.len - 1] != ' ';
    int index = 0;
    if (chain) {
        index = (s_pwd.tap + 1) % (int)strlen(letters);
        s_pwd.len -= 1;                                 /* only ASCII is ever pushed */
        s_pwd.text[s_pwd.len] = 0;
        pwd_render();
    }
    char c[2] = {letters[index], 0};
    if (s_pwd.mode == PWD_UPPER && c[0] >= 'a' && c[0] <= 'z') c[0] -= 32;
    pwd_push(c);
    s_pwd.last_key = digit;
    s_pwd.tap = index;
    s_pwd.last_ms = now;
}

static void pwd_submit(void)
{
    if (!s_pwd.len) { wifi_notice_for("无密码", 3000); return; }
    bool ok = ui.config.wifi_connect &&
              ui.config.wifi_connect(s_pwd.ssid, s_pwd.text);
    memset(s_pwd.text, 0, sizeof(s_pwd.text));    /* never keep the key in RAM */
    s_pwd.len = 0;
    if (!ok) { wifi_notice_for("Wi-Fi 未就绪", 5000); return; }
    wifi_notice("");                 /* the state line reports progress */
    meo_ui_navigate(MEO_WIFI);
}

static void password_create(void)
{
    lv_obj_t *p = ui.pages[MEO_PASSWORD];
    header(p, "密码", ACT_WIFI);
    lv_obj_t *chip = button(p, 336, 40, 88, 64, CARD, NULL, ACT_PWD_MODE);
    ui.pwd_mode = label(chip, "abc", 0, 0, 88, &meo_font_28, ACCENT_AMBER, true);
    lv_obj_center(ui.pwd_mode);

    ui.pwd_ssid = label(p, "", 24, 116, 412, &meo_font_28, MUTED, false);
    ui.pwd_field = box(p, 24, 158, 352, 64, CARD, 20);
    ui.pwd_text = label(ui.pwd_field, "密码", 18, 16, 316, &meo_font_28, DIM, false);
    lv_label_set_long_mode(ui.pwd_text, LV_LABEL_LONG_CLIP);
    lv_obj_t *eye = button(p, 384, 158, 64, 64, CARD, NULL, ACT_PWD_EYE);
    ui.pwd_eye = icon(eye, ICON_EYE, 34, WHITE);
    lv_obj_center(ui.pwd_eye);

    /* 3 rows x 4 columns: 1-4 / 5-8 / 9 0 delete OK */
    /* 3 rows must fit 240..456: 3*64 + 2*12 = 216, so keys stay >= the 64 px
       target without running off the panel. */
    const int key_w = 99, key_h = 64, gap = 10, gap_y = 12, x0 = 24, y0 = 240;
    for (unsigned i = 0; i < PWD_KEY_COUNT; ++i) {
        int col = (int)(i % 4), row = (int)(i / 4);
        lv_obj_t *key = button(p, x0 + col * (key_w + gap), y0 + row * (key_h + gap_y),
                               key_w, key_h, CARD, NULL, ACT_PWD_KEY0 + (int)i);
        lv_obj_set_style_radius(key, 22, 0);
        int digit = pwd_key_digit[i];
        const char *text = digit >= 0 ? "" : (digit == -1 ? "<-" : "OK");
        ui.pwd_keys[i] = key;
        ui.pwd_key_labels[i] = label(key, text, 0, 0, key_w, &meo_font_28, WHITE, true);
        lv_obj_center(ui.pwd_key_labels[i]);
    }
    pwd_render();
    pwd_mode_paint();
}

bool meo_ui_font_has(const char *utf8)
{
    /* Body text is always 28 px; the 44 px face only ever holds digits, so
       testing it here would report a false negative for every CJK string. */
    return text_renderable(utf8, &meo_font_28);
}

void meo_ui_password_open(const char *ssid)
{
    if (!ui.ready) return;
    memset(s_pwd.text, 0, sizeof(s_pwd.text));
    s_pwd.len = 0;
    s_pwd.last_key = -1;
    s_pwd.tap = 0;
    s_pwd.mode = PWD_ABC;
    s_pwd.mask = true;
    snprintf(s_pwd.ssid, sizeof(s_pwd.ssid), "%s", ssid ? ssid : "");
    if (ui.pwd_ssid) {
        const char *shown = text_renderable(s_pwd.ssid, &meo_font_28) ? s_pwd.ssid : "已选网络";
        lv_label_set_text(ui.pwd_ssid, shown);
    }
    pwd_render();
    pwd_mode_paint();
    meo_ui_navigate(MEO_PASSWORD);
}


/* ---------------- microphone level meter (two channels) ----------------
 * Two microphones land on the two ADC channels of the ES8389, so the page shows
 * two independent rows. Each row is a segmented bar: a segment is either fully
 * lit or dark, which reads as "how loud" at a glance and lets the redraw skip
 * every segment whose state did not change. */
static void mic_create(void)
{
    lv_obj_t *p = ui.pages[MEO_MIC];
    header(p, "麦克风", ACT_SETTINGS);

    ui.mic_db[0] = label(p, "L    -96 dB", 32, 146, 320, &meo_font_28, DIM, false);
    ui.mic_db[1] = label(p, "R    -96 dB", 32, 262, 320, &meo_font_28, DIM, false);
    ui.mic_state = label(p, "待机", 32, 374, 320, &meo_font_28, MUTED, false);

    /* 13 segments of 24 px with a 4 px gap fill 72..432, leaving room for the
       L/R label column without touching the panel edge. */
    const int seg_w = 24, gap = 4, x0 = 72, seg_h = 34;
    const int row_y[2] = {190, 306};
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < MIC_SEGS; ++i) {
            lv_obj_t *seg = box(p, x0 + i * (seg_w + gap), row_y[c],
                                seg_w, seg_h, 0x1A1A1A, 8);
            ui.mic_seg[c][i] = seg;
        }
    }
    ui.mic_target[0] = ui.mic_target[1] = 0.0f;
    ui.mic_shown[0] = ui.mic_shown[1] = 0.0f;
    ui.mic_db_shown[0] = ui.mic_db_shown[1] = -1000;
    memset(ui.mic_lit, -1, sizeof(ui.mic_lit));
}

void meo_ui_set_mic(const float level[2], bool running)
{
    if (!ui.ready || !level) return;
    for (int c = 0; c < 2; ++c) {
        float v = level[c];
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        ui.mic_target[c] = v;
        int db = v > 0.0001f ? (int)(20.0f * log10f(v)) : -96;
        if (db < -96) db = -96;
        if (db != ui.mic_db_shown[c] && ui.mic_db[c]) {
            ui.mic_db_shown[c] = db;
            lv_label_set_text_fmt(ui.mic_db[c], "%c   %d dB",
                                  c ? 'R' : 'L', db);
            lv_obj_set_style_text_color(ui.mic_db[c],
                                        color(v > 0.02f ? WHITE : DIM), 0);
        }
    }
    if (ui.mic_state)
        lv_label_set_text(ui.mic_state, running ? "运行" : "待机");
}

void meo_ui_set_audio(const meo_audio_view_t *view)
{
    if (!ui.ready || !view || !ui.audio_result || !ui.audio_meta) return;

    /* ASCII-only status: the shipped font covers it, and no per-frame text
       rebuild happens because both labels are cached by their strings. */
    char result[40];
    if (view->busy) snprintf(result, sizeof(result), "RUN");
    else if (view->loop_state == 2 || view->loop_state == 3)
        snprintf(result, sizeof(result), "%s %d",
                 view->loop_state == 2 ? "PASS" : "FAIL", view->loop_score);
    else snprintf(result, sizeof(result), "WAIT");

    if (strcmp(result, ui.audio_result_text) != 0) {
        strncpy(ui.audio_result_text, result, sizeof(ui.audio_result_text) - 1);
        ui.audio_result_text[sizeof(ui.audio_result_text) - 1] = '\0';
        lv_label_set_text(ui.audio_result, result);
        uint32_t col = view->loop_state == 2 ? ACCENT_CYAN
                     : view->loop_state == 3 ? 0xFF8A8A
                     : view->busy ? ACCENT_AMBER : MUTED;
        lv_obj_set_style_text_color(ui.audio_result, color(col), 0);
    }

    char meta[40];
    snprintf(meta, sizeof(meta), "MCLK %d%% MIC %u/%u VOL %d%s",
             view->mclk_duty, view->route_l, view->route_r, view->volume,
             view->mic_running ? "" : " OFF");
    if (strcmp(meta, ui.audio_meta_text) != 0) {
        strncpy(ui.audio_meta_text, meta, sizeof(ui.audio_meta_text) - 1);
        ui.audio_meta_text[sizeof(ui.audio_meta_text) - 1] = '\0';
        lv_label_set_text(ui.audio_meta, meta);
    }
}

/* Called from the UI tick: smooth + repaint only the segments that flipped. */
static void mic_tick(void)
{
    if (!ui.mic_seg[0][0]) return;
    for (int c = 0; c < 2; ++c) {
        /* Fast attack, slow release: a VU needle, not a raw 10 ms block. */
        if (ui.mic_target[c] > ui.mic_shown[c])
            ui.mic_shown[c] += (ui.mic_target[c] - ui.mic_shown[c]) * 0.55f;
        else
            ui.mic_shown[c] += (ui.mic_target[c] - ui.mic_shown[c]) * 0.12f;

        int lit = (int)(ui.mic_shown[c] * MIC_SEGS + 0.35f);
        for (int i = 0; i < MIC_SEGS; ++i) {
            bool on = i < lit;
            if (ui.mic_lit[c][i] == (int8_t)on) continue;
            ui.mic_lit[c][i] = (int8_t)on;
            uint32_t ink = !on ? 0x1A1A1A
                        : i > 10 ? 0xE06767
                        : i > 7  ? ACCENT_AMBER
                                 : ACCENT_CYAN;
            lv_obj_set_style_bg_color(ui.mic_seg[c][i], color(ink), 0);
        }
    }
}

static void breathe_create(void)
{
    lv_obj_t *p = ui.pages[MEO_BREATHE];
    /* Full-bleed dial: the page-wide right-swipe gesture returns to Apps. */
    ui.bloom=box(p,UI_BREATH_CX-UI_BREATH_SIZE/2,UI_BREATH_CY-UI_BREATH_SIZE/2,
                 UI_BREATH_SIZE,UI_BREATH_SIZE,BG,0);
    lv_obj_set_style_bg_opa(ui.bloom,LV_OPA_TRANSP,0);
    lv_obj_add_flag(ui.bloom,LV_OBJ_FLAG_USER_2);
    lv_obj_add_event_cb(ui.bloom,draw_breath_bloom,LV_EVENT_DRAW_MAIN,NULL);
    ui.number = label(p,"",40,375,380,&meo_font_44,WHITE,true);
    lv_obj_add_flag(ui.number,LV_OBJ_FLAG_HIDDEN);
}

static void visible(lv_obj_t *o, bool show)
{ if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

/* Producers often still have the raw 32-bit LVGL tick. Extend a low-word
 * timestamp around the UI's monotonic 64-bit clock so the 49-day wrap cannot
 * make a fresh gaze/respiration sample look ancient. Full 64-bit timestamps pass
 * through unchanged. */
static uint64_t normalize_input_time(uint64_t sample)
{
    if (ui.clock_ms <= UINT32_MAX || sample > UINT32_MAX) return sample;
    int32_t delta = (int32_t)((uint32_t)sample - (uint32_t)ui.clock_ms);
    return ui.clock_ms + delta;
}

/* The eye is a round-capped line with a ~76 px stroke, so every setter
 * invalidates a padded 216x236 box, and a width change invalidates the union of
 * the old and new extents (292x312). Pushing points + position + width every
 * frame repainted the equivalent of ~80 full screens per second, which starved
 * touch handling and made the whole UI feel stuck. Compute the target state,
 * compare with what the panel already shows, and push only real changes. */
static struct {
    bool valid;
    int x[2], y[2], width[2], count[2];
    lv_point_precise_t pts[2][EYE_POINTS];
    int pupil_x[2], pupil_y[2];
    bool pupil_on[2];
} s_eye;

static void update_home(void)
{
    const meo_model_t *m = &ui.model;
    for (int i = 0; i < 2; ++i) {
        const meo_eye_shape_t *s = &m->eye_shape[i];
        int width = (int)lroundf(s->width * m->openness);
        if (width < 6) width = 6;
        /* Squash the stroke vertically into a small horizontal dash for a
         * blink; preserve its horizontal footprint rather than disappearing. */
        float half_x = s->half_x + (s->width - width) * .5f;
        unsigned count = half_x < .1f && fabsf(s->arch) < .1f ? 2 : EYE_POINTS;
        int x = (i ? 309 : 151) - 70 + (int)lroundf(m->eye_x * 16);
        int y = 150 + (int)lroundf(m->eye_y * 12);

        lv_point_precise_t tmp[EYE_POINTS];
        for (unsigned j = 0; j < count; ++j) {
            float t = -1 + 2.0f * j / (count - 1);
            tmp[j].x = lroundf(70 + half_x * t);
            tmp[j].y = lroundf(80 + s->y + m->openness *
                               (s->half_y * t + s->arch * (t * t - .5f)));
        }

        bool moved = !s_eye.valid || s_eye.x[i] != x || s_eye.y[i] != y;
        bool resized = !s_eye.valid || s_eye.width[i] != width;
        bool reshaped = !s_eye.valid || s_eye.count[i] != (int)count ||
                        memcmp(tmp, s_eye.pts[i], count * sizeof(tmp[0])) != 0;

        /* Order matters: a width change is the expensive invalidation, so apply
           it before the points when both change, and let LVGL merge. */
        if (moved) {
            lv_obj_set_pos(ui.eye[i], x, y);
            s_eye.x[i] = x;
            s_eye.y[i] = y;
        }
        if (resized) {
            lv_obj_set_style_line_width(ui.eye[i], width, 0);
            s_eye.width[i] = width;
        }
        if (reshaped) {
            memcpy(ui.eye_points[i], tmp, count * sizeof(tmp[0]));
            lv_line_set_points(ui.eye[i], ui.eye_points[i], count);
            s_eye.count[i] = (int)count;
            memcpy(s_eye.pts[i], tmp, count * sizeof(tmp[0]));
        }

        int pupil_x = 58 + (int)lroundf(m->eye_x * 10);
        int pupil_y = 68 + (int)lroundf(m->eye_y * 7);
        bool round_eye = s->half_y > 0.5f || fabsf(s->arch) < .1f;
        bool pupil_on = round_eye && width > 24;
        if (!s_eye.valid || s_eye.pupil_x[i] != pupil_x || s_eye.pupil_y[i] != pupil_y) {
            lv_obj_set_pos(ui.pupil[i], pupil_x, pupil_y);
            s_eye.pupil_x[i] = pupil_x;
            s_eye.pupil_y[i] = pupil_y;
        }
        if (!s_eye.valid || s_eye.pupil_on[i] != pupil_on) {
            visible(ui.pupil[i], pupil_on);
            s_eye.pupil_on[i] = pupil_on;
        }
    }
    s_eye.valid = true;
}

static void update_breathe(void)
{
    const meo_model_t *m = &ui.model;
    /* Invalid or expired inputs produce a quiet static flower, never a
       synthesized breathing rhythm or a previously measured rate. */
    float level = !m->respiration_live ? .22f :
                  m->settings.reduced_motion ? .45f : m->breath_level;
    if (fabsf(level-ui.bloom_level)>.001f || ui.bloom_live!=m->respiration_live) {
        ui.bloom_level=level;
        ui.bloom_live=m->respiration_live;
        lv_obj_invalidate(ui.bloom);
    }
    visible(ui.number,m->rate_live);
    if (m->rate_live) {
        int tenths=(int)lroundf(m->respiration.rate_bpm*10);
        /* Re-laying out identical text every frame is pure invalidation. */
        if (tenths != ui.last_rate_tenths) {
            ui.last_rate_tenths = tenths;
            lv_label_set_text_fmt(ui.number,"%d.%d / min",tenths/10,tenths%10);
        }
    } else {
        ui.last_rate_tenths = -1;
    }
}


/* ---- cost probe ------------------------------------------------------------
 * Everything here runs on the GUI task, so the counters need no lock; only the
 * published snapshot is read from the app task. Windows of 30 samples (~1 s at
 * 33 ms) are averaged so the reported numbers do not jitter on a single frame. */
#define PERF_WINDOW 50          /* 50 x 20 ms = 1 s */
/* Frame-pacing probe state. Declared here because perf_event() consumes it. */
#define FPS_JANK_US 33333
#define FPS_SEG_BREAK_US 34000    /* a gap over one frame budget starts a new burst */
#define FPS_MAX_SAMPLES 128
static volatile meo_ui_fps_t s_fps_result;
static uint32_t s_fps_seq;
static bool     s_fps_active;
static int64_t  s_fps_deadline;
static int64_t  s_fps_times[FPS_MAX_SAMPLES];
static uint32_t s_fps_render_us[FPS_MAX_SAMPLES];
static uint32_t s_fps_n;
static uint32_t s_fps_max_render_us, s_fps_max_flush_us;
static char     s_fps_tag[16];
static void fps_check_deadline(void);
static volatile uint32_t s_pf_period_ms, s_pf_busy_us, s_pf_render_us, s_pf_flush_us;
static volatile uint32_t s_pf_refreshes;
static volatile uint32_t s_pf_px_s, s_pf_flush_n, s_pf_max_pub;
static volatile int32_t s_pf_max_area_pub[4];
static int32_t s_pf_max_area_live[4];
static uint32_t s_pf_px;
static uint32_t s_pf_n, s_pf_period, s_pf_busy, s_pf_render, s_pf_flush, s_pf_refr;
static uint32_t s_pf_flushes;
static uint32_t s_pf_max_px, s_pf_max_x1, s_pf_max_y1, s_pf_max_x2, s_pf_max_y2;
static int64_t s_pf_tick_at, s_pf_render_at, s_pf_flush_at;

static void perf_publish(void)
{
    if (s_pf_n < PERF_WINDOW) return;
    /* s_pf_period accumulates microseconds; publish milliseconds. */
    s_pf_period_ms = s_pf_period / s_pf_n / 1000;
    s_pf_busy_us   = s_pf_busy / s_pf_n;
    s_pf_render_us = s_pf_render / (s_pf_refr ? s_pf_refr : 1);
    s_pf_flush_us  = s_pf_flush / (s_pf_refr ? s_pf_refr : 1);
    /* Normalise to one second so refreshes reads as delivered fps. */
    s_pf_refreshes = (uint32_t)((uint64_t)s_pf_refr * 1000000ULL /
                                (s_pf_period ? s_pf_period : 1));
    /* 30 ticks are ~1.05 s; normalise so the number reads as pixels/second. */
    s_pf_px_s = (uint32_t)((uint64_t)s_pf_px * 1000000 /
                           (s_pf_period ? s_pf_period : 1));
    s_pf_flush_n = s_pf_flushes;
    s_pf_max_pub = s_pf_max_px;
    for (int i = 0; i < 4; ++i) s_pf_max_area_pub[i] = s_pf_max_area_live[i];
    s_pf_n = s_pf_period = s_pf_busy = s_pf_render = s_pf_flush = s_pf_refr = 0;
    s_pf_px = s_pf_flushes = 0;
    s_pf_max_px = 0;
}

/* RENDER_READY/FLUSH_FINISH arrive on the same task as the tick timer. */
static void perf_event(lv_event_t *event)
{
    int64_t now = perf_now_us();
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RENDER_START) {
        s_pf_render_at = now;
    } else if (code == LV_EVENT_RENDER_READY) {
        uint32_t took = s_pf_render_at ? (uint32_t)(now - s_pf_render_at) : 0;
        if (s_pf_render_at) { s_pf_render += took; ++s_pf_refr; }
        if (s_fps_active) {
            if (took > s_fps_max_render_us) s_fps_max_render_us = took;
            if (s_fps_n < FPS_MAX_SAMPLES) {
                s_fps_times[s_fps_n] = now;
                s_fps_render_us[s_fps_n] = took;
                ++s_fps_n;
            }
            fps_check_deadline();
        }
    } else if (code == LV_EVENT_FLUSH_START) {
        s_pf_flush_at = now;
    } else if (code == LV_EVENT_FLUSH_FINISH) {
        if (s_pf_flush_at) {
            uint32_t took = (uint32_t)(now - s_pf_flush_at);
            s_pf_flush += took;
            if (s_fps_active && took > s_fps_max_flush_us)
                s_fps_max_flush_us = took;
        }
    }
}

/* Count invalidated pixels. A static page must show almost none; a large
 * number means something is repainting the whole screen every frame. */
static void perf_invalidate(lv_event_t *event)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(event);
    if (!area) return;
    uint32_t px = (uint32_t)lv_area_get_size(area);
    s_pf_px += px;
    ++s_pf_flushes;
    if (px > s_pf_max_px) {
        s_pf_max_px = px;
        s_pf_max_x1 = (uint32_t)area->x1; s_pf_max_y1 = (uint32_t)area->y1;
        s_pf_max_x2 = (uint32_t)area->x2; s_pf_max_y2 = (uint32_t)area->y2;
        s_pf_max_area_live[0] = area->x1; s_pf_max_area_live[1] = area->y1;
        s_pf_max_area_live[2] = area->x2; s_pf_max_area_live[3] = area->y2;
    }
}

void meo_ui_perf(meo_ui_perf_t *out)
{
    if (!out) return;
    out->period_ms = s_pf_period_ms;
    out->busy_us   = s_pf_busy_us;
    out->render_us = s_pf_render_us;
    out->flush_us  = s_pf_flush_us;
    out->refreshes = s_pf_refreshes;
    out->px_per_s = s_pf_px_s;
    out->flushes = s_pf_flush_n;
    out->max_px = s_pf_max_pub;
    for (int i = 0; i < 4; ++i) out->max_area[i] = s_pf_max_area_pub[i];
}

/* ---- frame pacing probe ---------------------------------------------------
 * The 1-second cost window above cannot answer "did the 180 ms page swipe
 * hold 30 fps?". This probe timestamps every RENDER_READY while a window is
 * open, measures the gap between delivered frames, and publishes the worst
 * gap plus the number of frames that missed the 33.3 ms budget. Only frames
 * that were actually rendered count: LVGL correctly renders nothing on a
 * static page, and idle time after an animation must not be charged against
 * the animation's frame rate. */
void meo_ui_fps_start(uint32_t window_ms, const char *tag)
{
    if (window_ms < 50) window_ms = 50;
    if (window_ms > 10000) window_ms = 10000;
    s_fps_result.done = 0;
    s_fps_n = 0;
    s_fps_max_render_us = s_fps_max_flush_us = 0;
    if (tag) snprintf(s_fps_tag, sizeof(s_fps_tag), "%s", tag);
    else s_fps_tag[0] = '\0';
    s_fps_deadline = perf_now_us() + (int64_t)window_ms * 1000;
    s_fps_active = true;
}

void meo_ui_fps_read(meo_ui_fps_t *out)
{
    if (!out) return;
    *out = s_fps_result;      /* one 40-byte copy; published only when done=1 */
}

static void fps_publish(void)
{
    if (!s_fps_active) return;
    s_fps_active = false;
    /* Keep the longest burst of consecutive frames separated by <=100 ms.
       A page swipe followed by an idle screen is one burst, not a 340 ms
       "animation" with a huge fake stall at the end. */
    uint32_t best_start = 0, best_len = 0, cur_start = 0, n = s_fps_n;
    for (uint32_t i = 1; i < n; ++i) {
        if (s_fps_times[i] - s_fps_times[i - 1] > FPS_SEG_BREAK_US) {
            if (i - cur_start > best_len) { best_len = i - cur_start; best_start = cur_start; }
            cur_start = i;
        }
    }
    if (n - cur_start > best_len) { best_len = n - cur_start; best_start = cur_start; }
    int64_t span = 0;
    uint32_t jank = 0, max_gap = 0;
    if (best_len > 1) {
        span = s_fps_times[best_start + best_len - 1] - s_fps_times[best_start];
        for (uint32_t i = best_start + 1; i < best_start + best_len; ++i) {
            int64_t gap = s_fps_times[i] - s_fps_times[i - 1];
            if (gap > max_gap) max_gap = (uint32_t)gap;
            if (gap > FPS_JANK_US) ++jank;
        }
    }
    /* Diagnostics cover the WHOLE window, not only the longest segment, so
       cutting a window into segments can never hide a genuinely slow frame. */
    uint32_t slow_render = 0, slow_gap = 0;
    for (uint32_t i = 1; i < n; ++i) {
        int64_t gap = s_fps_times[i] - s_fps_times[i - 1];
        if (gap > FPS_JANK_US) {
            if (s_fps_render_us[i] > FPS_JANK_US) ++slow_render;
            else ++slow_gap;
        }
    }
    uint32_t fps_x10 = 0;
    if (best_len > 1 && span > 0)
        fps_x10 = (uint32_t)(((uint64_t)(best_len - 1) * 10000000ULL) /
                             (uint64_t)span);
    meo_ui_fps_t r;
    memset(&r, 0, sizeof(r));
    r.frames = best_len;
    r.fps_x10 = fps_x10;
    r.span_ms = (uint32_t)(span > 0 ? span / 1000 : 0);
    r.max_frame_ms = max_gap / 1000;
    r.jank = jank;
    r.slow_render = slow_render;
    r.slow_gap = slow_gap;
    r.max_render_us = s_fps_max_render_us;
    r.max_flush_us = s_fps_max_flush_us;
    snprintf(r.tag, sizeof(r.tag), "%s", s_fps_tag);
    r.done = 1;
    r.seq = ++s_fps_seq;
    s_fps_result = r;
    /* Never write USB from the GUI task: a blocked CDC write would stall the
       very frames we are measuring. The app service publishes the snapshot
       on its own task (UI FPS auto-report). */
}

static void fps_check_deadline(void)
{
    if (s_fps_active && perf_now_us() >= s_fps_deadline) fps_publish();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    int64_t t_enter = perf_now_us();
    if (s_pf_tick_at) s_pf_period += (uint32_t)(t_enter - s_pf_tick_at);
    s_pf_tick_at = t_enter;
    uint32_t now = lv_tick_get();
    ui.clock_ms += (uint32_t)(now - ui.last_tick); /* Extend the 49-day tick wrap. */
    ui.last_tick = now;
    meo_model_tick(&ui.model, ui.clock_ms);
    ui.frames++;
    /* Safety net: if a transition is interrupted and SCREEN_LOADED never
       arrives, never leave the icons at low detail permanently. Also drives
       the small per-frame batches that restore full detail. */
    icon_lod_service();
    if (s_notice_hide_at && ui.clock_ms >= s_notice_hide_at) wifi_notice("");
    /* The overlay is fully opaque and covers the whole panel: animating the
       face underneath would repaint ~91k px per tick for nobody to see. */
    if (ui.model.page == MEO_HOME && !s_overlay_up) update_home();
    else if (ui.model.page == MEO_BREATHE) update_breathe();
    else if (ui.model.page == MEO_MIC) mic_tick();
    glow_tick();
    /* Animation windows must also expire on a completely static page, where
       RENDER_READY never fires. */
    fps_check_deadline();
    s_pf_busy += (uint32_t)(perf_now_us() - t_enter);
    if (++s_pf_n >= PERF_WINDOW) perf_publish();
}

/* ---- cold-page warm-up ----------------------------------------------------
 * The first time a page is shown, LVGL lays out its tree, rasterises every
 * glyph and executes each icon's draw callback. On this board that cost
 * 78-89 ms on ONE frame, which is a visible stutter and breaks the 30 fps
 * floor on the first visit to apps/settings. Render every page once behind a
 * black veil during bring-up: ~250 ms of boot time, invisible to the user,
 * and every later transition starts from warm caches. A timer (not a blocking
 * loop) keeps the normal refresh path and the DSI flush callback in charge. */
#ifdef ESP_PLATFORM
static lv_timer_t *s_warm_timer;
static int s_warm_idx;

static void warmup_abort(void)
{
    if (s_warm_timer) { lv_timer_delete(s_warm_timer); s_warm_timer = NULL; }
    /* Never leave the panel dark when an early tap interrupts bring-up. */
    (void)fls2_display_set_brightness((uint8_t)ui.model.settings.brightness);
}

static void warmup_tick(lv_timer_t *t)
{
    (void)t;
    if (s_warm_idx < MEO_PAGE_COUNT) {
        lv_screen_load(ui.pages[s_warm_idx]);
        /* Backlight is off, so the user sees black while every glyph, icon
           and layout is rasterised. A top-layer veil cannot be used: LVGL
           skips drawing a screen fully covered by an opaque layer, which is
           why the earlier veil-based warm-up changed nothing. */
        lv_obj_invalidate(ui.pages[s_warm_idx]);
        lv_refr_now(lv_display_get_default());
        ++s_warm_idx;
        if (s_warm_idx == MEO_PAGE_COUNT) lv_timer_set_period(t, 180);
        return;
    }
    if (s_warm_idx < MEO_PAGE_COUNT + 2) {
        /* Also burn two full transitions: the cold cost showed up on every
           frame of the first two animated navigations, not just page load. */
        lv_screen_load_anim(ui.pages[(s_warm_idx & 1) ? MEO_SETTINGS : MEO_APPS],
                            LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0, false);
        ++s_warm_idx;
        return;
    }
    warmup_abort();
    lv_screen_load(ui.pages[MEO_HOME]);
}
int meo_ui_warmup_count(void) { return s_warm_idx; }
bool meo_ui_warmup_active(void) { return s_warm_timer != NULL; }
#else
/* The desktop render harness takes pixel-exact screenshots; a boot veil
   would make them all black. Warm-up is a device-only concern. */
static void warmup_abort(void) {}
int meo_ui_warmup_count(void) { return 0; }
bool meo_ui_warmup_active(void) { return false; }
#endif

void meo_ui_create(const meo_ui_config_t *config)
{
    if (ui.ready) return;
    ui.config = *config;
    ui.last_tick = lv_tick_get();
    ui.clock_ms = ui.last_tick;
    meo_model_init(&ui.model, ui.clock_ms, &config->settings);
    meo_model_set_mode(&ui.model,MEO_RADAR);
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_180);
    for (int i = 0; i < MEO_PAGE_COUNT; ++i) {
        ui.pages[i] = box(NULL, 0, 0, 460, 460, i == MEO_HOME ? HOME_BG : BG, 0);
        lv_obj_add_flag(ui.pages[i],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(ui.pages[i],screen_loaded,LV_EVENT_SCREEN_LOADED,NULL);
        lv_obj_add_event_cb(ui.pages[i],track_pointer_motion,LV_EVENT_PRESSING,NULL);
        lv_obj_set_style_text_font(ui.pages[i], &meo_font_28, 0);
    }
    home_create();
    apps_create();
    settings_create();
    display_settings_create();
    accessibility_create();
    wifi_create();
    password_create();
    mic_create();
    speaker_create();
    /* After every page exists: refresh_settings() writes ui.volume and
       ui.volume_summary, which are NULL until speaker_create() ran. */
    refresh_settings();
    breathe_create();
    ui.battery_percent = -1;   /* unknown until the adapter samples the ADC */
    ui.last_rate_tenths = -1;
    ui.wifi_state = MEO_WIFI_OFF;
    shutdown_create();
    ui.ready = true;
    for (lv_indev_t *indev=lv_indev_get_next(NULL);indev;indev=lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev)==LV_INDEV_TYPE_POINTER &&
            lv_indev_get_display(indev)==lv_display_get_default())
            lv_indev_add_event_cb(indev,pointer_navigation,LV_EVENT_ALL,indev);
    }
    lv_screen_load(ui.pages[MEO_HOME]);
    /* 20 ms model/eye/VU tick (50 Hz). LVGL itself refreshes at 15 ms, so a
       dropped logic tick can never be the reason an animation misses 30 fps. */
    lv_timer_create(tick, 20, NULL);
#ifdef ESP_PLATFORM
    /* Blank the panel with the backlight (not an LVGL veil) while every page
       is rendered once. ~610 ms of black boot screen buys warm caches for the
       first real navigation, which otherwise took 70-90 ms per frame. */
    (void)fls2_display_set_brightness(0);
    s_warm_idx = 0;
    s_warm_timer = lv_timer_create(warmup_tick, 25, NULL);
#endif
    update_home();
    update_breathe();
    lv_obj_add_event_cb(lv_layer_top(), draw_back_parent, LV_EVENT_DRAW_MAIN, NULL);
    lv_display_add_event_cb(lv_display_get_default(), perf_invalidate, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(lv_display_get_default(), perf_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), perf_event, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(lv_display_get_default(), perf_event, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), perf_event, LV_EVENT_FLUSH_FINISH, NULL);
}

void meo_ui_feed_gaze(meo_gaze_t sample)
{ sample.at_ms = normalize_input_time(sample.at_ms); meo_model_gaze(&ui.model, sample); }
void meo_ui_feed_respiration(meo_resp_t sample)
{ sample.at_ms = normalize_input_time(sample.at_ms); meo_model_respiration(&ui.model, sample); }

void meo_ui_set_breath_mode(meo_breath_mode_t mode)
{
    /* Product breathing follows samples only; preserve the hook for existing
       callers but do not re-enable the obsolete guided-session UI. */
    if (!ui.ready || mode!=MEO_RADAR) return;
    meo_model_set_mode(&ui.model,MEO_RADAR);
    update_breathe();
}

void meo_ui_set_expression(meo_expression_t expression, uint32_t hold_ms)
{ meo_model_set_expression(&ui.model, expression, hold_ms); }

void meo_ui_auto_expression(void)
{ meo_model_auto_expression(&ui.model); }

void meo_ui_settings_saved(bool success)
{
    if (ui.ready) save_status(success ? "已保存到设备" : "保存失败");
}

/* ---------------- status strip, Wi-Fi summary, shutdown overlay ---------------- */

static void power_paint(void)
{
    if (!ui.power_gauge) return;
    int pct = ui.battery_percent;
    bool known = pct >= 0;
    visible(ui.power_gauge, known);
    visible(ui.power_text, known);
    if (!known) return;
    if (pct > 100) pct = 100;
    /* Home is the face and stays strictly monochrome: level is ink, and a
       trailing "+" means VBUS is present (it does NOT mean the cell is
       charging). Colour is reserved for the settings/network pages. */
    int width = (pct * 50 + 50) / 100;
    if (width < 2) width = 2;
    lv_obj_set_width(ui.power_fill, width);
    lv_obj_set_style_bg_color(ui.power_fill, color(WHITE), 0);
    lv_obj_set_style_border_color(ui.power_gauge, color(ui.charging ? WHITE : MUTED), 0);
    lv_obj_set_style_text_color(ui.power_text, color(WHITE), 0);
    char pct_text[16];
    if (ui.charging) snprintf(pct_text, sizeof(pct_text), "%d%%+", pct);
    else snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
    label_text_if_changed(ui.power_text, pct_text);
}

void meo_ui_set_power(int percent, bool charging)
{
    if (!ui.ready) return;
    if (percent == ui.battery_percent && charging == ui.charging) return;
    ui.battery_percent = percent;
    ui.charging = charging;
    power_paint();
}

/* Line under the state: the connected SSID, else how many networks were found.
 * Keeping it populated removes the 76 px hole that appeared when both the SSID
 * and the transient notice were hidden. */
static void refresh_wifi_info(void)
{
    if (!ui.wifi_page_ssid) return;
    if (ui.wifi_state == MEO_WIFI_ONLINE) {
        visible(ui.wifi_page_ssid, true);
        return;                          /* text set by meo_ui_set_wifi */
    }
    if (ui.wifi_ap_visible) {
        char found[48];
        snprintf(found, sizeof(found), "找到 %u 个网络", ui.wifi_ap_visible);
        label_text_if_changed(ui.wifi_page_ssid, found);
        visible(ui.wifi_page_ssid, true);
    } else {
        visible(ui.wifi_page_ssid, false);
    }
}

void meo_ui_set_wifi(meo_wifi_state_t state, const char *ssid, int rssi)
{
    if (!ui.ready) return;
    (void)rssi; /* Signal strength is not drawn; kept for the report path. */
    ui.wifi_state = state;
    uint32_t ink = state == MEO_WIFI_ONLINE ? ACCENT_BLUE :
                  state == MEO_WIFI_CONNECTING ? ACCENT_AMBER :
                  state == MEO_WIFI_ERROR ? 0xE06767 : DIM;
    if (ui.wifi_mark && s_wifi_ink != ink) {
        s_wifi_ink = ink;
        lv_obj_set_style_line_color(ui.wifi_mark, color(ink), 0);
    }
    const char *label_text = state == MEO_WIFI_ONLINE ? (ssid && *ssid ? "已接入" : "已接入")
                           : state == MEO_WIFI_CONNECTING ? "连接中"
                           : state == MEO_WIFI_ERROR ? "连接失败" : "未接入";
    label_text_if_changed(ui.wifi_summary, label_text);
    if (state == MEO_WIFI_ONLINE || state == MEO_WIFI_ERROR) wifi_notice("");
    const char *scan_text = state == MEO_WIFI_ERROR ? "重试连接" : "扫描附近网络";
    label_text_if_changed(ui.wifi_scan_label, scan_text);
    char page_state[48];
    snprintf(page_state, sizeof(page_state), "Wi-Fi %s", label_text);
    label_text_if_changed(ui.wifi_page_state, page_state);
    const char *name = state == MEO_WIFI_ONLINE && ssid && *ssid ? ssid : "";
    if (name[0] && !text_renderable(name, &meo_font_28)) name = "已连接网络";
    if (name[0]) label_text_if_changed(ui.wifi_page_ssid, name);
    refresh_wifi_info();
}

void meo_ui_set_scan_list(const char ssid[][33], const int rssi[], unsigned count)
{
    if (!ui.ready) return;
    unsigned rows = count > WIFI_ROWS ? WIFI_ROWS : count;
    ui.wifi_ap_visible = rows;
    for (unsigned i = 0; i < WIFI_ROWS; ++i) {
        if (i < rows && ssid[i][0]) {
            snprintf(ui.wifi_ap_ssid[i], sizeof(ui.wifi_ap_ssid[i]), "%s", ssid[i]);
            if (text_renderable(ui.wifi_ap_ssid[i], &meo_font_28)) {
                label_text_if_changed(ui.wifi_row_ssid[i], ui.wifi_ap_ssid[i]);
            } else {
                char fallback[32];
                snprintf(fallback, sizeof(fallback), "网络 %u", i + 1);
                label_text_if_changed(ui.wifi_row_ssid[i], fallback);
            }
            char rssi_text[32];
            snprintf(rssi_text, sizeof(rssi_text), "%d dBm", rssi[i]);
            label_text_if_changed(ui.wifi_row_rssi[i], rssi_text);
            visible(ui.wifi_rows[i], true);
        } else {
            ui.wifi_ap_ssid[i][0] = 0;
            visible(ui.wifi_rows[i], false);
        }
    }
    if (ui.wifi_hint) visible(ui.wifi_hint, rows == 0);
    refresh_wifi_info();
}

static void anim_bg_opa(void *obj, int32_t value)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)value, 0);
}

static void anim_text_opa(void *obj, int32_t value)
{
    lv_obj_set_style_text_opa(obj, (lv_opa_t)value, 0);
}

/* Halo breathing, driven from the 20 ms tick. As a standalone screen the
   overlay is the only layer rasterised, so a full 30 fps update cycle fits in
   the frame budget; the old 85 ms step (12 fps) left visible 90 ms gaps. */
#define GLOW_STEP_MS 20
static uint64_t s_glow_at_ms;
static uint32_t s_glow_phase;

static void glow_start(bool on)
{
    if (!ui.shutdown_glow) return;
    s_glow_phase = 0;
    s_glow_at_ms = ui.clock_ms;
    if (!on) lv_obj_set_style_bg_opa(ui.shutdown_glow, 22, 0);
}

static void glow_tick(void)
{
    if (!s_overlay_up || !ui.shutdown_glow) return;
    if (ui.clock_ms < s_glow_at_ms) return;
    s_glow_at_ms = ui.clock_ms + GLOW_STEP_MS;
    s_glow_phase = (s_glow_phase + 1) % 40;          /* 40 steps x 85 ms = 3.4 s */
    float t = (float)s_glow_phase / 39.0f;
    float wave = 0.5f - 0.5f * cosf(t * 6.2831853f);
    lv_obj_set_style_bg_opa(ui.shutdown_glow, (lv_opa_t)(14 + 30.0f * wave), 0);
}

/* Each new digit fades up once: motion confirms the count is live. Scaling the
 * label would invalidate its parent (the full-screen overlay) every frame, so
 * the pulse is done on text opacity instead. */
static void number_pop(void)
{
    if (!ui.shutdown_number) return;
    lv_anim_delete(ui.shutdown_number, anim_text_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui.shutdown_number);
    lv_anim_set_exec_cb(&a, anim_text_opa);
    lv_anim_set_values(&a, 110, LV_OPA_COVER);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void shutdown_create(void)
{
    /* A standalone screen, NOT a child of lv_layer_top(). A top-layer
       overlay forces LVGL to rasterise two full-screen layers on every
       animation frame; as the active screen it is the only layer drawn,
       which halves the halo animation's frame cost. */
    ui.shutdown_layer = box(NULL, 0, 0, 460, 460, 0x000000, 0);
    lv_obj_set_style_bg_opa(ui.shutdown_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(ui.shutdown_layer, &meo_font_28, 0);
    ui.shutdown_title = label(ui.shutdown_layer, "即将关机", 40, 96, 380, &meo_font_28, WHITE, true);

    /* A closed ring reads as a countdown; an open arc looked like a horseshoe.
       Rotation 270 puts the ring's start at 12 o'clock. */
    /* Soft halo behind the ring: the only moving element besides the number,
       so the page reads as "counting" rather than a static warning. */
    ui.shutdown_glow = box(ui.shutdown_layer, 141, 154, 178, 178, ACCENT_AMBER, 89);
    lv_obj_set_style_bg_opa(ui.shutdown_glow, 22, 0);
    lv_obj_set_style_transform_pivot_x(ui.shutdown_glow, 89, 0);
    lv_obj_set_style_transform_pivot_y(ui.shutdown_glow, 89, 0);

    ui.shutdown_arc = lv_arc_create(ui.shutdown_layer);
    lv_obj_set_pos(ui.shutdown_arc, 141, 154);
    lv_obj_set_size(ui.shutdown_arc, 178, 178);
    lv_arc_set_rotation(ui.shutdown_arc, 270);
    lv_arc_set_bg_angles(ui.shutdown_arc, 0, 360);
    lv_arc_set_range(ui.shutdown_arc, 0, 50);
    lv_arc_set_value(ui.shutdown_arc, 50);
    lv_obj_remove_style(ui.shutdown_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ui.shutdown_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.shutdown_arc, 11, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.shutdown_arc, color(0x262626), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.shutdown_arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.shutdown_arc, color(ACCENT_AMBER), LV_PART_INDICATOR);
    lv_obj_remove_flag(ui.shutdown_arc, LV_OBJ_FLAG_CLICKABLE);

    ui.shutdown_number = label(ui.shutdown_layer, "5", 141, 218, 178, &meo_font_44, WHITE, true);
    ui.shutdown_hint = label(ui.shutdown_layer, "松手即取消", 60, 362, 340, &meo_font_28, MUTED, true);
}

void meo_ui_set_shutdown(int seconds_left)
{
    if (!ui.ready || !ui.shutdown_layer) return;
    if (seconds_left < 0) {
        if (s_overlay_up) {
            s_overlay_up = false;
            s_eye.valid = false;        /* force one fresh face push */
            lv_screen_load(ui.pages[ui.model.page]);
        }
        glow_start(false);
        s_key_number_shown = -1;
        return;
    }
    if (!s_overlay_up) {
        s_overlay_up = true;
        lv_screen_load(ui.shutdown_layer);
        /* Fade the whole overlay in so it never snaps onto the screen. */
        lv_obj_set_style_bg_opa(ui.shutdown_layer, LV_OPA_TRANSP, 0);
        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, ui.shutdown_layer);
        lv_anim_set_exec_cb(&fade, anim_bg_opa);
        lv_anim_set_values(&fade, 0, 255);
        lv_anim_set_duration(&fade, 150);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
        lv_anim_start(&fade);
        glow_start(true);
    }
    if (seconds_left > 0) {
        bool changed = s_key_number_shown != seconds_left;
        label_text_if_changed(ui.shutdown_title, "即将关机");
        char digit[16];
        snprintf(digit, sizeof(digit), "%d", seconds_left);
        label_text_if_changed(ui.shutdown_number, digit);
        if (ui.shutdown_arc) lv_arc_set_value(ui.shutdown_arc, seconds_left * 10);
        visible(ui.shutdown_hint, true);
        if (changed) {
            s_key_number_shown = seconds_left;
            number_pop();
        }
    } else {
        /* Rail released: the key can no longer cancel anything. */
        label_text_if_changed(ui.shutdown_title, "正在关机");
        label_text_if_changed(ui.shutdown_number, "");
        if (ui.shutdown_arc) lv_arc_set_value(ui.shutdown_arc, 0);
        visible(ui.shutdown_hint, false);
        s_key_number_shown = -1;
        glow_start(false);
    }
}

const meo_model_t *meo_ui_model(void) { return &ui.model; }
uint32_t meo_ui_frames(void) { return ui.frames; }

const char *meo_ui_page_name(meo_page_t page)
{
    static const char *names[] = {
        "eyes", "apps", "settings", "wifi",
        "breathe", "display", "accessibility", "password", "mic", "speaker"
    };
    /* Guard the array itself, not MEO_PAGE_COUNT: this list has silently
       lagged behind the enum before, and names[9] read a garbage pointer that
       surfaced as a crash inside printf on the page that was just entered. */
    if (page < MEO_HOME || page >= (meo_page_t)(sizeof(names) / sizeof(names[0])))
        return "invalid";
    return names[page];
}
