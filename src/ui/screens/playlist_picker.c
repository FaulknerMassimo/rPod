#include "playlist_picker.h"

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
 * accent-blue highlight, a hairline separator between rows. Metrics-driven
 * (not the picker's own constants) so rows shrink to fit the square HAT's
 * tighter popup card the same way list_screen.c's rows do. */
static lv_obj_t *add_picker_row(lv_obj_t *list, pk_row_t *row, bool is_last)
{
    const rpod_metrics_t *m = rpod_metrics();
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

/* Dim scrim opacity for the area outside the sheet -- low enough that the
 * screen underneath (Now Playing's art, a list mid-scroll, whatever was up
 * when the hold fired) stays recognisable, per rpod_screen_stack_open_overlay
 * leaving it loaded and rendering on every refresh; high enough to read as
 * "this is a modal" rather than a stray transparent box. */
#define PICKER_SCRIM_OPA 110 /* ~43% */

/* The sheet itself needs to read clearly as a modal card, not blend into
 * whatever's showing through the scrim behind it -- unlike the other
 * rpod_theme_style_glass_panel() users (list_screen.c, now_playing.c), which
 * sit directly on a flat black screen or are meant to show the blurred art
 * through them, this sheet floats over the *live* previous screen (art,
 * a mid-scroll list, ...) dimmed only by PICKER_SCRIM_OPA above, so the
 * shared RPOD_GLASS_FILL_OPA default (~59%) left it looking washed out /
 * see-through. Overridden here, after rpod_theme_style_glass_panel(), rather
 * than raising the shared constant, since that would also thin out every
 * other glass surface in the app. */
#define PICKER_SHEET_OPA LV_OPA_90 /* ~90% -- opaque enough to read as a solid card */

/* A genuine floating popup, not another full page: `screen` here is the
 * overlay's root (see rpod_screen_stack_open_overlay) -- dimmed but
 * translucent, so the actual screen behind the hold gesture keeps showing
 * through it, with a single "liquid glass" sheet floating on top of that,
 * inset from every edge (including the top, below the status bar). Header
 * text and the playlist list are both children of that one sheet -- the
 * list itself stays plain/transparent rather than being its own nested
 * glass panel, so there's one card, not glass-on-glass. Margins shrink on
 * the square HAT so the sheet still leaves visible backdrop on a 128x128
 * screen instead of eating it entirely. */
static void build_picker_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    pk_state_t *st = ctx;

    const rpod_metrics_t *m = rpod_metrics();
    bool square = m->form == RPOD_FORM_SQUARE;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, PICKER_SCRIM_OPA, 0);

    int side_margin = square ? 8 : 28;
    int top_margin = m->header_h + (square ? 6 : 16);
    int bottom_margin = square ? 6 : 16;
    int pad = square ? 6 : 14;

    int sheet_w = m->screen_w - 2 * side_margin;
    int sheet_h = m->screen_h - top_margin - bottom_margin;

    lv_obj_t *sheet = lv_obj_create(screen);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, sheet_w, sheet_h);
    lv_obj_align(sheet, LV_ALIGN_TOP_MID, 0, top_margin);
    rpod_theme_style_glass_panel(sheet, square ? 10 : 16);
    lv_obj_set_style_bg_opa(sheet, PICKER_SHEET_OPA, 0);
    lv_obj_set_style_clip_corner(sheet, true, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Header: what we're adding, and to where. --- */
    lv_obj_t *title = lv_label_create(sheet);
    lv_label_set_text(title, "Add to Playlist");
    lv_obj_set_style_text_font(title, m->font_body, 0);
    lv_obj_set_style_text_color(title, RPOD_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad, pad);

    lv_obj_t *song = lv_label_create(sheet);
    lv_label_set_text(song, st->title);
    lv_obj_set_style_text_font(song, m->font_small, 0);
    lv_obj_set_style_text_color(song, RPOD_COLOR_DIM_TEXT, 0);
    lv_label_set_long_mode(song, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(song, sheet_w - 2 * pad);
    int title_h = lv_font_get_line_height(m->font_body);
    lv_obj_align(song, LV_ALIGN_TOP_LEFT, pad, pad + title_h + 2);

    /* --- Playlist list, filling the rest of the sheet: full sheet width so
     * the sheet's own rounded corners (clip_corner above) round off the
     * list's last row to match, iOS grouped-table style. The active theme is
     * light-mode (lv_conf.h's LV_THEME_DEFAULT_DARK 0) and lv_list's default
     * "card" styling includes a light bg/border/radius that would otherwise
     * show through as a stray pale outline around the rows, so those are
     * overridden below -- explicitly, not via remove_style_all, which would
     * also wipe the LV_FLEX_FLOW_COLUMN layout lv_list_create() applies
     * internally to stack rows vertically, leaving every row at the
     * container's default (0,0) and overlapping (see list_screen.c's list,
     * which uses this same targeted-override pattern). --- */
    int list_top = pad + title_h + lv_font_get_line_height(m->font_small) + 2 + pad;
    lv_obj_t *list = lv_list_create(sheet);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_size(list, sheet_w, sheet_h - list_top);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);
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

    rpod_screen_stack_open_overlay(stack, build_picker_screen, st);
}
