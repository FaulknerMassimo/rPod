/*
 * "New Playlist" naming screen and the "add songs" picker it hands off to.
 * The keyboard here is a trimmed copy of search_screen.c's: same encoder
 * group ordering (keys, then the action row) and the same uppercase-ASCII
 * key set, minus the live-search debounce/results machinery -- this screen
 * only ever produces one string, on demand, when Create is pressed.
 */

#include "playlist_edit_screens.h"

#include "list_screen.h"
#include "ui/metrics.h"
#include "ui/theme.h"
#include "audio/mpd_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYLIST_NAME_MAX     63
#define PANEL_W      (rpod_metrics()->screen_w - 16)
#define FIELD_Y      (rpod_metrics()->header_h + 10)
#define SQUARE       (rpod_metrics()->form == RPOD_FORM_SQUARE)
#define FIELD_H      (SQUARE ? 16 : 28)
#define KEY_H        (SQUARE ? 13 : 21)
#define KEY_W        (SQUARE ? 10 : 26)
#define KB_PAD       (SQUARE ? 2 : 5)
#define KB_ROW_GAP   (SQUARE ? 1 : 3)
#define KB_COL_GAP   (SQUARE ? 1 : 3)
#define KB_H         (4 * KEY_H + 3 * KB_ROW_GAP + 2 * KB_PAD)
#define KB_MARGIN    6
#define CURSOR_W     2

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_mpd_t *mpd;
    lv_group_t *group;
    lv_obj_t *text_wrap;
    lv_obj_t *name_label;
    lv_obj_t *cursor;
    lv_obj_t *kb;
    lv_obj_t *toggle_btn;
    lv_timer_t *blink;
    char name[PLAYLIST_NAME_MAX + 1];
    size_t name_len;
    bool digits_mode;
} name_state_t;

enum {
    KEY_CODE_BACKSPACE = -1,
    KEY_CODE_TOGGLE = -2,
    KEY_CODE_CREATE = -3,
};

typedef struct {
    name_state_t *st;
    int code;
} key_ctx_t;

static void build_keyboard(name_state_t *st);
static void sync_group(name_state_t *st, lv_obj_t *focus_or_null);

/* --- Name field ------------------------------------------------------- */

