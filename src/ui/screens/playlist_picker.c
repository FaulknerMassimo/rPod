#include "playlist_picker.h"

#include "sheet.h"
#include "ui/classic.h"
#include "ui/heart_icon.h"
#include "ui/metrics.h"
#include "ui/theme.h"
#include "audio/mpd_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pk_state pk_state_t;

typedef struct {
    pk_state_t *st;
    char name[256];
    bool is_liked;
    bool member;
    lv_obj_t *mark;  /* shown while a member: a check, or classic's heart for Liked Songs */
    lv_obj_t *heart; /* glass's heart control -- the Liked Songs row */
} pk_row_t;

struct pk_state {
    rpod_mpd_t *mpd;
    char *uri;
    char *title;
    pk_row_t *rows;
    size_t count;
};

static void picker_cleanup_cb(lv_event_t *e)
{
    pk_state_t *st = lv_event_get_user_data(e);
    free(st->rows);
    free(st->uri);
    free(st->title);
    free(st);
}

static void row_set_member(pk_row_t *row, bool member, bool animate)
{
    row->member = member;
    if (row->heart != NULL) {
        rpod_heart_set_liked(row->heart, member, animate);
    } else if (row->mark != NULL) {
        if (member) {
            lv_obj_remove_flag(row->mark, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(row->mark, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void row_click_cb(lv_event_t *e)
{
    pk_row_t *row = lv_event_get_user_data(e);
    pk_state_t *st = row->st;

    bool ok;
    if (row->member) {
        ok = rpod_mpd_playlist_remove_song(st->mpd, row->name, st->uri);
    } else {
        ok = rpod_mpd_playlist_add_song(st->mpd, row->name, st->uri);
    }
    if (ok) {
        row_set_member(row, !row->member, true);
    }
}

/* Classic: the heart or check in the row's text colour, inverted with the
 * selection (list_screen.c's rows do the same). */
static void classic_row_colors(lv_obj_t *btn, pk_row_t *row)
{
    lv_color_t c = lv_obj_has_state(btn, LV_STATE_FOCUSED) ? RPOD_COLOR_SEL_TEXT : RPOD_COLOR_TEXT;
    rpod_classic_icon_set_color(row->mark, c);
}

static void classic_row_focus_cb(lv_event_t *e)
{
    classic_row_colors(lv_event_get_target(e), lv_event_get_user_data(e));
}

/* The classic themes' row: list_screen.c's classic row, with its marks --
 * the iPod-style pixel heart on Liked Songs, a check on the rest, either
 * there only while the song is in the playlist. */
static lv_obj_t *add_picker_row_classic(lv_obj_t *list, pk_row_t *row)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    rpod_classic_style_row(btn);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, row->name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_height(label, lv_font_get_line_height(m->font_body));

    row->mark = rpod_classic_icon_create(btn, row->is_liked ? RPOD_ICON_HEART : RPOD_ICON_CHECK,
                                         RPOD_COLOR_TEXT);
    if (!row->member) {
        lv_obj_add_flag(row->mark, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, row);
    lv_obj_add_event_cb(btn, classic_row_focus_cb, LV_EVENT_FOCUSED, row);
    lv_obj_add_event_cb(btn, classic_row_focus_cb, LV_EVENT_DEFOCUSED, row);
    /* The first row is focused as it joins the group, with no FOCUSED
     * event to follow. */
    classic_row_colors(btn, row);
    return btn;
}

/* iOS-style row shared with list_screen.c's look: transparent until focused,
 * accent-blue highlight, a hairline separator between rows. Metrics-driven
 * (not the picker's own constants) so rows match list_screen.c's. */
static lv_obj_t *add_picker_row(lv_obj_t *list, pk_row_t *row, bool is_last)
{
    if (rpod_theme()->classic) {
        return add_picker_row_classic(list, row);
    }
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_SEL_TEXT, LV_STATE_FOCUSED);
    rpod_theme_style_row_focus(btn);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    if (!is_last) {
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, RPOD_COLOR_SEPARATOR, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_pad_hor(btn, m->row_pad_x, 0);
    lv_obj_set_style_pad_ver(btn, m->row_pad_y, 0);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, row->name);
    lv_obj_set_style_text_font(label, m->font_body, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_width(label, LV_SIZE_CONTENT);

    if (row->is_liked) {
        row->heart = rpod_heart_create(btn, m->row_heart_size);
        rpod_heart_set_liked(row->heart, row->member, false);
    } else {
        row->mark = lv_label_create(btn);
        lv_label_set_text(row->mark, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(row->mark, RPOD_COLOR_TEXT, 0);
        if (!row->member) {
            lv_obj_add_flag(row->mark, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, row);
    return btn;
}

/* A genuine floating popup: the shared glass sheet (sheet.h) over the screen
 * the hold came from, with the playlists as its rows. */
static void build_picker_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    pk_state_t *st = ctx;
    lv_obj_t *list = rpod_sheet_create(screen, "Add to Playlist", st->title);

    for (size_t i = 0; i < st->count; i++) {
        add_picker_row(list, &st->rows[i], i == st->count - 1);
    }

    lv_obj_add_event_cb(screen, picker_cleanup_cb, LV_EVENT_DELETE, st);
}

void rpod_playlist_picker_push(rpod_screen_stack_t *stack, rpod_mpd_t *mpd,
                                const char *uri, const char *title)
{
    pk_state_t *st = calloc(1, sizeof(*st));
    st->mpd = mpd;
    st->uri = strdup(uri);
    st->title = strdup(title != NULL && title[0] != '\0' ? title : uri);

    /* "Liked Songs" always leads, whether or not it exists server-side yet;
     * the rest of the stored playlists follow (skipping a real "Liked Songs"
     * so it isn't listed twice). */
    rpod_mpd_item_t *playlists = NULL;
    size_t n = 0;
    rpod_mpd_list_playlists(mpd, &playlists, &n);

    st->rows = calloc(n + 1, sizeof(*st->rows));
    st->count = 0;

    pk_row_t *liked = &st->rows[st->count++];
    liked->st = st;
    liked->is_liked = true;
    snprintf(liked->name, sizeof(liked->name), "%s", RPOD_LIKED_PLAYLIST_NAME);
    rpod_mpd_playlist_contains(mpd, RPOD_LIKED_PLAYLIST_NAME, uri, &liked->member);

    for (size_t i = 0; i < n; i++) {
        if (strcmp(playlists[i].name, RPOD_LIKED_PLAYLIST_NAME) == 0) {
            continue;
        }
        pk_row_t *row = &st->rows[st->count++];
        row->st = st;
        row->is_liked = false;
        snprintf(row->name, sizeof(row->name), "%s", playlists[i].name);
        rpod_mpd_playlist_contains(mpd, row->name, uri, &row->member);
    }
    rpod_mpd_free_items(playlists);

    rpod_screen_stack_open_overlay(stack, build_picker_screen, st);
}
