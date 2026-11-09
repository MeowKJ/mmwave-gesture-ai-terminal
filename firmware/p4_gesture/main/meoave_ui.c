/* Meoave / Quiet companion. Code-native LVGL 9 visuals, no bitmap mockups.
 * Logical coordinates use the complete 460x460 square panel. The proven ESP display
 * port owns 180-degree pixel rotation and LVGL owns pointer rotation once. */
#include "meoave_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

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
       ACT_SLOT_LEFT, ACT_SLOT_RIGHT };
typedef struct {
    lv_obj_t *rail, *tiles[GALLERY_COUNT], *captions[GALLERY_VISIBLE];
    lv_obj_t *titles[GALLERY_VISIBLE], *dots[APP_COUNT];
    const char *const *names;
    const int *actions;
    unsigned first;
    bool moving, captions_ready, recentering;
} gallery_t;
static struct {
    meo_model_t model;
    meo_ui_config_t config;
    lv_obj_t *pages[MEO_PAGE_COUNT], *eye[2], *pupil[2];
    lv_point_precise_t eye_points[2][EYE_POINTS];
    lv_obj_t *brightness, *slider, *save_labels[3], *motion, *brightness_summary, *motion_summary;
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

/* Icons are native draw primitives, not private-use text glyphs. Keep their
 * geometry inside a 32-unit canvas and render at the requested pixel size;
 * never scale a label through a temporary transformed layer. USER_1 marks
 * passive icon objects for real-render regression checks. */
typedef enum { ICON_LEFT=1, ICON_RIGHT, ICON_WIFI, ICON_SUN, ICON_EYE,
               ICON_MINUS, ICON_PLUS, ICON_GEAR, ICON_FACE, ICON_FLOWER } icon_kind_t;

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
        for (int step=0; step<32; ++step) {
            float a=step/32.0f, b=(step+1)/32.0f, mid=(a+b)*.5f;
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

static void draw_icon(lv_event_t *event)
{
    lv_obj_t *o=lv_event_get_target_obj(event);
    icon_draw_t d={.layer=lv_event_get_layer(event),
        .ink=lv_obj_get_style_line_color(o,0), .size=lv_obj_get_width(o)};
    lv_obj_get_coords(o,&d.area);
    d.stroke=(d.size*3+16)/32;
    if (d.stroke<2) d.stroke=2;
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
    return o;
}

static void action(lv_event_t *event);
static void track_pointer_motion(lv_event_t *event);
static void update_breathe(void);
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
    if (!ui.ready || page < MEO_HOME || page >= MEO_PAGE_COUNT || page == ui.model.page ||
        ui.navigating || ui.back_dragging || ui.back_settling) return;
    meo_page_t previous = ui.model.page;
    ui.model.page = page;
    if (page==MEO_BREATHE) update_breathe();
    ui.pointer_down = ui.back_candidate = false;
    bool back = page == meo_page_parent(previous);
    ui.navigating = true;
    lv_screen_load_anim(ui.pages[page], ui.model.settings.reduced_motion ? LV_SCR_LOAD_ANIM_NONE
                        : back ? LV_SCR_LOAD_ANIM_MOVE_RIGHT : LV_SCR_LOAD_ANIM_MOVE_LEFT,
                        ui.model.settings.reduced_motion ? 0 : 180, 0, false);
}

static void screen_loaded(lv_event_t *event)
{
    if (lv_event_get_target_obj(event)==ui.pages[ui.model.page]) ui.navigating=false;
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

static void action(lv_event_t *event)
{
    lv_indev_t *indev=lv_indev_active();
    if (ui.navigating || ui.back_dragging || ui.back_settling || ui.press_blocked) return;
    /* A drag across an app/row is not a tap, including a non-edge swipe. */
    if (indev && lv_indev_get_type(indev)==LV_INDEV_TYPE_POINTER &&
        (ui.tap_moved || lv_indev_get_gesture_dir(indev)!=LV_DIR_NONE)) return;
    int id = (int)(uintptr_t)lv_event_get_user_data(event);
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
    case ACT_DIM: case ACT_BRIGHT: {
        int value = ui.model.settings.brightness + (id==ACT_DIM ? -10 : 10);
        value = value < 15 ? 15 : value > 100 ? 100 : value;
        if (value==ui.model.settings.brightness) break;
        ui.model.settings.brightness = (uint8_t)value;
        lv_slider_set_value(ui.slider,ui.model.settings.brightness,LV_ANIM_OFF);
        refresh_settings(); changed(); break;
    }
    }
}

static void settings_event(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (target == ui.motion) ui.model.settings.reduced_motion = lv_obj_has_state(target, LV_STATE_CHECKED);
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
        lv_label_set_text(g->titles[i],g->names[(g->first+i)%APP_COUNT]);
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

static lv_obj_t *setting_row(lv_obj_t *page, int y, const char *name, icon_kind_t symbol,
                             uint32_t tint, int action_id)
{
    /* Settings are component cards rather than compact phone list rows. */
    /* Square panel: all components share the same generous bounds. */
    int w = 396, h = 96;
    lv_obj_t *row = button(page, (460-w)/2, y, w, h, CARD, NULL, action_id);
    lv_obj_set_style_radius(row, 26, 0);
    lv_obj_t *badge = box(row, 18, (h-56)/2, 56, 56, tint, 18);
    lv_obj_t *mark = icon(badge, symbol, 32, 0xFFFFFF);
    lv_obj_center(mark);
    label(row, name, 94, (h-34)/2, 150, UI_SETTINGS_FONT, WHITE, false);
    lv_obj_t *chevron=icon(row,ICON_RIGHT,28,DIM);
    lv_obj_set_pos(chevron,w-46,(h-28)/2);
    return row;
}

static void settings_create(void)
{
    lv_obj_t *p = ui.pages[MEO_SETTINGS];
    header(p, "设置", ACT_APPS);
    lv_obj_t *row = setting_row(p, 112, "Wi-Fi", ICON_WIFI, 0x2873CB, ACT_WIFI);
    label(row, "未接入", 246, 31, 88, UI_SETTINGS_FONT, MUTED, true);
    row = setting_row(p, 220, "显示与亮度", ICON_SUN, 0xA96724, ACT_DISPLAY);
    ui.brightness_summary = label(row, "80%", 246, 31, 88, UI_SETTINGS_FONT, MUTED, true);
    row = setting_row(p, 328, "辅助功能", ICON_EYE, 0x7952BE, ACT_ACCESSIBILITY);
    ui.motion_summary = label(row, "标准", 246, 31, 88, UI_SETTINGS_FONT, MUTED, true);
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
    lv_obj_t *mark=icon(p,ICON_WIFI,64,ACCENT_BLUE);
    lv_obj_set_pos(mark,198,128);
    label(p, "Wi-Fi 未接入", 40, 216, 380, UI_SETTINGS_FONT, WHITE, true);
    label(p, "—", 0, 262, 460, &meo_font_28, MUTED, true);
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
        lv_obj_set_pos(ui.eye[i], (i ? 309 : 151) - 70 + (int)(m->eye_x * 16),
                       150 + (int)(m->eye_y * 12));
        lv_obj_set_style_line_width(ui.eye[i], width, 0);
        for (unsigned j = 0; j < count; ++j) {
            float t = -1 + 2.0f * j / (count - 1);
            ui.eye_points[i][j].x = 70 + half_x * t;
            ui.eye_points[i][j].y = 80 + s->y + m->openness *
                (s->half_y * t + s->arch * (t * t - .5f));
        }
        lv_line_set_points(ui.eye[i], ui.eye_points[i], count);
        lv_obj_set_pos(ui.pupil[i], 58 + (int)(m->eye_x * 10),
                       68 + (int)(m->eye_y * 7));
        bool round_eye = s->half_y > 0.5f || fabsf(s->arch) < .1f;
        visible(ui.pupil[i], round_eye && width > 24);
    }
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
        lv_label_set_text_fmt(ui.number,"%d.%d / min",tenths/10,tenths%10);
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    ui.clock_ms += (uint32_t)(now - ui.last_tick); /* Extend the 49-day tick wrap. */
    ui.last_tick = now;
    meo_model_tick(&ui.model, ui.clock_ms);
    ui.frames++;
    if (ui.model.page == MEO_HOME) update_home();
    else if (ui.model.page == MEO_BREATHE) update_breathe();
}

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
    refresh_settings();
    wifi_create();
    breathe_create();
    ui.ready = true;
    for (lv_indev_t *indev=lv_indev_get_next(NULL);indev;indev=lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev)==LV_INDEV_TYPE_POINTER &&
            lv_indev_get_display(indev)==lv_display_get_default())
            lv_indev_add_event_cb(indev,pointer_navigation,LV_EVENT_ALL,indev);
    }
    lv_screen_load(ui.pages[MEO_HOME]);
    lv_timer_create(tick, 33, NULL);
    update_home();
    update_breathe();
    lv_obj_add_event_cb(lv_layer_top(), draw_back_parent, LV_EVENT_DRAW_MAIN, NULL);
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

const meo_model_t *meo_ui_model(void) { return &ui.model; }
uint32_t meo_ui_frames(void) { return ui.frames; }

const char *meo_ui_page_name(meo_page_t page)
{
    static const char *names[] = {
        "eyes", "apps", "settings", "wifi",
        "breathe", "display", "accessibility"
    };
    return page >= MEO_HOME && page < MEO_PAGE_COUNT
        ? names[page] : "invalid";
}
