#include "spinner.h"

#include "big_digits.h"
#include "classic.h"
#include "metrics.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* A wrong combination's shake: how far, and for how long. */
#define SHAKE_PX 14
#define SHAKE_MS 420

typedef struct {
    rpod_spinner_t *sp;
    void (*done)(void *ctx);
    void *ctx;
    lv_obj_t *up[RPOD_SPINNER_MAX_FIELDS];
    lv_obj_t *box[RPOD_SPINNER_MAX_FIELDS];
    lv_obj_t *digits[RPOD_SPINNER_MAX_FIELDS];
    lv_obj_t *dot[RPOD_SPINNER_MAX_FIELDS];
    lv_obj_t *down[RPOD_SPINNER_MAX_FIELDS];
} spinner_view_t;

static int32_t box_pad_x(void)
{
    return rpod_theme()->classic ? 3 * RPOD_CLASSIC_PX : 8;
}

static int32_t box_pad_y(void)
{
    return rpod_theme()->classic ? 2 * RPOD_CLASSIC_PX : 4;
}

/* An arrow above or below a field: the iPod's, or a symbol. */
static lv_obj_t *arrow(lv_obj_t *parent, bool up)
{
    if (rpod_theme()->classic) {
        return rpod_classic_icon_create(parent, up ? RPOD_ICON_SPIN_UP : RPOD_ICON_SPIN_DOWN, RPOD_COLOR_TEXT);
    }
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(l, rpod_metrics()->font_small, 0);
    lv_obj_set_style_text_color(l, RPOD_COLOR_DIM_TEXT, 0);
    return l;
}

/* Only on a change: hiding an object invalidates it, hidden already or not,
 * and refreshes come every step of the wheel. */
