#include "volume_hud.h"

#include "metrics.h"
#include "theme.h"

#include <stdlib.h>

/* Capsule geometry. Sized to sit inside the 28px status bar, centred between
 * the clock and the battery readout so it covers only the bar's title. */
#define HUD_W         136
#define HUD_H         22
#define HUD_PAD_L     10
#define HUD_PAD_R     12
#define HUD_GAP       8
#define HUD_BAR_H     6

/* The pill drops in from above the screen edge while widening out from
 * HUD_START_W, and reverses that on the way out. The way in is ease-out-back:
 * it leaves at full speed, so the pill reacts the instant the wheel moves,
 * then overshoots ~10% and settles. (LVGL's lv_anim_path_overshoot starts
 * from rest instead, which left the pill invisible for the first ~130 ms of
 * a turn.) Clear of the shadow when tucked. */
#define HUD_START_W     (HUD_W * 55 / 100)
#define HUD_Y_HIDDEN    (-(HUD_H + 12))
#define HUD_IN_MS       380
#define HUD_FADE_IN_MS  160
#define HUD_OUT_MS      260
#define HUD_HOLD_MS     1500 /* idle time before it tucks away */
#define HUD_BAR_ANIM_MS 120

/* Turning past either end stretches the pill toward the turn, then lets it
 * spring back -- iOS's rubber band at max/min volume. */
#define HUD_STRETCH_PX      10
#define HUD_STRETCH_OUT_MS  90
#define HUD_STRETCH_BACK_MS 240

struct rpod_volume_hud {
    lv_obj_t *pill;
    lv_obj_t *icon;
    lv_obj_t *bar;
    lv_timer_t *hide_timer;

    bool shown;     /* up, or on its way in -- false once it starts tucking away */
    bool available; /* last show() had a real level, not "no volume control" */

    /* Animated state, mirrored here rather than read back off the object (an
     * lv_obj's coords lag its styles until the next layout pass), so an
     * interrupted animation can restart from exactly where it was. */
    int32_t y;
    int32_t opa;
    int32_t base_w;  /* from the in/out animation */
    int32_t stretch; /* signed rubber-band offset from bump() */
};

static int32_t hud_y_shown(void)
{
    return (rpod_metrics()->header_h - HUD_H) / 2;
}

/* Width is the in/out width plus however far the rubber band is pulled; the
 * x shift keeps the far edge put, so the pill grows *toward* the turn. */
static void apply_geometry(rpod_volume_hud_t *hud)
{
    int32_t s = hud->stretch;
    lv_obj_set_width(hud->pill, hud->base_w + (s < 0 ? -s : s));
    lv_obj_set_x(hud->pill, s / 2);
}

static void anim_y_cb(void *var, int32_t v)
{
    rpod_volume_hud_t *hud = var;
    hud->y = v;
    lv_obj_set_y(hud->pill, v);
}

static void anim_opa_cb(void *var, int32_t v)
{
    rpod_volume_hud_t *hud = var;
    hud->opa = v;
    lv_obj_set_style_opa(hud->pill, (lv_opa_t)v, 0);
}

static void anim_w_cb(void *var, int32_t v)
{
    rpod_volume_hud_t *hud = var;
    hud->base_w = v;
    apply_geometry(hud);
}

static void anim_stretch_cb(void *var, int32_t v)
{
    rpod_volume_hud_t *hud = var;
    hud->stretch = v;
    apply_geometry(hud);
}

/* Every animation runs on `hud` itself (not the pill object) so the exec
 * callbacks can reach its mirrored state, and so rpod_volume_hud_delete() can
 * cancel the lot with one lv_anim_delete(hud, NULL). Starting one replaces
 * any animation already running with the same exec callback -- that's how a
 * show() interrupts a hide that's still in flight. */
static void start_anim(rpod_volume_hud_t *hud, lv_anim_exec_xcb_t exec_cb, int32_t from, int32_t to,
                       uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done_cb)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, hud);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    if (path == lv_anim_path_custom_bezier3) {
        LV_ANIM_SET_EASE_OUT_BACK(&a); /* the only custom curve used here */
    }
    if (done_cb != NULL) {
        lv_anim_set_completed_cb(&a, done_cb);
    }
    lv_anim_start(&a);
}

/* Tucked fully away: stop drawing it at all. */
static void hide_done_cb(lv_anim_t *a)
{
    rpod_volume_hud_t *hud = a->var;
    lv_obj_add_flag(hud->pill, LV_OBJ_FLAG_HIDDEN);
}

static void hide_timer_cb(lv_timer_t *timer)
{
    rpod_volume_hud_t *hud = lv_timer_get_user_data(timer);
    lv_timer_pause(timer);
    hud->shown = false;
    start_anim(hud, anim_y_cb, hud->y, HUD_Y_HIDDEN, HUD_OUT_MS, lv_anim_path_ease_in, hide_done_cb);
    start_anim(hud, anim_opa_cb, hud->opa, LV_OPA_TRANSP, HUD_OUT_MS, lv_anim_path_ease_in, NULL);
    start_anim(hud, anim_w_cb, hud->base_w, HUD_START_W, HUD_OUT_MS, lv_anim_path_ease_in, NULL);
}

static const char *level_glyph(int percent)
{
    if (percent == 0) {
        return LV_SYMBOL_MUTE;
    }
    return percent < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX;
}

/* The three speaker glyphs differ in width; sizing the icon to the widest
 * keeps the bar from shifting sideways as the glyph changes. */
static int32_t widest_glyph_w(const lv_font_t *font)
{
    static const char *const glyphs[] = { LV_SYMBOL_MUTE, LV_SYMBOL_VOLUME_MID, LV_SYMBOL_VOLUME_MAX };
    int32_t w = 0;
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
        lv_point_t size;
        lv_text_get_size(&size, glyphs[i], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > w) {
            w = size.x;
        }
    }
    return w;
}

