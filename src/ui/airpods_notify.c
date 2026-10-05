#include "airpods_notify.h"

#include "airpods_art.h"
#include "hud.h"
#include "metrics.h"
#include "theme.h"
#include "audio/aap.h"
#include "audio/airpods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The card: a glass sheet docked to the bottom of the screen, art for the
 * buds and the case side by side with their batteries underneath. */
#define CARD_W         300
#define CARD_H         134
#define CARD_MARGIN    10
#define CARD_RADIUS    22
/* Nearly solid: at playlist_picker.c's 90% a list's highlighted row still
 * showed through it as a blue band. */
#define CARD_OPA       245
#define CARD_SCRIM_OPA 120
#define CARD_PAD       14
#define CARD_IN_MS     340
#define CARD_SHOW_MS   6000
#define BUDS_ART_H     52
#define CASE_ART_H     40
#define ART_BASELINE   94  /* art bottoms line up here, in card coordinates */
#define BATTERY_Y      102

/* The case coming back (a bud put away) shows the card again, but not more
 * often than this -- nor does the control channel reconnecting. */
#define CARD_REPEAT_MS 30000
/* Controls up but no battery after this long: just say they're connected. */
#define BATTERY_WAIT_MS 4000
#define CHECK_MS        500

#define COLOR_CHARGING lv_color_hex(0x30d158)
#define COLOR_LOW      lv_color_hex(0xff453a)
#define LOW_PERCENT    20

static struct {
    rpod_screen_stack_t *stack;
    rpod_airpods_link_t link;
    char path[64];
    char name[96];

    /* This control session's (READY until it drops). */
    bool was_ready;
    bool card_wanted;     /* show the card once the battery arrives */
    uint32_t ready_at;
    bool mode_known;
    uint8_t mode;
    bool case_known;
    int low_warned;       /* threshold last warned about: 100 none yet, then 20, 10 */

    char card_path[64];   /* which AirPods the card last showed, and when */
    uint32_t card_at;
    lv_obj_t *card_root;  /* the overlay, while the card's up */
    lv_obj_t *card;
    lv_timer_t *card_timer;
} n = { .low_warned = 100 };

/* --- Card -------------------------------------------------------------------- */

static void close_card_async(void *user)
{
    (void)user;
    if (n.card_root != NULL && rpod_screen_stack_overlay(n.stack) == n.card_root) {
        rpod_screen_stack_close_overlay(n.stack);
    }
}

static void card_timeout_cb(lv_timer_t *t)
{
    (void)t;
    n.card_timer = NULL; /* a one-shot: LVGL deletes it after this */
    close_card_async(NULL);
}

static void card_deleted_cb(lv_event_t *e)
{
    (void)e;
    if (n.card_timer != NULL) {
        lv_timer_delete(n.card_timer);
        n.card_timer = NULL;
    }
    lv_async_call_cancel(close_card_async, NULL);
    n.card_root = NULL;
    n.card = NULL;
}

/* Any turn or click dismisses it. CLICKED rather than PRESSED: the click
 * comes with the release, so the screen underneath never sees half a press
 * (see list_screen.c's lv_indev_wait_release() for that hazard). Async,
 * because closing deletes the object this event is on. */
static void proxy_event_cb(lv_event_t *e)
{
    (void)e;
    lv_async_call(close_card_async, NULL);
}