static void show(lv_obj_t *obj, bool shown)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != shown) {
        return;
    }
    if (shown) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void rpod_spinner_refresh(lv_obj_t *spinner)
{
    spinner_view_t *v = lv_obj_get_user_data(spinner);
    const rpod_spinner_t *sp = v->sp;
    for (int i = 0; i < sp->count; i++) {
        const rpod_spinner_field_t *f = &sp->field[i];
        bool active = i == sp->active;
        bool masked = sp->mask && i < sp->active;

        char text[8];
        snprintf(text, sizeof(text), "%0*d", f->digits, f->value);
        rpod_big_digits_set_text(v->digits[i], text);
        show(v->digits[i], !masked);
        show(v->dot[i], masked);

        /* The field being set is the selection: filled in the accent, as a
         * list's selected row is. The arrows stay put on the others,
         * invisible, so nothing shifts as the selection moves on. */
        lv_color_t fg = active ? RPOD_COLOR_SEL_TEXT : RPOD_COLOR_TEXT;
        rpod_big_digits_set_color(v->digits[i], fg);
        lv_obj_set_style_bg_color(v->dot[i], fg, 0);
        if (active) {
            lv_obj_set_style_bg_color(v->box[i], RPOD_COLOR_ACCENT, 0);
            lv_obj_set_style_bg_opa(v->box[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(v->box[i], RPOD_COLOR_GLASS_FILL, 0);
            lv_obj_set_style_bg_opa(v->box[i], RPOD_GLASS_FILL_OPA, 0);
        }
        lv_obj_set_style_opa(v->up[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(v->down[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void step(spinner_view_t *v, int dir)
{
    rpod_spinner_field_t *f = &v->sp->field[v->sp->active];
    int span = f->max - f->min + 1;
    f->value = f->min + ((f->value - f->min + dir) % span + span) % span;
}

static void event_cb(lv_event_t *e)
{
    lv_obj_t *spinner = lv_event_get_current_target(e);
    spinner_view_t *v = lv_event_get_user_data(e);
    rpod_spinner_t *sp = v->sp;

    if (lv_event_get_code(e) == LV_EVENT_KEY) {
        uint32_t k = lv_event_get_key(e);
        if (k == LV_KEY_RIGHT || k == LV_KEY_UP) {
            step(v, +1);
        } else if (k == LV_KEY_LEFT || k == LV_KEY_DOWN) {
            step(v, -1);
        } else {
            return;
        }
        rpod_spinner_refresh(spinner);
        return;
    }

    /* Center: on to the next field, or done after the last. */
    if (sp->active + 1 < sp->count) {
        sp->active++;
        rpod_spinner_refresh(spinner);
    } else if (v->done != NULL) {
        v->done(v->ctx); /* may delete the spinner: nothing after this */
    }
}

static void delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* A field: its arrows, and the box its number shows in. */
static void add_field(spinner_view_t *v, lv_obj_t *parent, int i)
{
    const rpod_spinner_field_t *f = &v->sp->field[i];
    bool classic = rpod_theme()->classic;

    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, classic ? 2 * RPOD_CLASSIC_PX : 4, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    v->up[i] = arrow(col, true);

    /* Sized for its widest number, so a mask or a narrower digit can't
     * change its width. */
    char widest[8];
    snprintf(widest, sizeof(widest), "%0*d", f->digits, 0);
    lv_obj_t *box = lv_obj_create(col);
    lv_obj_remove_style_all(box);
    rpod_theme_style_glass_panel(box, 12);
    lv_obj_set_size(box, rpod_big_digits_width(widest) + 2 * box_pad_x(),
                    rpod_big_digits_height() + 2 * box_pad_y());
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    v->box[i] = box;

    v->digits[i] = rpod_big_digits_create(box);
    lv_obj_center(v->digits[i]);

    int32_t dot = classic ? 5 * RPOD_CLASSIC_PX : 14;
    v->dot[i] = lv_obj_create(box);
    lv_obj_remove_style_all(v->dot[i]);
    lv_obj_set_size(v->dot[i], dot, dot);
    lv_obj_set_style_radius(v->dot[i], classic ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(v->dot[i], LV_OPA_COVER, 0);
    lv_obj_center(v->dot[i]);

    v->down[i] = arrow(col, false);
}

lv_obj_t *rpod_spinner_create(lv_obj_t *parent, rpod_spinner_t *sp, void (*done)(void *ctx), void *ctx)
{
    spinner_view_t *v = calloc(1, sizeof(*v));
    v->sp = sp;
    v->done = done;
    v->ctx = ctx;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, rpod_theme()->classic ? 3 * RPOD_CLASSIC_PX : 8, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_user_data(obj, v);

    for (int i = 0; i < sp->count && i < RPOD_SPINNER_MAX_FIELDS; i++) {
        if (i > 0 && sp->time) {
            lv_obj_t *colon = rpod_big_digits_create(obj);
            rpod_big_digits_set_text(colon, ":");
        }
        add_field(v, obj, i);
    }
    rpod_spinner_refresh(obj);

    /* The wheel: edit mode turns rotation into LV_EVENT_KEY here rather
     * than moving focus, and as the spinner is neither editable nor
     * scrollable Center still arrives as a click (Now Playing's proxy works
     * the same way). CLICKED, not SHORT_CLICKED: done() may delete the
     * spinner, and LVGL follows SHORT_CLICKED with SINGLE/DOUBLE_CLICKED to
     * the same object -- CLICKED comes last. */
    lv_obj_add_event_cb(obj, event_cb, LV_EVENT_KEY, v);
    lv_obj_add_event_cb(obj, event_cb, LV_EVENT_CLICKED, v);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, v);
    lv_group_t *g = lv_group_get_default();
    if (g != NULL) {
        lv_group_add_obj(g, obj);
        lv_group_focus_obj(obj);
        lv_group_set_editing(g, true);
    }
    return obj;
}

/* A decaying side-to-side wobble, from the animation's 0..1000. */
static void shake_cb(void *obj, int32_t t)
{
    float k = (float)t / 1000.0f;
    float x = (float)SHAKE_PX * sinf(k * 4.0f * 3.14159265f) * (1.0f - k);
    lv_obj_set_style_translate_x(obj, (int32_t)lroundf(x), 0);
}

void rpod_spinner_shake(lv_obj_t *spinner)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, spinner);
    lv_anim_set_exec_cb(&a, shake_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, SHAKE_MS);
    lv_anim_start(&a);
}
