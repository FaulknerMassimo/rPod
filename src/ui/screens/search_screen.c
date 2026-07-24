/*
 * Music > Search (docs/PLAN.md §8.1): a wheel-driven on-screen keyboard
 * with debounced search-as-you-type results, grouped Artists / Albums /
 * Songs, iPod Classic style but with the project's liquid-glass look.
 *
 * Input model: everything focusable lives in this screen's one encoder
 * group, ordered keyboard keys first, then result rows -- so rotating
 * right past the last key walks down into the results, and rotating left
 * from the first result climbs back onto the keyboard. The group is
 * re-ordered by hand (sync_group) after every rebuild because widgets
 * auto-join the default group in *creation* order, and results are
 * recreated on every keystroke -- without the re-sort, fresh result rows
 * would land after the keys one rebuild and interleave wrongly the next
 * layout toggle.
 *
 * Queries: song matches come from MPD's `search any` (case-insensitive
 * substring, capped server-side with a window). Artist/album suggestions
 * deliberately do NOT come from the matched songs' tags -- copy_multi_tag()
 * joins a multi-artist song's artists into one "A, B" display string, which
 * is not a real tag value and would drill down to an empty album list.
 * Instead they're filtered client-side out of the same full tag
 * enumerations the Artists/Albums browse screens already run per push, so
 * every suggestion is an exact tag value that drills down correctly.
 */

#include "search_screen.h"

#include "music_screens.h"
#include "now_playing.h"
#include "ui/metrics.h"
#include "ui/theme.h"
#include "audio/mpd_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEARCH_QUERY_MAX    63
#define SEARCH_DEBOUNCE_MS  220
#define SEARCH_MPD_WINDOW   100 /* server-side cap on song matches */
#define SEARCH_MAX_TAG_ROWS 4   /* artist / album suggestions each */
#define SEARCH_MAX_SONGS    30

#define SQUARE     (rpod_metrics()->form == RPOD_FORM_SQUARE)
#define PANEL_W    (rpod_metrics()->screen_w - 16)
#define FIELD_Y    (rpod_metrics()->header_h + 4)
#define FIELD_H    (SQUARE ? 16 : 26)
#define KEY_H      (SQUARE ? 13 : 21)
/* Fixed key width sized so the widest (10-key QWERTY) row fits the panel's
 * inner width with its gaps; shorter rows center, iOS-keyboard style,
 * instead of stretching to fill. Tiny on the 128px square panel. */
#define KEY_W      (SQUARE ? 10 : 26)
#define KB_PAD     (SQUARE ? 2 : 5)
#define KB_ROW_GAP (SQUARE ? 1 : 3)
#define KB_COL_GAP (SQUARE ? 1 : 3)
#define KB_H       (4 * KEY_H + 3 * KB_ROW_GAP + 2 * KB_PAD)
#define KB_MARGIN  (SQUARE ? 3 : 6)
#define RESULTS_Y  (FIELD_Y + FIELD_H + 4)
#define RESULTS_H  (rpod_metrics()->screen_h - KB_MARGIN - KB_H - 4 - RESULTS_Y)
#define ROW_H      (SQUARE ? 15 : 22)
#define CURSOR_W   2

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_mpd_t *mpd;
    lv_group_t *group;
    lv_obj_t *screen;
    lv_obj_t *text_wrap;
    lv_obj_t *query_label;
    lv_obj_t *cursor;
    lv_obj_t *results;
    lv_obj_t *kb;
    lv_obj_t *toggle_btn;
    lv_timer_t *debounce;
    lv_timer_t *blink;
    char query[SEARCH_QUERY_MAX + 1];
    size_t query_len;
    bool digits_mode;
} search_state_t;

/* Negative key codes are actions; positive ones are the literal character. */
enum {
    KEY_CODE_BACKSPACE = -1,
    KEY_CODE_TOGGLE = -2,
};

typedef struct {
    search_state_t *st;
    int code;
} key_ctx_t;

enum result_kind {
    RESULT_ARTIST,
    RESULT_ALBUM,
    RESULT_SONG,
};

/* Owns copies of everything it needs -- result rows are rebuilt (and the
 * backing MPD fetches freed) on every debounced keystroke, so pointing into
 * a fetch array would be a lifetime bug waiting to happen. */
typedef struct {
    search_state_t *st;
    enum result_kind kind;
    char name[256]; /* artist or album name; unused for songs */
    char uri[512];  /* songs only */
} result_ctx_t;