static void update_name_view(name_state_t *st)
{
    if (st->name_len == 0) {
        lv_label_set_text(st->name_label, "Playlist Name");
        lv_obj_set_style_text_color(st->name_label, RPOD_COLOR_DIM_TEXT, 0);
    } else {
        lv_label_set_text(st->name_label, st->name);
        lv_obj_set_style_text_color(st->name_label, RPOD_COLOR_TEXT, 0);
    }

    /* Same right-pinned overflow handling as search_screen.c's query field:
     * keep the end of the name (where typing happens) in view rather than
     * letting a long name's tail vanish under the clip. */
    lv_obj_update_layout(st->text_wrap);
    bool overflow = st->name_len > 0 &&
                    lv_obj_get_width(st->name_label) > lv_obj_get_content_width(st->text_wrap);
    if (overflow) {
        lv_obj_align(st->name_label, LV_ALIGN_RIGHT_MID, -(CURSOR_W + 3), 0);
    } else if (st->name_len > 0) {
        lv_obj_align(st->name_label, LV_ALIGN_LEFT_MID, 0, 0);
    } else {
        lv_obj_align(st->name_label, LV_ALIGN_LEFT_MID, CURSOR_W + 4, 0);
    }

    if (st->name_len > 0) {
        lv_obj_align_to(st->cursor, st->name_label, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    } else {
        lv_obj_align(st->cursor, LV_ALIGN_LEFT_MID, 0, 0);
    }

    lv_obj_remove_flag(st->cursor, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(st->blink);
}

static void cursor_blink_cb(lv_timer_t *timer)
{
    name_state_t *st = lv_timer_get_user_data(timer);
    if (lv_obj_has_flag(st->cursor, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(st->cursor, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(st->cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --- Encoder group ordering --------------------------------------------- */

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

static void sync_group(name_state_t *st, lv_obj_t *focus_or_null)
{
    lv_group_remove_all_objs(st->group);

    uint32_t rows = lv_obj_get_child_count(st->kb);
    for (uint32_t i = 0; i < rows; i++) {
        add_group_buttons(st->group, lv_obj_get_child(st->kb, i));
    }

    if (focus_or_null == NULL && rows > 0) {
        focus_or_null = lv_obj_get_child(lv_obj_get_child(st->kb, 0), 0);
    }
    if (focus_or_null != NULL) {
        lv_group_focus_obj(focus_or_null);
    }
}

/* --- Keyboard ------------------------------------------------------------ */

static const char *const kb_rows_letters[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
/* Same symbol row as search_screen.c's, minus '/' -- a playlist name maps
 * straight onto a filename in MPD's playlist directory, and '/' would
 * either be rejected server-side or read as a path separator. */
static const char *const kb_rows_digits[]  = { "1234567890", ".,-'&!?_", "\"();+#@=" };

static void key_ctx_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void on_create_click(lv_event_t *e);

static void key_click_cb(lv_event_t *e)
{
    key_ctx_t *k = lv_event_get_user_data(e);
    name_state_t *st = k->st;

    if (k->code == KEY_CODE_CREATE) {
        on_create_click(e);
        return;
    }

    if (k->code == KEY_CODE_TOGGLE) {
        st->digits_mode = !st->digits_mode;
        /* Empty the group before deleting the old keys -- see
         * search_screen.c's key_click_cb for why (focus cascading off the
         * keyboard mid-rebuild otherwise). */
        lv_group_remove_all_objs(st->group);
        build_keyboard(st);
        sync_group(st, st->toggle_btn);
        return;
    }

    if (k->code == KEY_CODE_BACKSPACE) {
        if (st->name_len == 0) {
            return;
        }
        st->name[--st->name_len] = '\0';
    } else {
        if (st->name_len >= PLAYLIST_NAME_MAX) {
            return;
        }
        st->name[st->name_len++] = (char)k->code;
        st->name[st->name_len] = '\0';
    }

    update_name_view(st);
}

static lv_obj_t *add_key(name_state_t *st, lv_obj_t *row, const char *text, int code, uint8_t grow)
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
        lv_obj_set_width(btn, KEY_W);
    }
    lv_obj_set_style_radius(btn, 5, 0);
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
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, KB_COL_GAP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void build_keyboard(name_state_t *st)
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
    add_key(st, row, "space", ' ', 3);
    /* Square key font (montserrat_10) lacks FontAwesome glyphs -- ASCII label. */
    add_key(st, row, SQUARE ? "DEL" : LV_SYMBOL_BACKSPACE, KEY_CODE_BACKSPACE, 2);
    add_key(st, row, "Create", KEY_CODE_CREATE, 3);
}

/* --- Screen --------------------------------------------------------------- */

static void name_cleanup_cb(lv_event_t *e)
{
    name_state_t *st = lv_event_get_user_data(e);
    lv_timer_delete(st->blink);
    free(st);
}

static void on_create_click(lv_event_t *e)
{
    key_ctx_t *k = lv_event_get_user_data(e);
    name_state_t *st = k->st;
    if (st->name_len == 0) {
        return;
    }

    /* Copy what's needed before popping -- pop() deletes this screen (and,
     * with it, `st` via name_cleanup_cb) synchronously, same as
     * settings_screens.c's on_output_toggle. */
    char name_copy[PLAYLIST_NAME_MAX + 1];
    snprintf(name_copy, sizeof(name_copy), "%s", st->name);
    rpod_mpd_t *mpd = st->mpd;
    rpod_screen_stack_t *stack = st->stack;

    rpod_screen_stack_pop(stack);
    rpod_push_add_songs_screen(stack, mpd, name_copy);
}

void rpod_new_playlist_name_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    name_state_t *st = calloc(1, sizeof(*st));
    st->stack = stack;
    st->mpd = ctx;
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

    st->name_label = lv_label_create(st->text_wrap);
    lv_obj_set_style_text_font(st->name_label, rpod_metrics()->font_body, 0);

    st->cursor = lv_obj_create(st->text_wrap);
    lv_obj_remove_style_all(st->cursor);
    lv_obj_set_size(st->cursor, CURSOR_W, 16);
    lv_obj_set_style_radius(st->cursor, 1, 0);
    lv_obj_set_style_bg_color(st->cursor, RPOD_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(st->cursor, LV_OPA_COVER, 0);

    st->kb = lv_obj_create(screen);
    lv_obj_remove_style_all(st->kb);
    rpod_theme_style_glass_panel(st->kb, 10);
    lv_obj_set_size(st->kb, PANEL_W, KB_H);
    lv_obj_align(st->kb, LV_ALIGN_BOTTOM_MID, 0, -KB_MARGIN);
    lv_obj_set_flex_flow(st->kb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(st->kb, KB_PAD, 0);
    lv_obj_set_style_pad_row(st->kb, KB_ROW_GAP, 0);
    lv_obj_clear_flag(st->kb, LV_OBJ_FLAG_SCROLLABLE);

    st->blink = lv_timer_create(cursor_blink_cb, 530, st);

    lv_obj_add_event_cb(screen, name_cleanup_cb, LV_EVENT_DELETE, st);

    build_keyboard(st);
    update_name_view(st);
    sync_group(st, NULL);
}

/* --- Add Songs picker ------------------------------------------------------ */

typedef struct add_songs_state add_songs_state_t;

typedef struct {
    add_songs_state_t *st;
    const rpod_mpd_song_t *song;
} add_song_row_t;

struct add_songs_state {
    rpod_mpd_t *mpd;
    char *playlist_name;
    rpod_mpd_song_t *songs;
    add_song_row_t *rows;
    lv_obj_t *toast;
    lv_timer_t *toast_timer;
};

typedef struct {
    rpod_mpd_t *mpd;
    char *playlist_name;
} add_songs_ctx_t;

static void add_songs_ctx_free(void *p)
{
    add_songs_ctx_t *ctx = p;
    free(ctx->playlist_name);
    free(ctx);
}

static void add_songs_cleanup_cb(lv_event_t *e)
{
    add_songs_state_t *st = lv_event_get_user_data(e);
    lv_timer_delete(st->toast_timer);
    rpod_mpd_free_songs(st->songs);
    free(st->rows);
    free(st->playlist_name);
    free(st);
}

static void toast_hide_cb(lv_timer_t *timer)
{
    add_songs_state_t *st = lv_timer_get_user_data(timer);
    lv_obj_add_flag(st->toast, LV_OBJ_FLAG_HIDDEN);
}

static void on_add_song_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    add_song_row_t *row = item_ctx;
    add_songs_state_t *st = row->st;

    const char *label = row->song->title[0] != '\0' ? row->song->title : row->song->uri;
    char msg[300];
    if (rpod_mpd_playlist_add_song(st->mpd, st->playlist_name, row->song->uri)) {
        snprintf(msg, sizeof(msg), "Added \"%.200s\"", label);
    } else {
        snprintf(msg, sizeof(msg), "Couldn't add \"%.200s\"", label);
    }
    lv_label_set_text(st->toast, msg);
    lv_obj_remove_flag(st->toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(st->toast_timer);
    lv_timer_resume(st->toast_timer);
}

static void build_add_songs_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    add_songs_ctx_t *in = ctx;

    rpod_mpd_song_t *songs = NULL;
    size_t count = 0;
    rpod_mpd_list_songs(in->mpd, NULL, NULL, &songs, &count);

    add_songs_state_t *st = calloc(1, sizeof(*st));
    st->mpd = in->mpd;
    st->playlist_name = strdup(in->playlist_name);
    st->songs = songs;
    st->rows = count > 0 ? malloc(count * sizeof(*st->rows)) : NULL;
    for (size_t i = 0; i < count; i++) {
        st->rows[i].st = st;
        st->rows[i].song = &songs[i];
    }

    rpod_list_item_t *ui_items = count > 0 ? calloc(count, sizeof(*ui_items)) : NULL;
    for (size_t i = 0; i < count; i++) {
        const char *label = songs[i].title[0] != '\0' ? songs[i].title : songs[i].uri;
        snprintf(ui_items[i].text, sizeof(ui_items[i].text), "%.*s",
                 (int)sizeof(ui_items[i].text) - 1, label);
        if (songs[i].artist[0] != '\0') {
            snprintf(ui_items[i].subtitle, sizeof(ui_items[i].subtitle), "%s", songs[i].artist);
        }
        ui_items[i].on_select = on_add_song_select;
        ui_items[i].item_ctx = &st->rows[i];
    }
    rpod_list_screen_build(stack, screen, ui_items, count);
    free(ui_items);

    /* Created after the list, so it draws on top as a floating toast
     * instead of sitting behind the list panel. */
    st->toast = lv_label_create(screen);
    lv_obj_set_style_bg_color(st->toast, RPOD_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(st->toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st->toast, 8, 0);
    lv_obj_set_style_pad_hor(st->toast, 10, 0);
    lv_obj_set_style_pad_ver(st->toast, 5, 0);
    lv_obj_set_style_text_color(st->toast, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(st->toast, rpod_metrics()->font_small, 0);
    lv_label_set_long_mode(st->toast, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(st->toast, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(st->toast, PANEL_W - 20, 0);
    lv_obj_align(st->toast, LV_ALIGN_TOP_MID, 0, rpod_metrics()->header_h + 4);
    lv_obj_add_flag(st->toast, LV_OBJ_FLAG_HIDDEN);

    st->toast_timer = lv_timer_create(toast_hide_cb, 1100, st);
    lv_timer_pause(st->toast_timer);

    lv_obj_add_event_cb(screen, add_songs_cleanup_cb, LV_EVENT_DELETE, st);
}

void rpod_push_add_songs_screen(rpod_screen_stack_t *stack, rpod_mpd_t *mpd, const char *playlist_name)
{
    add_songs_ctx_t *ctx = malloc(sizeof(*ctx));
    ctx->mpd = mpd;
    ctx->playlist_name = strdup(playlist_name);
    rpod_screen_stack_push(stack, build_add_songs_screen, ctx, add_songs_ctx_free);
}