static const char *battery_glyph(int level)
{
    return level >= 85   ? LV_SYMBOL_BATTERY_FULL
           : level >= 60 ? LV_SYMBOL_BATTERY_3
           : level >= 35 ? LV_SYMBOL_BATTERY_2
           : level >= 10 ? LV_SYMBOL_BATTERY_1
                         : LV_SYMBOL_BATTERY_EMPTY;
}

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, rpod_metrics()->font_small, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* "L [battery] 80%" -- green with a bolt while charging, red when low. */
static void add_battery(lv_obj_t *row, const char *tag, const rpod_airpods_battery_t *b)
{
    lv_color_t color = b->charging            ? COLOR_CHARGING
                       : b->level <= LOW_PERCENT ? COLOR_LOW
                                                 : RPOD_COLOR_TEXT;
    lv_obj_t *item = lv_obj_create(row);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 4, 0);
    if (tag != NULL) {
        add_label(item, tag, RPOD_COLOR_DIM_TEXT);
    }
    add_label(item, battery_glyph(b->level), color);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", b->level);
    add_label(item, pct, RPOD_COLOR_TEXT);
    if (b->charging) {
        add_label(item, LV_SYMBOL_CHARGE, COLOR_CHARGING);
    }
}

/* The batteries under one picture, centred on x = `cx` (card coordinates). */
static lv_obj_t *battery_row(lv_obj_t *card, int32_t cx)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, cx - CARD_W / 2, BATTERY_Y);
    return row;
}

static void add_art(lv_obj_t *card, const lv_image_dsc_t *dsc, int32_t cx)
{
    if (dsc == NULL) {
        return;
    }
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, cx - dsc->header.w / 2, ART_BASELINE - dsc->header.h);
}

/* Rebuilds the card's content from the AirPods' current state: called on
 * open and whenever they report something new. */
static void fill_card(void)
{
    const rpod_airpods_t *ap = rpod_airpods();
    lv_obj_t *card = n.card;
    lv_obj_clean(card);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, n.name[0] != '\0' ? n.name : "AirPods");
    lv_obj_set_style_text_font(title, rpod_metrics()->font_body, 0);
    lv_obj_set_style_text_color(title, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title, CARD_W - 2 * CARD_PAD);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, CARD_PAD - 2);

    /* The case only while it's reporting; the buds take the middle then. */
    bool with_case = ap->charging_case.level >= 0;
    int32_t buds_x = with_case ? CARD_W * 30 / 100 : CARD_W / 2;
    int32_t case_x = CARD_W * 73 / 100;

    add_art(card, rpod_airpods_art_buds(BUDS_ART_H), buds_x);
    lv_obj_t *row = battery_row(card, buds_x);
    const rpod_airpods_battery_t *l = &ap->left, *r = &ap->right;
    if (ap->headset.level >= 0) {
        add_battery(row, NULL, &ap->headset);
    } else if (l->level >= 0 && r->level >= 0 && abs(l->level - r->level) <= 10 &&
               l->charging == r->charging) {
        /* Close enough to read as one, the way iOS shows them. */
        add_battery(row, NULL, l->level < r->level ? l : r);
    } else if (l->level >= 0 || r->level >= 0) {
        if (l->level >= 0) {
            add_battery(row, "L", l);
        }
        if (r->level >= 0) {
            add_battery(row, "R", r);
        }
    } else {
        add_label(row, "Connecting...", RPOD_COLOR_DIM_TEXT);
    }

    if (with_case) {
        add_art(card, rpod_airpods_art_case(CASE_ART_H), case_x);
        add_battery(battery_row(card, case_x), NULL, &ap->charging_case);
    }
}

static void card_changed_cb(void *user)
{
    (void)user;
    if (n.card != NULL) {
        fill_card();
    }
}

static void anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y(var, v);
}

static void anim_scrim_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa(var, (lv_opa_t)v, 0);
}

