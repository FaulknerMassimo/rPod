#include "clock_screens.h"

#include "list_screen.h"
#include "live_list.h"
#include "settings_screens.h"
#include "audio/mpd_client.h"
#include "ui/alarm.h"
#include "ui/big_digits.h"
#include "ui/classic.h"
#include "ui/metrics.h"
#include "ui/sleep_timer.h"
#include "ui/spinner.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The face's padding above and below the time. */
#define FACE_PAD_Y 8

/* "07:30", "Snoozed" or "Off": the alarm as its row shows it. */
static void alarm_value(char *out, size_t size)
{
    if (rpod_alarm_snoozed_until() != 0) {
        snprintf(out, size, "Snoozed");
    } else if (rpod_alarm_enabled()) {
        int h, m;
        rpod_alarm_time(&h, &m);
        snprintf(out, size, "%02d:%02d", h, m);
    } else {
        snprintf(out, size, "Off");
    }
}

/* The sleep timer's minutes left, rounded up; 0 when it's off. */
static unsigned sleep_minutes(void)
{
    return (unsigned)((rpod_sleep_timer_remaining_ms() + 59999u) / 60000u);
}

/* --- Clock ------------------------------------------------------------------- */

typedef struct {
    rpod_mpd_t *mpd;
    rpod_live_list_t *ll;
    lv_timer_t *timer;
    lv_obj_t *time;   /* this fill's; NULL before the first */
    lv_obj_t *date;
    char shown_alarm[16];
    unsigned shown_sleep;
} clock_view_t;

static void update_face(clock_view_t *cv)
{
    if (cv->time == NULL) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    rpod_big_digits_set_text(cv->time, buf);
    /* Chicago at 2x fits the short form across the panel, not the long. */
    strftime(buf, sizeof(buf), rpod_theme()->classic ? "%a %-d %b %Y" : "%A %-d %B", &tm);
    if (strcmp(lv_label_get_text(cv->date), buf) != 0) {
        lv_label_set_text(cv->date, buf);
    }
}

static void on_alarm_clock(rpod_screen_stack_t *stack, void *item_ctx);

static void on_sleep_timer(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, rpod_sleep_timer_screen_build, NULL, NULL);
}

static void fill_clock(rpod_live_list_t *ll)
{
    clock_view_t *cv = rpod_live_list_ctx(ll);
    const rpod_metrics_t *m = rpod_metrics();

    lv_obj_t *face = rpod_live_list_block(ll);
    lv_obj_set_flex_align(face, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(face, FACE_PAD_Y, 0);
    cv->time = rpod_big_digits_create(face);
    cv->date = lv_label_create(face);
    lv_label_set_text(cv->date, "");
    lv_obj_set_style_text_font(cv->date, rpod_theme()->classic ? m->font_body : m->font_small, 0);
    lv_obj_set_style_text_color(cv->date, RPOD_COLOR_DIM_TEXT, 0);
    update_face(cv);

    alarm_value(cv->shown_alarm, sizeof(cv->shown_alarm));
    cv->shown_sleep = sleep_minutes();

    rpod_list_item_t *it = rpod_live_list_add(ll, "alarm", on_alarm_clock);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Alarm Clock");
        snprintf(it->accessory, sizeof(it->accessory), "%s", cv->shown_alarm);
        it->chevron = true;
    }
    it = rpod_live_list_add(ll, "sleep", on_sleep_timer);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Sleep Timer");
        if (cv->shown_sleep != 0) {
            snprintf(it->accessory, sizeof(it->accessory), "%u min", cv->shown_sleep);
        } else {
            snprintf(it->accessory, sizeof(it->accessory), "Off");
        }
        it->chevron = true;
    }
}

/* Every second: the face, and the rows when what they say has changed. */
static void clock_tick_cb(lv_timer_t *t)
{
    clock_view_t *cv = lv_timer_get_user_data(t);
    char alarm[sizeof(cv->shown_alarm)];
    alarm_value(alarm, sizeof(alarm));
    if (strcmp(alarm, cv->shown_alarm) != 0 || sleep_minutes() != cv->shown_sleep) {
        rpod_live_list_changed(cv->ll);
    } else {
        update_face(cv);
    }
}

/* Back from Alarm Clock or Sleep Timer: they may have changed. */
static void clock_loaded_cb(lv_event_t *e)
{
    clock_view_t *cv = lv_event_get_user_data(e);
    rpod_live_list_changed(cv->ll);
}

static void clock_view_free(void *ctx)
{
    clock_view_t *cv = ctx;
    lv_timer_delete(cv->timer);
    free(cv);
}