rpod_volume_hud_t *rpod_volume_hud_create(void)
{
    rpod_volume_hud_t *hud = calloc(1, sizeof(*hud));
    const rpod_metrics_t *m = rpod_metrics();

    hud->pill = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(hud->pill);
    rpod_theme_style_glass_panel(hud->pill, LV_RADIUS_CIRCLE);
    /* Solid rather than the usual translucent glass: the pill sits over the
     * status bar's "Now Playing" title, which ghosts through even a ~92% fill
     * (most visibly across the bar's light track). Fading the whole pill in
     * still cross-fades the title out underneath it. */
    lv_obj_set_style_bg_opa(hud->pill, LV_OPA_COVER, 0);
    lv_obj_set_height(hud->pill, HUD_H);
    lv_obj_remove_flag(hud->pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hud->pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(hud->pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hud->pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(hud->pill, HUD_PAD_L, 0);
    lv_obj_set_style_pad_right(hud->pill, HUD_PAD_R, 0);
    lv_obj_set_style_pad_column(hud->pill, HUD_GAP, 0);

    hud->icon = lv_label_create(hud->pill);
    lv_obj_set_style_text_font(hud->icon, m->font_small, 0);
    lv_obj_set_style_text_color(hud->icon, RPOD_COLOR_TEXT, 0);
    lv_obj_set_width(hud->icon, widest_glyph_w(m->font_small));
    lv_label_set_text_static(hud->icon, LV_SYMBOL_VOLUME_MAX);

    /* Flex-grown, so the bar is what gives as the pill widens, narrows, or
     * stretches -- the icon and padding stay put. */
    hud->bar = lv_bar_create(hud->pill);
    lv_obj_remove_style_all(hud->bar);
    lv_obj_set_height(hud->bar, HUD_BAR_H);
    lv_obj_set_flex_grow(hud->bar, 1);
    lv_obj_set_style_radius(hud->bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hud->bar, RPOD_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hud->bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(hud->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(hud->bar, RPOD_COLOR_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(hud->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(hud->bar, HUD_BAR_ANIM_MS, LV_PART_MAIN);
    lv_bar_set_range(hud->bar, 0, 100);
    hud->available = true;

    hud->y = HUD_Y_HIDDEN;
    hud->opa = LV_OPA_TRANSP;
    hud->base_w = HUD_START_W;
    lv_obj_align(hud->pill, LV_ALIGN_TOP_MID, 0, hud->y);
    lv_obj_set_style_opa(hud->pill, (lv_opa_t)hud->opa, 0);
    apply_geometry(hud);
    lv_obj_add_flag(hud->pill, LV_OBJ_FLAG_HIDDEN);

    hud->hide_timer = lv_timer_create(hide_timer_cb, HUD_HOLD_MS, hud);
    lv_timer_pause(hud->hide_timer);
    return hud;
}

void rpod_volume_hud_delete(rpod_volume_hud_t *hud)
{
    if (hud == NULL) {
        return;
    }
    lv_anim_delete(hud, NULL);
    lv_timer_delete(hud->hide_timer);
    lv_obj_delete(hud->pill);
    free(hud);
}

void rpod_volume_hud_show(rpod_volume_hud_t *hud, int percent)
{
    bool appearing = !hud->shown;
    bool available = percent >= 0;

    lv_label_set_text_static(hud->icon, available ? level_glyph(percent) : LV_SYMBOL_VOLUME_MAX);
    if (available != hud->available) {
        hud->available = available;
        lv_obj_set_style_text_color(hud->icon, available ? RPOD_COLOR_TEXT : RPOD_COLOR_DIM_TEXT, 0);
    }
    /* Slide the fill between levels while the pill is up; when it's only
     * just appearing, the bar may still hold a level from minutes ago, so
     * jump straight to the current one instead of sweeping from that. */
    lv_bar_set_value(hud->bar, available ? percent : 0, appearing ? LV_ANIM_OFF : LV_ANIM_ON);

    if (appearing) {
        hud->shown = true;
        lv_obj_remove_flag(hud->pill, LV_OBJ_FLAG_HIDDEN);
        /* From wherever it is now: fully tucked away, or part-way out if
         * this interrupts a hide. */
        start_anim(hud, anim_y_cb, hud->y, hud_y_shown(), HUD_IN_MS, lv_anim_path_custom_bezier3, NULL);
        start_anim(hud, anim_opa_cb, hud->opa, LV_OPA_COVER, HUD_FADE_IN_MS, lv_anim_path_ease_out, NULL);
        start_anim(hud, anim_w_cb, hud->base_w, HUD_W, HUD_IN_MS, lv_anim_path_custom_bezier3, NULL);
    }

    lv_timer_reset(hud->hide_timer);
    lv_timer_resume(hud->hide_timer);
}

void rpod_volume_hud_bump(rpod_volume_hud_t *hud, int dir)
{
    if (dir == 0) {
        return;
    }
    /* Turning on past a limit keeps delivering steps; let each stretch play
     * out instead of restarting it every step, which would just pin it
     * stretched. */
    if (lv_anim_get(hud, anim_stretch_cb) != NULL) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, hud);
    lv_anim_set_exec_cb(&a, anim_stretch_cb);
    lv_anim_set_values(&a, 0, dir > 0 ? HUD_STRETCH_PX : -HUD_STRETCH_PX);
    lv_anim_set_duration(&a, HUD_STRETCH_OUT_MS);
    lv_anim_set_reverse_duration(&a, HUD_STRETCH_BACK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

bool rpod_volume_hud_is_shown(const rpod_volume_hud_t *hud)
{
    return hud->shown;
}
