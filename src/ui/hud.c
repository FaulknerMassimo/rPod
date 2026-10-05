#include "hud.h"

#include "airpods_art.h"
#include "metrics.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

/* Capsule geometry. Sized to sit inside the 28px status bar, centred between
 * the clock and the battery readout so it covers only the bar's title. A
 * message grows the pill to fit its text; one too wide to stay centred
 * shifts left rather than run into the readout (and the AirPods glyph
 * beside it), and the room between the clock and that is the limit. */
#define HUD_W         136
#define HUD_MSG_MIN_W 110
#define HUD_LEFT_EDGE  52  /* clear of the clock */
#define HUD_RIGHT_EDGE 238 /* clear of the AirPods glyph and the battery */
#define HUD_MSG_MAX_W (HUD_RIGHT_EDGE - HUD_LEFT_EDGE)
#define HUD_H         22
#define HUD_PAD_L     10
#define HUD_PAD_R     12
#define HUD_GAP       8
#define HUD_BAR_H     6
#define HUD_GLYPH_H   12 /* the AirPods glyph, matched to the font's symbols */

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
#define HUD_RESIZE_MS   200  /* switching faces while up */
#define HUD_HOLD_MS     1500 /* idle time before the volume face tucks away */
#define HUD_MSG_HOLD_MS 2200 /* how long a message stays */
#define HUD_BAR_ANIM_MS 120

/* Turning past either end stretches the pill toward the turn, then lets it
 * spring back -- iOS's rubber band at max/min volume. */
#define HUD_STRETCH_PX      10
#define HUD_STRETCH_OUT_MS  90
#define HUD_STRETCH_BACK_MS 240

typedef enum { FACE_VOLUME, FACE_MESSAGE } face_t;

static struct {
    bool created;
    lv_obj_t *pill;
    lv_obj_t *icon;   /* font glyph: speaker levels, Bluetooth */
    lv_obj_t *glyph;  /* AirPods image */
    lv_obj_t *bar;
    lv_obj_t *text;
    lv_timer_t *hide_timer;

    bool shown;     /* up, or on its way in -- false once it starts tucking away */
    face_t face;
    bool available; /* last volume had a real level, not "no volume control" */
    int32_t icon_w; /* the widest speaker glyph -- see widest_glyph_w() */

    /* A message that arrived mid-turn, shown once the volume face is done. */
    bool queued;
    rpod_hud_icon_t queued_icon;
    char queued_text[64];

    /* Animated state, mirrored here rather than read back off the object (an
     * lv_obj's coords lag its styles until the next layout pass), so an
     * interrupted animation can restart from exactly where it was. */
    int32_t y;
    int32_t opa;
    int32_t base_w;  /* from the in/out/resize animation */
    int32_t stretch; /* signed rubber-band offset from bump() */
} h;

static int32_t hud_y_shown(void)
{
    return (rpod_metrics()->header_h - HUD_H) / 2;
}

/* Width is the in/out width plus however far the rubber band is pulled; the
 * x shift keeps the far edge put, so the pill grows *toward* the turn. A
 * pill wider than fits centred is nudged left of centre, following its
 * width as that animates. */
static void apply_geometry(void)
{
    int32_t s = h.stretch;
    int32_t over = rpod_metrics()->screen_w / 2 + h.base_w / 2 - HUD_RIGHT_EDGE;
    lv_obj_set_width(h.pill, h.base_w + (s < 0 ? -s : s));
    lv_obj_set_x(h.pill, (over > 0 ? -over : 0) + s / 2);
}

static void anim_y_cb(void *var, int32_t v)
{
    (void)var;
    h.y = v;
    lv_obj_set_y(h.pill, v);
}

static void anim_opa_cb(void *var, int32_t v)
{
    (void)var;
    h.opa = v;
    lv_obj_set_style_opa(h.pill, (lv_opa_t)v, 0);
}

static void anim_w_cb(void *var, int32_t v)
{
    (void)var;
    h.base_w = v;
    apply_geometry();
}

static void anim_stretch_cb(void *var, int32_t v)
{
    (void)var;
    h.stretch = v;
    apply_geometry();
}

/* Every animation runs on `h` itself (not the pill object), so the exec
 * callbacks share one var. Starting one replaces any animation already
 * running with the same var and exec callback -- that's how a show
 * interrupts a hide that's still in flight. */
static void start_anim(lv_anim_exec_xcb_t exec_cb, int32_t from, int32_t to, uint32_t ms,
                       lv_anim_path_cb_t path, lv_anim_completed_cb_t done_cb)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &h);
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

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
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