static void run_search(search_state_t *st);
static void build_keyboard(search_state_t *st);
static void sync_group(search_state_t *st, lv_obj_t *focus_or_null);

/* ASCII-only case folding: the on-screen keyboard can only produce ASCII,
 * and MPD's own `search` matching handles the song side. */
static bool contains_ci(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return true;
    }
    for (const char *h = haystack; *h != '\0'; h++) {
        size_t i = 0;
        while (i < nlen && h[i] != '\0' &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

/* --- Search field --------------------------------------------------------- */

static void update_query_view(search_state_t *st)
{
    if (st->query_len == 0) {
        lv_label_set_text(st->query_label, "Search");
        lv_obj_set_style_text_color(st->query_label, RPOD_COLOR_DIM_TEXT, 0);
    } else {
        lv_label_set_text(st->query_label, st->query);
        lv_obj_set_style_text_color(st->query_label, RPOD_COLOR_TEXT, 0);
    }

    /* Keep the *end* of the query in view once it outgrows the field: pin
     * the label's right edge (leaving room for the cursor) instead of
     * letting the tail vanish under the clip. A long-mode ellipsis is the
     * wrong tool here -- it hides exactly the part being typed. */
    lv_obj_update_layout(st->text_wrap);
    bool overflow = st->query_len > 0 &&
                    lv_obj_get_width(st->query_label) > lv_obj_get_content_width(st->text_wrap);
    if (overflow) {
        lv_obj_align(st->query_label, LV_ALIGN_RIGHT_MID, -(CURSOR_W + 3), 0);
    } else if (st->query_len > 0) {
        lv_obj_align(st->query_label, LV_ALIGN_LEFT_MID, 0, 0);
    } else {
        /* Empty: cursor sits at the left, dim placeholder just after it. */
        lv_obj_align(st->query_label, LV_ALIGN_LEFT_MID, CURSOR_W + 4, 0);
    }

    if (st->query_len > 0) {
        lv_obj_align_to(st->cursor, st->query_label, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    } else {
        lv_obj_align(st->cursor, LV_ALIGN_LEFT_MID, 0, 0);
    }

    /* Restart the blink phase visible, so the cursor never disappears the
     * instant a character lands. */
    lv_obj_remove_flag(st->cursor, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(st->blink);
}

static void cursor_blink_cb(lv_timer_t *timer)
{
    search_state_t *st = lv_timer_get_user_data(timer);
    if (lv_obj_has_flag(st->cursor, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(st->cursor, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(st->cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --- Encoder group ordering ----------------------------------------------- */

static void add_group_buttons(lv_group_t *group, lv_obj_t *parent)
{
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, &lv_button_class)) {
            lv_group_add_obj(group, child);
        }
    }
}

/* Rebuilds the group as keys-then-results and restores focus. NULL means
 * "no meaningful previous focus" -- fall back to the first keyboard key. */
static void sync_group(search_state_t *st, lv_obj_t *focus_or_null)
{
    lv_group_remove_all_objs(st->group);

    uint32_t rows = lv_obj_get_child_count(st->kb);
    for (uint32_t i = 0; i < rows; i++) {
        add_group_buttons(st->group, lv_obj_get_child(st->kb, i));
    }
    add_group_buttons(st->group, st->results);

    if (focus_or_null == NULL && rows > 0) {
        focus_or_null = lv_obj_get_child(lv_obj_get_child(st->kb, 0), 0);
    }
    if (focus_or_null != NULL) {
        lv_group_focus_obj(focus_or_null);
    }
}

/* --- Keyboard ------------------------------------------------------------- */

static const char *const kb_rows_letters[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static const char *const kb_rows_digits[]  = { "1234567890", ".,-'&!?/:", "\"();+#@=" };

static void key_ctx_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void key_click_cb(lv_event_t *e)
{
    key_ctx_t *k = lv_event_get_user_data(e);
    search_state_t *st = k->st;

    if (k->code == KEY_CODE_TOGGLE) {
        st->digits_mode = !st->digits_mode;
        /* Empty the group before deleting the old keys: deleting the
         * focused key while it's still a member makes LVGL hand focus to
         * the next member, and as each key dies in turn the focus ripples
         * off the keyboard into the result rows -- whose focus handler
         * then scrolls the results to wherever the cascade lands (seen as
         * the list jumping on every layout toggle). sync_group rebuilds
         * the membership afterwards anyway. */
        lv_group_remove_all_objs(st->group);
        build_keyboard(st);
        sync_group(st, st->toggle_btn);
        return;
    }

    if (k->code == KEY_CODE_BACKSPACE) {
        if (st->query_len == 0) {
            return;
        }
        st->query[--st->query_len] = '\0';
    } else {
        if (st->query_len >= SEARCH_QUERY_MAX) {
            return;
        }
        st->query[st->query_len++] = (char)k->code;
        st->query[st->query_len] = '\0';
    }

    update_query_view(st);
    lv_timer_reset(st->debounce);
    lv_timer_resume(st->debounce);
}

static lv_obj_t *add_key(search_state_t *st, lv_obj_t *row, const char *text, int code,
                          uint8_t grow)
{
    key_ctx_t *k = malloc(sizeof(*k));
    k->st = st;
    k->code = code;

    lv_obj_t *btn = lv_button_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, LV_PCT(100));
    if (grow > 0) {
        lv_obj_set_flex_grow(btn, grow);
    } else {
        lv_obj_set_width(btn, KEY_W); /* fixed-width character key */
    }
    lv_obj_set_style_radius(btn, 5, 0);
    /* Faint white fill so each key reads as a raised glass keycap against
     * the panel; focus swaps it for the solid accent highlight. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, 20, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, rpod_metrics()->font_small, 0);
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, key_click_cb, LV_EVENT_CLICKED, k);
    lv_obj_add_event_cb(btn, key_ctx_free_cb, LV_EVENT_DELETE, k);
    return btn;
}

static lv_obj_t *add_kb_row(lv_obj_t *kb)
{
    lv_obj_t *row = lv_obj_create(kb);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), KEY_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* Centering only matters for the fixed-width character rows; the
     * bottom action row's growing keys fill the width either way. */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, KB_COL_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void build_keyboard(search_state_t *st)
{
    lv_obj_clean(st->kb);

    const char *const *char_rows = st->digits_mode ? kb_rows_digits : kb_rows_letters;
    for (size_t r = 0; r < 3; r++) {
        lv_obj_t *row = add_kb_row(st->kb);
        for (const char *c = char_rows[r]; *c != '\0'; c++) {
            char text[2] = { *c, '\0' };
            add_key(st, row, text, *c, 0);
        }
    }

    lv_obj_t *row = add_kb_row(st->kb);
    st->toggle_btn = add_key(st, row, st->digits_mode ? "ABC" : "123", KEY_CODE_TOGGLE, 2);
    add_key(st, row, "space", ' ', 5);
    /* The square profile's key font (montserrat_10) has no FontAwesome glyphs,
     * so the backspace symbol would render as a box -- use an ASCII label. */
    add_key(st, row, SQUARE ? "DEL" : LV_SYMBOL_BACKSPACE, KEY_CODE_BACKSPACE, 2);
}

/* --- Results -------------------------------------------------------------- */

static void result_ctx_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void result_click_cb(lv_event_t *e)
{
    result_ctx_t *r = lv_event_get_user_data(e);
    switch (r->kind) {
        case RESULT_ARTIST:
            rpod_music_push_artist_albums(r->st->stack, r->st->mpd, r->name);
            break;
        case RESULT_ALBUM:
            /* Album name only, no artist scope -- matches what selecting
             * the same album from the unscoped Albums browse list does. */
            rpod_music_push_album_songs(r->st->stack, r->st->mpd, NULL, r->name);
            break;
        case RESULT_SONG:
            rpod_mpd_play_uri(r->st->mpd, r->uri);
            rpod_screen_stack_push(r->st->stack, rpod_now_playing_build, r->st->mpd, NULL);
            break;
    }
}

/* Same instant-snap scrolling rationale as list_screen.c's row_focus_cb. */
static void result_focus_cb(lv_event_t *e)
{
    lv_obj_scroll_to_view_recursive(lv_event_get_target(e), LV_ANIM_OFF);
}

static void add_section_header(search_state_t *st, const char *text)
{
    lv_obj_t *label = lv_label_create(st->results);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_style_text_font(label, rpod_metrics()->font_small, 0);
    lv_obj_set_style_pad_left(label, 12, 0);
    lv_obj_set_style_pad_top(label, 4, 0);
}

static void add_result_row(search_state_t *st, enum result_kind kind, const char *title,
                            const char *accessory_or_null, const char *uri_or_null, bool chevron)
{
    result_ctx_t *r = calloc(1, sizeof(*r));
    r->st = st;
    r->kind = kind;
    snprintf(r->name, sizeof(r->name), "%s", title);
    if (uri_or_null != NULL) {
        snprintf(r->uri, sizeof(r->uri), "%s", uri_or_null);
    }

    lv_obj_t *btn = lv_button_create(st->results);
    lv_obj_remove_style_all(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(btn, LV_PCT(100), ROW_H);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_pad_column(btn, 8, 0);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);

    lv_obj_t *title_label = lv_label_create(btn);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, rpod_metrics()->font_small, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_set_height(title_label, lv_font_get_line_height(rpod_metrics()->font_small));

    /* Secondary text at reduced opacity rather than an explicit dim color:
     * it inherits the row's text color, so it stays readable on the accent
     * highlight without list_screen.c's manual focus/defocus recoloring. */
    if (accessory_or_null != NULL && accessory_or_null[0] != '\0') {
        lv_obj_t *acc = lv_label_create(btn);
        lv_label_set_text(acc, accessory_or_null);
        lv_obj_set_style_text_font(acc, rpod_metrics()->font_small, 0);
        lv_obj_set_style_text_opa(acc, LV_OPA_60, 0);
        lv_label_set_long_mode(acc, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_max_width(acc, 120, 0);
        lv_obj_set_height(acc, lv_font_get_line_height(rpod_metrics()->font_small));
    }

    if (chevron) {
        lv_obj_t *ch = lv_label_create(btn);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_opa(ch, LV_OPA_60, 0);
    }

    lv_obj_add_event_cb(btn, result_click_cb, LV_EVENT_CLICKED, r);
    lv_obj_add_event_cb(btn, result_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, result_ctx_free_cb, LV_EVENT_DELETE, r);
}

static void show_center_hint(search_state_t *st, const char *text)
{
    lv_obj_set_flex_align(st->results, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *label = lv_label_create(st->results);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_style_text_font(label, rpod_metrics()->font_small, 0);
}

static void run_search(search_state_t *st)
{
    /* If focus is sitting on a result row we're about to delete, don't try
     * to restore it afterwards -- fall back to the keyboard instead. */
    lv_obj_t *foc = lv_group_get_focused(st->group);
    if (foc != NULL && lv_obj_get_parent(foc) == st->results) {
        foc = NULL;
    }

    lv_obj_clean(st->results);
    lv_obj_scroll_to_y(st->results, 0, LV_ANIM_OFF);
    lv_obj_set_flex_align(st->results, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    if (st->query_len == 0) {
        show_center_hint(st, "Type to search");
        sync_group(st, foc);
        return;
    }

    rpod_mpd_item_t *artists = NULL, *albums = NULL;
    size_t artist_count = 0, album_count = 0;
    rpod_mpd_list_artists(st->mpd, &artists, &artist_count);
    rpod_mpd_list_albums(st->mpd, NULL, &albums, &album_count);

    rpod_mpd_song_t *songs = NULL;
    size_t song_count = 0;
    rpod_mpd_search_songs(st->mpd, st->query, SEARCH_MPD_WINDOW, &songs, &song_count);

    size_t shown = 0;

    size_t n = 0;
    for (size_t i = 0; i < artist_count && n < SEARCH_MAX_TAG_ROWS; i++) {
        if (contains_ci(artists[i].name, st->query)) {
            if (n++ == 0) {
                add_section_header(st, "ARTISTS");
            }
            add_result_row(st, RESULT_ARTIST, artists[i].name, NULL, NULL, true);
            shown++;
        }
    }

    n = 0;
    for (size_t i = 0; i < album_count && n < SEARCH_MAX_TAG_ROWS; i++) {
        if (contains_ci(albums[i].name, st->query)) {
            if (n++ == 0) {
                add_section_header(st, "ALBUMS");
            }
            add_result_row(st, RESULT_ALBUM, albums[i].name, NULL, NULL, true);
            shown++;
        }
    }

    n = 0;
    for (size_t i = 0; i < song_count && n < SEARCH_MAX_SONGS; i++) {
        const char *title = songs[i].title[0] != '\0' ? songs[i].title : songs[i].uri;
        if (contains_ci(title, st->query)) {
            if (n++ == 0) {
                add_section_header(st, "SONGS");
            }
            add_result_row(st, RESULT_SONG, title, songs[i].artist, songs[i].uri, false);
            shown++;
        }
    }

    /* `search any` can match on tags we don't section (genre, album artist,
     * composer...). Rather than claiming "no results" while MPD clearly
     * found songs, list those matches under Songs. */
    if (shown == 0 && song_count > 0) {
        add_section_header(st, "SONGS");
        for (size_t i = 0; i < song_count && i < SEARCH_MAX_SONGS; i++) {
            const char *title = songs[i].title[0] != '\0' ? songs[i].title : songs[i].uri;
            add_result_row(st, RESULT_SONG, title, songs[i].artist, songs[i].uri, false);
            shown++;
        }
    }

    rpod_mpd_free_items(artists);
    rpod_mpd_free_items(albums);
    rpod_mpd_free_songs(songs);

    if (shown == 0) {
        show_center_hint(st, "No Results");
    }
    sync_group(st, foc);
}

static void debounce_cb(lv_timer_t *timer)
{
    search_state_t *st = lv_timer_get_user_data(timer);
    lv_timer_pause(timer);
    run_search(st);
}

/* --- Screen --------------------------------------------------------------- */

static void search_cleanup_cb(lv_event_t *e)
{
    search_state_t *st = lv_event_get_user_data(e);
    lv_timer_delete(st->debounce);
    lv_timer_delete(st->blink);
    free(st);
}

void rpod_search_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    search_state_t *st = calloc(1, sizeof(*st));
    st->stack = stack;
    st->mpd = ctx;
    st->screen = screen;
    st->group = lv_group_get_default();

    lv_obj_t *field = lv_obj_create(screen);
    lv_obj_remove_style_all(field);
    rpod_theme_style_glass_panel(field, 8);
    lv_obj_set_size(field, PANEL_W, FIELD_H);
    lv_obj_align(field, LV_ALIGN_TOP_MID, 0, FIELD_Y);
    lv_obj_set_style_pad_hor(field, 10, 0);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE);

    st->text_wrap = lv_obj_create(field);
    lv_obj_remove_style_all(st->text_wrap);
    lv_obj_set_size(st->text_wrap, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(st->text_wrap, LV_OBJ_FLAG_SCROLLABLE);

    st->query_label = lv_label_create(st->text_wrap);
    lv_obj_set_style_text_font(st->query_label, rpod_metrics()->font_body, 0);

    st->cursor = lv_obj_create(st->text_wrap);
    lv_obj_remove_style_all(st->cursor);
    lv_obj_set_size(st->cursor, CURSOR_W, 16);
    lv_obj_set_style_radius(st->cursor, 1, 0);
    lv_obj_set_style_bg_color(st->cursor, RPOD_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(st->cursor, LV_OPA_COVER, 0);

    st->results = lv_obj_create(screen);
    lv_obj_remove_style_all(st->results);
    rpod_theme_style_glass_panel(st->results, 10);
    lv_obj_set_size(st->results, PANEL_W, RESULTS_H);
    lv_obj_align(st->results, LV_ALIGN_TOP_MID, 0, RESULTS_Y);
    lv_obj_set_style_clip_corner(st->results, true, 0);
    lv_obj_set_flex_flow(st->results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(st->results, LV_SCROLLBAR_MODE_OFF);

    st->kb = lv_obj_create(screen);
    lv_obj_remove_style_all(st->kb);
    rpod_theme_style_glass_panel(st->kb, 10);
    lv_obj_set_size(st->kb, PANEL_W, KB_H);
    lv_obj_align(st->kb, LV_ALIGN_BOTTOM_MID, 0, -KB_MARGIN);
    lv_obj_set_flex_flow(st->kb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(st->kb, KB_PAD, 0);
    lv_obj_set_style_pad_row(st->kb, KB_ROW_GAP, 0);
    lv_obj_clear_flag(st->kb, LV_OBJ_FLAG_SCROLLABLE);

    st->debounce = lv_timer_create(debounce_cb, SEARCH_DEBOUNCE_MS, st);
    lv_timer_pause(st->debounce);
    st->blink = lv_timer_create(cursor_blink_cb, 530, st);

    lv_obj_add_event_cb(screen, search_cleanup_cb, LV_EVENT_DELETE, st);

    build_keyboard(st);
    update_query_view(st);
    run_search(st); /* empty query: hint + focus lands on the first key */
}
