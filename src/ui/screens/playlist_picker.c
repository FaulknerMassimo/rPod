#include "playlist_picker.h"

#include "ui/heart_icon.h"
#include "ui/theme.h"
#include "audio/mpd_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PICKER_HEART_SIZE 22

typedef struct pk_state pk_state_t;

typedef struct {
    pk_state_t *st;
    char name[256];
    bool is_liked;
    bool member;
    lv_obj_t *check; /* LV_SYMBOL_OK label -- non-liked rows */
    lv_obj_t *heart; /* heart control -- the Liked Songs row */
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
    } else if (row->check != NULL) {
        if (member) {
            lv_obj_remove_flag(row->check, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(row->check, LV_OBJ_FLAG_HIDDEN);
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

/* iOS-style row shared with list_screen.c's look: transparent until focused,
 * accent-blue highlight, a hairline separator between rows. */
static lv_obj_t *add_picker_row(lv_obj_t *list, pk_row_t *row, bool is_last)
{
    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    if (!is_last) {
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, RPOD_COLOR_SEPARATOR, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, row->name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_width(label, LV_SIZE_CONTENT);

    if (row->is_liked) {
        row->heart = rpod_heart_create(btn, PICKER_HEART_SIZE);
        rpod_heart_set_liked(row->heart, row->member, false);
    } else {
        row->check = lv_label_create(btn);
        lv_label_set_text(row->check, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(row->check, RPOD_COLOR_TEXT, 0);
        if (!row->member) {
            lv_obj_add_flag(row->check, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, row);
    return btn;
}

static void build_picker_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    pk_state_t *st = ctx;

    /* --- Header: what we're adding, and to where. --- */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Add to Playlist");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, RPOD_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, RPOD_HEADER_HEIGHT + 8);

    lv_obj_t *song = lv_label_create(screen);
    lv_label_set_text(song, st->title);
    lv_obj_set_style_text_color(song, RPOD_COLOR_DIM_TEXT, 0);
    lv_label_set_long_mode(song, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(song, RPOD_SCREEN_WIDTH - 28);
    lv_obj_align(song, LV_ALIGN_TOP_LEFT, 14, RPOD_HEADER_HEIGHT + 30);

    /* --- Playlist list below the header. --- */
    int list_top = RPOD_HEADER_HEIGHT + 52;
    lv_obj_t *list = lv_list_create(screen);
    lv_obj_set_size(list, RPOD_SCREEN_WIDTH - 16, RPOD_SCREEN_HEIGHT - list_top - 8);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);
    rpod_theme_style_glass_panel(list, 12);
    lv_obj_set_style_clip_corner(list, true, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

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

    rpod_screen_stack_push(stack, build_picker_screen, st, NULL);
}