static void build_card(rpod_screen_stack_t *stack, lv_obj_t *root, void *ctx)
{
    (void)stack;
    (void)ctx;
    const rpod_metrics_t *m = rpod_metrics();
    n.card_root = root;

    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);

    n.card = lv_obj_create(root);
    lv_obj_remove_style_all(n.card);
    rpod_theme_style_glass_panel(n.card, CARD_RADIUS);
    lv_obj_set_style_bg_opa(n.card, CARD_OPA, 0);
    lv_obj_set_size(n.card, CARD_W, CARD_H);
    lv_obj_remove_flag(n.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_x(n.card, (m->screen_w - CARD_W) / 2);
    fill_card();

    /* Slides up from below the screen while the scrim fades in. */
    int32_t y_shown = m->screen_h - CARD_MARGIN - CARD_H;
    lv_obj_set_y(n.card, m->screen_h);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, n.card);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, m->screen_h, y_shown);
    lv_anim_set_duration(&a, CARD_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_exec_cb(&a, anim_scrim_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, CARD_SCRIM_OPA);
    lv_anim_start(&a);

    /* The overlay's group needs something focused to receive the wheel: an
     * offscreen proxy in edit mode, so a turn arrives as LV_EVENT_KEY (as
     * on Now Playing). */
    lv_obj_t *proxy = lv_obj_create(root);
    lv_obj_remove_style_all(proxy);
    lv_obj_set_size(proxy, 1, 1);
    lv_obj_remove_flag(proxy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(proxy, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(proxy, proxy_event_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(proxy, proxy_event_cb, LV_EVENT_CLICKED, NULL);
    lv_group_t *group = lv_group_get_default();
    if (group != NULL) {
        lv_group_add_obj(group, proxy);
        lv_group_focus_obj(proxy);
        lv_group_set_editing(group, true);
    }

    rpod_airpods_watch(root, card_changed_cb, NULL);
    lv_obj_add_event_cb(root, card_deleted_cb, LV_EVENT_DELETE, NULL);
    n.card_timer = lv_timer_create(card_timeout_cb, CARD_SHOW_MS, NULL);
    lv_timer_set_repeat_count(n.card_timer, 1);
}

/* The card, or -- with another overlay up (the playlist picker) -- the same
 * news as a HUD message. */
static void show_card(void)
{
    snprintf(n.card_path, sizeof(n.card_path), "%s", n.path);
    n.card_at = lv_tick_get();
    if (n.card_root != NULL) {
        fill_card();
        if (n.card_timer != NULL) {
            lv_timer_reset(n.card_timer);
        }
        return;
    }
    if (rpod_screen_stack_overlay(n.stack) != NULL) {
        const rpod_airpods_t *ap = rpod_airpods();
        int low = -1;
        const rpod_airpods_battery_t *parts[] = { &ap->headset, &ap->left, &ap->right };
        for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
            if (parts[i]->level >= 0 && (low < 0 || parts[i]->level < low)) {
                low = parts[i]->level;
            }
        }
        char text[64];
        if (low >= 0) {
            snprintf(text, sizeof(text), "Connected \xC2\xB7 %d%%", low);
        } else {
            snprintf(text, sizeof(text), "Connected");
        }
        rpod_hud_message(RPOD_HUD_ICON_AIRPODS, text);
        return;
    }
    rpod_screen_stack_open_overlay(n.stack, build_card, NULL);
}

static bool card_shown_lately(void)
{
    return strcmp(n.card_path, n.path) == 0 && lv_tick_elaps(n.card_at) < CARD_REPEAT_MS;
}

/* --- Following the AirPods ---------------------------------------------------- */

static const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case RPOD_AAP_MODE_ANC:          return "Noise Cancellation";
    case RPOD_AAP_MODE_TRANSPARENCY: return "Transparency";
    case RPOD_AAP_MODE_ADAPTIVE:     return "Adaptive";
    case RPOD_AAP_MODE_OFF:          return "Noise Control Off";
    default:                         return NULL;
    }
}

static bool battery_known(const rpod_airpods_t *ap)
{
    return ap->left.level >= 0 || ap->right.level >= 0 || ap->headset.level >= 0;
}

/* The emptiest bud that's in use (not charging in the case), or -1. */
static int lowest_in_use(const rpod_airpods_t *ap)
{
    const rpod_airpods_battery_t *parts[] = { &ap->headset, &ap->left, &ap->right };
    int low = -1;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const rpod_airpods_battery_t *b = parts[i];
        if (b->level >= 0 && !b->charging && (low < 0 || b->level < low)) {
            low = b->level;
        }
    }
    return low;
}