void rpod_clock_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    clock_view_t *cv = calloc(1, sizeof(*cv));
    if (cv == NULL) {
        return;
    }
    cv->mpd = ctx;
    cv->timer = lv_timer_create(clock_tick_cb, 1000, cv);
    cv->ll = rpod_live_list_create(stack, screen, fill_clock, cv, clock_view_free);
    if (cv->ll != NULL) { /* else create has freed cv already */
        lv_obj_add_event_cb(screen, clock_loaded_cb, LV_EVENT_SCREEN_LOADED, cv);
    }
}

/* --- Alarm Clock: on or off, its time, and what it plays -------------------- */

static void build_alarm_time_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);
static void build_alarm_sound_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_spinner_t sp;
} time_entry_t;

static void on_alarm_toggle(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    rpod_alarm_set_enabled(!rpod_alarm_enabled());
    rpod_live_list_changed(ref->ll);
}

static void on_alarm_time(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    time_entry_t *te = calloc(1, sizeof(*te));
    if (te == NULL) {
        return;
    }
    int h, m;
    rpod_alarm_time(&h, &m);
    te->stack = stack;
    te->sp = (rpod_spinner_t){
        .field = { { .min = 0, .max = 23, .value = h, .digits = 2 },
                   { .min = 0, .max = 59, .value = m, .digits = 2 } },
        .count = 2,
        .time = true,
    };
    rpod_screen_stack_push(stack, build_alarm_time_screen, te, free);
}

static void on_alarm_sound(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    rpod_screen_stack_push(stack, build_alarm_sound_screen, rpod_live_list_ctx(ref->ll), NULL);
}

static void fill_alarm(rpod_live_list_t *ll)
{
    rpod_list_item_t *it = rpod_live_list_add(ll, "on", on_alarm_toggle);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Alarm");
        snprintf(it->accessory, sizeof(it->accessory), "%s", rpod_alarm_enabled() ? "On" : "Off");
    }
    it = rpod_live_list_add(ll, "time", on_alarm_time);
    if (it != NULL) {
        int h, m;
        rpod_alarm_time(&h, &m);
        snprintf(it->text, sizeof(it->text), "Time");
        snprintf(it->accessory, sizeof(it->accessory), "%02d:%02d", h, m);
        it->chevron = true;
    }
    it = rpod_live_list_add(ll, "sound", on_alarm_sound);
    if (it != NULL) {
        const char *sound = rpod_alarm_sound();
        snprintf(it->text, sizeof(it->text), "Sound");
        snprintf(it->accessory, sizeof(it->accessory), "%s", sound[0] != '\0' ? sound : "Shuffle Songs");
        it->chevron = true;
    }
}

static void list_loaded_cb(lv_event_t *e)
{
    rpod_live_list_changed(lv_event_get_user_data(e));
}

static void build_alarm_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill_alarm, ctx, NULL);
    if (ll != NULL) {
        lv_obj_add_event_cb(screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, ll);
    }
}

static void on_alarm_clock(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    clock_view_t *cv = rpod_live_list_ctx(ref->ll);
    rpod_screen_stack_push(stack, build_alarm_screen, cv->mpd, NULL);
}

/* Setting the time sets the alarm too: why else set it? */
static void time_done(void *ctx)
{
    time_entry_t *te = ctx;
    rpod_alarm_set_time(te->sp.field[0].value, te->sp.field[1].value);
    rpod_alarm_set_enabled(true);
    rpod_screen_stack_pop(te->stack); /* frees te */
}

static void build_alarm_time_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    time_entry_t *te = ctx;
    rpod_screen_stack_set_title(stack, "Alarm Time");
    lv_obj_t *sp = rpod_spinner_create(screen, &te->sp, time_done, te);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, rpod_metrics()->header_h / 2);
}

/* What it plays: Shuffle Songs, or one of the stored playlists (Liked Songs
 * first), checked. Row keys are the playlist's name, "" for Shuffle Songs. */
static void on_pick_sound(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    rpod_alarm_set_sound(ref->key);
    rpod_live_list_changed(ref->ll);
}

static void add_sound(rpod_live_list_t *ll, const char *name, const char *label)
{
    rpod_list_item_t *it = rpod_live_list_add(ll, name, on_pick_sound);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "%s", label);
        it->status = strcmp(rpod_alarm_sound(), name) == 0 ? RPOD_ROW_STATUS_CHECK : RPOD_ROW_STATUS_NONE;
    }
}