/* Shows `icon` (for the speaker, at `percent`) and returns its width. */
static int32_t set_icon(rpod_hud_icon_t icon, int percent, bool dim)
{
    lv_color_t color = dim ? RPOD_COLOR_DIM_TEXT : RPOD_COLOR_TEXT;
    bool airpods = icon == RPOD_HUD_ICON_AIRPODS;
    set_hidden(h.glyph, !airpods);
    set_hidden(h.icon, airpods);
    if (airpods) {
        lv_obj_set_style_image_recolor(h.glyph, color, 0);
        const lv_image_dsc_t *dsc = rpod_airpods_glyph(HUD_GLYPH_H);
        return dsc != NULL ? dsc->header.w : 0;
    }
    lv_obj_set_style_text_color(h.icon, color, 0);
    if (icon == RPOD_HUD_ICON_BLUETOOTH) {
        lv_label_set_text_static(h.icon, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_width(h.icon, LV_SIZE_CONTENT);
        lv_point_t size;
        lv_text_get_size(&size, LV_SYMBOL_BLUETOOTH, rpod_metrics()->font_small, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        return size.x;
    }
    lv_label_set_text_static(h.icon, level_glyph(percent));
    lv_obj_set_width(h.icon, h.icon_w);
    return h.icon_w;
}

/* Drops the pill in from wherever it is (fully tucked away, or part-way out
 * if this interrupts a hide) at width `w`; if it's already up, just eases it
 * to `w`. Restarts the countdown either way. */
static void show(int32_t w, uint32_t hold_ms)
{
    if (!h.shown) {
        h.shown = true;
        lv_obj_remove_flag(h.pill, LV_OBJ_FLAG_HIDDEN);
        start_anim(anim_y_cb, h.y, hud_y_shown(), HUD_IN_MS, lv_anim_path_custom_bezier3, NULL);
        start_anim(anim_opa_cb, h.opa, LV_OPA_COVER, HUD_FADE_IN_MS, lv_anim_path_ease_out, NULL);
        start_anim(anim_w_cb, h.base_w, w, HUD_IN_MS, lv_anim_path_custom_bezier3, NULL);
    } else if (w != h.base_w) {
        start_anim(anim_w_cb, h.base_w, w, HUD_RESIZE_MS, lv_anim_path_ease_out, NULL);
    }
    lv_timer_set_period(h.hide_timer, hold_ms);
    lv_timer_reset(h.hide_timer);
    lv_timer_resume(h.hide_timer);
}

static void show_message(rpod_hud_icon_t icon, const char *text)
{
    const rpod_metrics_t *m = rpod_metrics();
    h.face = FACE_MESSAGE;
    set_hidden(h.bar, true);
    set_hidden(h.text, false);
    int32_t icon_w = set_icon(icon, 100, false);
    lv_label_set_text(h.text, text);

    lv_point_t size;
    lv_text_get_size(&size, text, m->font_small, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t w = HUD_PAD_L + icon_w + HUD_GAP + size.x + HUD_PAD_R;
    w = w < HUD_MSG_MIN_W ? HUD_MSG_MIN_W : w > HUD_MSG_MAX_W ? HUD_MSG_MAX_W : w;
    show(w, HUD_MSG_HOLD_MS);
}

/* Tucked fully away: stop drawing it at all. */
static void hide_done_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(h.pill, LV_OBJ_FLAG_HIDDEN);
}

static void hide_timer_cb(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    if (h.queued) {
        h.queued = false;
        show_message(h.queued_icon, h.queued_text);
        return;
    }
    h.shown = false;
    start_anim(anim_y_cb, h.y, HUD_Y_HIDDEN, HUD_OUT_MS, lv_anim_path_ease_in, hide_done_cb);
    start_anim(anim_opa_cb, h.opa, LV_OPA_TRANSP, HUD_OUT_MS, lv_anim_path_ease_in, NULL);
    start_anim(anim_w_cb, h.base_w, HUD_START_W, HUD_OUT_MS, lv_anim_path_ease_in, NULL);
}

void rpod_hud_init(void)
{
    if (h.created) {
        return;
    }
    h.created = true;
    const rpod_metrics_t *m = rpod_metrics();

    h.pill = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(h.pill);
    rpod_theme_style_glass_panel(h.pill, LV_RADIUS_CIRCLE);
    /* Solid rather than the usual translucent glass: the pill sits over the
     * status bar's title, which ghosts through even a ~92% fill (most
     * visibly across the bar's light track). Fading the whole pill in still
     * cross-fades the title out underneath it. */
    lv_obj_set_style_bg_opa(h.pill, LV_OPA_COVER, 0);
    lv_obj_set_height(h.pill, HUD_H);
    lv_obj_remove_flag(h.pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(h.pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(h.pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h.pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(h.pill, HUD_PAD_L, 0);
    lv_obj_set_style_pad_right(h.pill, HUD_PAD_R, 0);
    lv_obj_set_style_pad_column(h.pill, HUD_GAP, 0);

    h.icon_w = widest_glyph_w(m->font_small);
    h.icon = lv_label_create(h.pill);
    lv_obj_set_style_text_font(h.icon, m->font_small, 0);
    lv_obj_set_style_text_color(h.icon, RPOD_COLOR_TEXT, 0);
    lv_obj_set_width(h.icon, h.icon_w);
    lv_label_set_text_static(h.icon, LV_SYMBOL_VOLUME_MAX);

    h.glyph = rpod_airpods_glyph_create(h.pill, HUD_GLYPH_H, RPOD_COLOR_TEXT);
    lv_obj_add_flag(h.glyph, LV_OBJ_FLAG_HIDDEN);

    /* Flex-grown, so the bar is what gives as the pill widens, narrows, or
     * stretches -- the icon and padding stay put. */
    h.bar = lv_bar_create(h.pill);
    lv_obj_remove_style_all(h.bar);
    lv_obj_set_height(h.bar, HUD_BAR_H);
    lv_obj_set_flex_grow(h.bar, 1);
    lv_obj_set_style_radius(h.bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(h.bar, RPOD_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(h.bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(h.bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(h.bar, RPOD_COLOR_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(h.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(h.bar, HUD_BAR_ANIM_MS, LV_PART_MAIN);
    lv_bar_set_range(h.bar, 0, 100);
    h.available = true;

    /* Grown like the bar, and truncated with an ellipsis if a message runs
     * past HUD_MSG_MAX_W -- which takes a fixed height: with its height
     * left to the content, a label wraps instead. */
    h.text = lv_label_create(h.pill);
    lv_obj_set_style_text_font(h.text, m->font_small, 0);
    lv_obj_set_style_text_color(h.text, RPOD_COLOR_TEXT, 0);
    lv_label_set_long_mode(h.text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(h.text, lv_font_get_line_height(m->font_small));
    lv_obj_set_flex_grow(h.text, 1);
    lv_obj_add_flag(h.text, LV_OBJ_FLAG_HIDDEN);

    h.y = HUD_Y_HIDDEN;
    h.opa = LV_OPA_TRANSP;
    h.base_w = HUD_START_W;
    lv_obj_align(h.pill, LV_ALIGN_TOP_MID, 0, h.y);
    lv_obj_set_style_opa(h.pill, (lv_opa_t)h.opa, 0);
    apply_geometry();
    lv_obj_add_flag(h.pill, LV_OBJ_FLAG_HIDDEN);

    h.hide_timer = lv_timer_create(hide_timer_cb, HUD_HOLD_MS, NULL);
    lv_timer_pause(h.hide_timer);
}

void rpod_hud_volume(int percent, rpod_hud_icon_t icon)
{
    if (!h.created) {
        return;
    }
    /* Only just appearing (or coming back from a message): the bar may still
     * hold a level from minutes ago, so jump straight to the current one
     * instead of sweeping from that. Slide between levels while it's up. */
    bool fresh = !h.shown || h.face != FACE_VOLUME;
    bool available = percent >= 0;
    h.face = FACE_VOLUME;
    set_hidden(h.text, true);
    set_hidden(h.bar, false);
    h.available = available;
    set_icon(icon, available ? percent : 100, !available);
    lv_bar_set_value(h.bar, available ? percent : 0, fresh ? LV_ANIM_OFF : LV_ANIM_ON);
    show(HUD_W, HUD_HOLD_MS);
}

void rpod_hud_volume_bump(int dir)
{
    if (!h.created || dir == 0 || h.face != FACE_VOLUME) {
        return;
    }
    /* Turning on past a limit keeps delivering steps; let each stretch play
     * out instead of restarting it every step, which would just pin it
     * stretched. */
    if (lv_anim_get(&h, anim_stretch_cb) != NULL) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &h);
    lv_anim_set_exec_cb(&a, anim_stretch_cb);
    lv_anim_set_values(&a, 0, dir > 0 ? HUD_STRETCH_PX : -HUD_STRETCH_PX);
    lv_anim_set_duration(&a, HUD_STRETCH_OUT_MS);
    lv_anim_set_reverse_duration(&a, HUD_STRETCH_BACK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

bool rpod_hud_volume_shown(void)
{
    return h.created && h.shown && h.face == FACE_VOLUME;
}

void rpod_hud_message(rpod_hud_icon_t icon, const char *text)
{
    if (!h.created) {
        return;
    }
    if (rpod_hud_volume_shown()) {
        h.queued = true;
        h.queued_icon = icon;
        snprintf(h.queued_text, sizeof(h.queued_text), "%s", text);
        return;
    }
    show_message(icon, text);
}