/* Warns once at 20% and again at 10%; charging back up re-arms it. While
 * the card is up its red battery already says so. */
static void check_low(const rpod_airpods_t *ap)
{
    int low = lowest_in_use(ap);
    if (low < 0) {
        return;
    }
    if (low > LOW_PERCENT + 5) {
        n.low_warned = 100;
        return;
    }
    int threshold = low <= 10 ? 10 : LOW_PERCENT;
    if (threshold >= n.low_warned) {
        return;
    }
    n.low_warned = threshold;
    if (n.card_root == NULL && !n.card_wanted) {
        char text[64];
        snprintf(text, sizeof(text), "Battery Low \xC2\xB7 %d%%", low);
        rpod_hud_message(RPOD_HUD_ICON_AIRPODS, text);
    }
}

static void session_start(const rpod_airpods_t *ap)
{
    n.was_ready = true;
    n.ready_at = lv_tick_get();
    n.mode_known = false;
    n.case_known = ap->charging_case.level >= 0;
    n.low_warned = 100;
    /* A dropped control channel coming back isn't news. */
    n.card_wanted = !card_shown_lately();
}

static void changed_cb(void *user)
{
    (void)user;
    const rpod_airpods_t *ap = rpod_airpods();

    if (ap->link == RPOD_AIRPODS_ABSENT && n.link != RPOD_AIRPODS_ABSENT) {
        if (n.was_ready) {
            rpod_hud_message(RPOD_HUD_ICON_AIRPODS, "Disconnected");
        }
        n.was_ready = false;
        n.card_wanted = false;
        /* Not from in here: closing deletes the card's own watcher while
         * audio/airpods.c is walking them. */
        lv_async_call(close_card_async, NULL);
    }
    bool started = ap->link == RPOD_AIRPODS_READY && n.link != RPOD_AIRPODS_READY;
    n.link = ap->link;
    snprintf(n.path, sizeof(n.path), "%s", ap->path);
    if (ap->name[0] != '\0') {
        snprintf(n.name, sizeof(n.name), "%s", ap->name);
    }
    if (ap->link != RPOD_AIRPODS_READY) {
        return;
    }
    if (started) {
        session_start(ap);
    }

    uint8_t mode;
    if (rpod_airpods_get(RPOD_AAP_CTL_LISTENING_MODE, &mode)) {
        const char *name = mode_name(mode);
        if (n.mode_known && mode != n.mode && name != NULL) {
            rpod_hud_message(RPOD_HUD_ICON_AIRPODS, name);
        }
        n.mode_known = true;
        n.mode = mode;
    }

    if (n.card_wanted && battery_known(ap)) {
        n.card_wanted = false;
        show_card();
    }
    /* The case reporting again, well after connecting: a bud went back in
     * (or the lid opened with one inside). */
    bool case_known = ap->charging_case.level >= 0;
    if (case_known && !n.case_known && !n.card_wanted && !card_shown_lately() &&
        lv_tick_elaps(n.ready_at) > BATTERY_WAIT_MS) {
        show_card();
    }
    n.case_known = case_known;

    check_low(ap);
}

static void check_cb(lv_timer_t *t)
{
    (void)t;
    if (n.card_wanted && lv_tick_elaps(n.ready_at) > BATTERY_WAIT_MS) {
        n.card_wanted = false;
        rpod_hud_message(RPOD_HUD_ICON_AIRPODS, "Connected");
    }
}

void rpod_airpods_notify_init(rpod_screen_stack_t *stack)
{
    n.stack = stack;
    n.link = rpod_airpods()->link;
    rpod_airpods_watch(NULL, changed_cb, NULL);
    lv_timer_create(check_cb, CHECK_MS, NULL);
}