static void fill_sound(rpod_live_list_t *ll)
{
    rpod_mpd_t *mpd = rpod_live_list_ctx(ll);
    add_sound(ll, "", "Shuffle Songs");

    rpod_mpd_item_t *playlists = NULL;
    size_t n = 0;
    rpod_mpd_list_playlists(mpd, &playlists, &n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(playlists[i].name, RPOD_LIKED_PLAYLIST_NAME) == 0) {
            add_sound(ll, playlists[i].name, playlists[i].name);
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(playlists[i].name, RPOD_LIKED_PLAYLIST_NAME) != 0) {
            add_sound(ll, playlists[i].name, playlists[i].name);
        }
    }
    rpod_mpd_free_items(playlists);
}

static void build_alarm_sound_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    rpod_live_list_create(stack, screen, fill_sound, ctx, NULL);
}

/* --- The alarm going off ------------------------------------------------------ */

/* Same scrim as the playlist picker's: the screen underneath stays
 * recognisable, but the card reads as modal. */
#define CARD_SCRIM_OPA 110
#define CARD_W         232

static void on_snooze(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_alarm_snooze();
    rpod_screen_stack_close_overlay(stack);
}

static void on_stop(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_alarm_stop();
    rpod_screen_stack_close_overlay(stack);
}

/* Gone some other way -- Menu, or a theme change: the music plays on. */
static void card_delete_cb(lv_event_t *e)
{
    (void)e;
    if (rpod_alarm_ringing()) {
        rpod_alarm_dismiss();
    }
}

/* The bell: the iPod's own in the classic themes, else a symbol. */
static lv_obj_t *bell(lv_obj_t *parent)
{
    if (rpod_theme()->classic && rpod_classic_has_assets()) {
        return rpod_classic_icon_create(parent, RPOD_ICON_ALARM, RPOD_COLOR_TEXT);
    }
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(l, rpod_metrics()->font_np_glyph, 0);
    lv_obj_set_style_text_color(l, RPOD_COLOR_TEXT, 0);
    return l;
}

static void build_alarm_card(rpod_screen_stack_t *stack, lv_obj_t *root, void *ctx)
{
    (void)ctx;
    const rpod_metrics_t *m = rpod_metrics();
    bool classic = rpod_theme()->classic;

    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, classic ? LV_OPA_TRANSP : CARD_SCRIM_OPA, 0);

    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, LV_SIZE_CONTENT);
    rpod_theme_style_glass_panel(card, 16);
    if (!classic) {
        lv_obj_set_style_bg_opa(card, LV_OPA_90, 0); /* over the live screen: see playlist_picker.c */
    }
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, m->header_h / 2);

    /* The bell, "Alarm" and its time. */
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(head, classic ? 4 * RPOD_CLASSIC_PX : 14, 0);
    lv_obj_set_style_pad_column(head, classic ? 4 * RPOD_CLASSIC_PX : 12, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, classic ? RPOD_CLASSIC_PX : 1, 0);
    lv_obj_set_style_border_color(head, classic ? RPOD_COLOR_TEXT : RPOD_COLOR_SEPARATOR, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    bell(head);

    lv_obj_t *text = lv_obj_create(head);
    lv_obj_remove_style_all(text);
    lv_obj_set_size(text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(text);
    lv_label_set_text(title, "Alarm");
    lv_obj_set_style_text_font(title, m->font_title, 0);
    lv_obj_set_style_text_color(title, RPOD_COLOR_TEXT, 0);
    int h, mi;
    rpod_alarm_time(&h, &mi);
    lv_obj_t *when = lv_label_create(text);
    lv_label_set_text_fmt(when, "%02d:%02d", h, mi);
    lv_obj_set_style_text_font(when, m->font_body, 0);
    lv_obj_set_style_text_color(when, RPOD_COLOR_DIM_TEXT, 0);

    /* Snooze or Stop: the rows of the lists everywhere else. */
    lv_obj_t *list = lv_list_create(card);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    rpod_list_item_t items[2] = { 0 };
    snprintf(items[0].text, sizeof(items[0].text), "Snooze");
    snprintf(items[0].accessory, sizeof(items[0].accessory), "%d min", RPOD_ALARM_SNOOZE_MIN);
    items[0].on_select = on_snooze;
    snprintf(items[1].text, sizeof(items[1].text), "Stop");
    items[1].on_select = on_stop;
    rpod_list_screen_populate(stack, list, items, 2);

    lv_obj_add_event_cb(root, card_delete_cb, LV_EVENT_DELETE, NULL);
}

void rpod_alarm_card_open(rpod_screen_stack_t *stack)
{
    rpod_screen_stack_close_overlay(stack);
    rpod_screen_stack_open_overlay(stack, build_alarm_card, NULL);
}
