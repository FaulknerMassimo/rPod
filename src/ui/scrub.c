#include "scrub.h"

#include "metrics.h"
#include "theme.h"

LV_FONT_DECLARE(rpod_font_letters_64)

/* A rounded tile in the middle of the list area, opaque enough that the
 * letter reads over the rows scrolling beneath it. No shadow: it's redrawn
 * with every jump, and a soft shadow is the expensive part on the Pi. */
#define TILE_SIZE    96
#define TILE_RADIUS  20
#define TILE_OPA     235
#define HOLD_MS      700 /* after the last jump, before it fades */
#define FADE_OUT_MS  180

static struct {
    lv_obj_t *tile;
    lv_obj_t *label;
    lv_timer_t *hide_timer;
    uint32_t event;
} s;

uint32_t rpod_scrub_event(void)
{
    if (s.event == 0) {
        s.event = lv_event_register_id();
    }
    return s.event;
}

static void opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void faded_cb(lv_anim_t *a)
{
    lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void hide_cb(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s.tile);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, lv_obj_get_style_opa(s.tile, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FADE_OUT_MS);
    lv_anim_set_completed_cb(&a, faded_cb);
    lv_anim_start(&a);
}

void rpod_scrub_init(void)
{
    const rpod_metrics_t *m = rpod_metrics();

    s.tile = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s.tile);
    lv_obj_set_size(s.tile, TILE_SIZE, TILE_SIZE);
    /* Centred on the list area below the status bar, not the whole screen. */
    lv_obj_align(s.tile, LV_ALIGN_CENTER, 0, m->header_h / 2);
    lv_obj_set_style_radius(s.tile, TILE_RADIUS, 0);
    lv_obj_set_style_bg_color(s.tile, RPOD_COLOR_HEADER_BG, 0);
    lv_obj_set_style_bg_opa(s.tile, TILE_OPA, 0);
    lv_obj_set_style_border_color(s.tile, RPOD_COLOR_GLASS_EDGE, 0);
    lv_obj_set_style_border_opa(s.tile, RPOD_GLASS_EDGE_OPA, 0);
    lv_obj_set_style_border_width(s.tile, 1, 0);
    lv_obj_remove_flag(s.tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s.tile, LV_OBJ_FLAG_HIDDEN);

    s.label = lv_label_create(s.tile);
    lv_obj_set_style_text_font(s.label, &rpod_font_letters_64, 0);
    lv_obj_set_style_text_color(s.label, RPOD_COLOR_TEXT, 0);
    lv_obj_center(s.label);

    s.hide_timer = lv_timer_create(hide_cb, HOLD_MS, NULL);
    lv_timer_pause(s.hide_timer);
}

static void show_letter(char letter)
{
    char text[2] = { letter, '\0' };
    lv_label_set_text(s.label, text);

    lv_anim_delete(s.tile, opa_cb);
    lv_obj_set_style_opa(s.tile, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s.tile, LV_OBJ_FLAG_HIDDEN);

    lv_timer_reset(s.hide_timer);
    lv_timer_resume(s.hide_timer);
}

bool rpod_scrub(int dir)
{
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        return false;
    }
    rpod_scrub_param_t param = { .dir = dir };
    lv_obj_send_event(screen, rpod_scrub_event(), &param);
    if (!param.handled) {
        return false;
    }
    if (s.tile != NULL) {
        show_letter(param.letter);
    }
    return true;
}
