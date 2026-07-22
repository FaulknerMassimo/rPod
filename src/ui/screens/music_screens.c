#include "music_screens.h"

#include "list_screen.h"
#include "now_playing.h"
#include "search_screen.h"
#include "playlist_edit_screens.h"
#include "audio/mpd_client.h"
#include "ui/cover_art.h"
#include "ui/theme.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-screen filter, passed as the rpod_screen_stack_push() ctx for every
 * browse/leaf screen below. Which fields are meaningful depends on the
 * screen: album/artist lists use `artist`; the song list uses whichever of
 * `artist`+`album` or `playlist` got set on the way down. */
typedef struct {
    rpod_mpd_t *mpd;
    char *artist;
    char *album;
    char *genre;
    char *playlist;
} music_ctx_t;

static char *dup_or_null(const char *s)
{
    return s != NULL ? strdup(s) : NULL;
}

static music_ctx_t *music_ctx_new(rpod_mpd_t *mpd, const char *artist, const char *album,
                                   const char *genre, const char *playlist)
{
    music_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->mpd = mpd;
    ctx->artist = dup_or_null(artist);
    ctx->album = dup_or_null(album);
    ctx->genre = dup_or_null(genre);
    ctx->playlist = dup_or_null(playlist);
    return ctx;
}

static void music_ctx_free(void *p)
{
    music_ctx_t *ctx = p;
    free(ctx->artist);
    free(ctx->album);
    free(ctx->genre);
    free(ctx->playlist);
    free(ctx);
}

/* A clicked row's context for the name-only browse screens (artists,
 * albums, genres, playlists): which connection, which name, and -- for
 * albums reached via an artist -- which artist scoped the list, so
 * selecting an album can carry the artist filter forward to the song list. */
typedef struct {
    rpod_mpd_t *mpd;
    const char *name;
    const char *parent_artist;
} name_row_t;

/* Owns the raw mpd_item_t array a browse screen fetched plus the name_row_t
 * array built from it, both freed together when the screen is popped. */
typedef struct {
    rpod_mpd_item_t *items_raw;
    name_row_t *rows;
} name_list_fetch_t;

static void name_list_cleanup_cb(lv_event_t *e)
{
    name_list_fetch_t *fetch = lv_event_get_user_data(e);
    rpod_mpd_free_items(fetch->items_raw);
    free(fetch->rows);
    free(fetch);
}

/* Shared plumbing for every name-only browse screen: takes ownership of
 * `items_raw` (already fetched by the caller), builds the on-screen list,
 * and wires each row's item_ctx to a name_row_t carrying `parent_artist`
 * through (NULL where it doesn't apply). */
static void build_name_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen,
                                    rpod_mpd_t *mpd, rpod_mpd_item_t *items_raw, size_t count,
                                    const char *parent_artist,
                                    void (*on_select)(rpod_screen_stack_t *, void *))
{
    name_list_fetch_t *fetch = malloc(sizeof(*fetch));
    fetch->items_raw = items_raw;
    fetch->rows = count > 0 ? malloc(count * sizeof(*fetch->rows)) : NULL;
    for (size_t i = 0; i < count; i++) {
        fetch->rows[i].mpd = mpd;
        fetch->rows[i].name = items_raw[i].name;
        fetch->rows[i].parent_artist = parent_artist;
    }
    lv_obj_add_event_cb(screen, name_list_cleanup_cb, LV_EVENT_DELETE, fetch);

    rpod_list_item_t *ui_items = count > 0 ? calloc(count, sizeof(*ui_items)) : NULL;
    for (size_t i = 0; i < count; i++) {
        snprintf(ui_items[i].text, sizeof(ui_items[i].text), "%s", items_raw[i].name);
        ui_items[i].chevron = true;
        ui_items[i].on_select = on_select;
        ui_items[i].item_ctx = &fetch->rows[i];
    }
    rpod_list_screen_build(stack, screen, ui_items, count);
    free(ui_items);
}

static void build_song_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);
static void build_album_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);
static void build_genre_artist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

static void on_album_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    name_row_t *row = item_ctx;
    rpod_screen_stack_push(stack, build_song_list_screen,
                            music_ctx_new(row->mpd, row->parent_artist, row->name, NULL, NULL),
                            music_ctx_free);
}

static void on_artist_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    name_row_t *row = item_ctx;
    rpod_screen_stack_push(stack, build_album_list_screen,
                            music_ctx_new(row->mpd, row->name, NULL, NULL, NULL),
                            music_ctx_free);
}

static void on_genre_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    name_row_t *row = item_ctx;
    rpod_screen_stack_push(stack, build_genre_artist_list_screen,
                            music_ctx_new(row->mpd, NULL, NULL, row->name, NULL), music_ctx_free);
}

static void on_playlist_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    name_row_t *row = item_ctx;
    rpod_screen_stack_push(stack, build_song_list_screen,
                            music_ctx_new(row->mpd, NULL, NULL, NULL, row->name), music_ctx_free);
}

static void build_album_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_albums(filter->mpd, filter->artist, &items, &count);
    build_name_list_screen(stack, screen, filter->mpd, items, count, filter->artist, on_album_select);
}

static void build_artist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_artists(filter->mpd, &items, &count);
    build_name_list_screen(stack, screen, filter->mpd, items, count, NULL, on_artist_select);
}

static void build_genre_artist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_artists_in_genre(filter->mpd, filter->genre, &items, &count);
    /* Same next screen as the plain Artists list: pick an artist, browse
     * their albums -- the genre filter has done its job by here. */
    build_name_list_screen(stack, screen, filter->mpd, items, count, NULL, on_artist_select);
}

static void build_genre_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_genres(filter->mpd, &items, &count);
    build_name_list_screen(stack, screen, filter->mpd, items, count, NULL, on_genre_select);
}

static void on_new_playlist(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_new_playlist_name_build, item_ctx, NULL);
}

/* Not routed through build_name_list_screen -- that helper assumes a 1:1
 * mapping between fetched items and rows, but this list also needs one
 * extra leading action row ("New Playlist") that isn't an MPD playlist at
 * all.
 *
 * Rebuilds (rather than being built once): a brand new playlist only
 * starts existing server-side once its first song is added, from the Add
 * Songs picker pushed on top of this screen -- so this screen's own rows,
 * fetched back when it was first pushed, would otherwise go stale the
 * moment the user backs out and never show the playlist they just made.
 * Called directly for the initial build and again from
 * playlist_list_loaded_cb on every LV_EVENT_SCREEN_LOADED (i.e. every time
 * this screen becomes visible again, including via pop -- see
 * screen_stack.c's pop() for why group-default ordering makes it safe for
 * a LOADED handler to create fresh focusable rows here). */
static void populate_playlist_list(rpod_screen_stack_t *stack, lv_obj_t *screen, rpod_mpd_t *mpd)
{
    lv_obj_t *old_list = lv_obj_get_child(screen, 0);
    if (old_list != NULL) {
        lv_obj_delete(old_list);
    }

    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_playlists(mpd, &items, &count);

    name_list_fetch_t *fetch = malloc(sizeof(*fetch));
    fetch->items_raw = items;
    fetch->rows = count > 0 ? malloc(count * sizeof(*fetch->rows)) : NULL;
    for (size_t i = 0; i < count; i++) {
        fetch->rows[i].mpd = mpd;
        fetch->rows[i].name = items[i].name;
        fetch->rows[i].parent_artist = NULL;
    }

    rpod_list_item_t *ui_items = calloc(count + 1, sizeof(*ui_items));
    snprintf(ui_items[0].text, sizeof(ui_items[0].text), "New Playlist");
    ui_items[0].chevron = true;
    ui_items[0].on_select = on_new_playlist;
    ui_items[0].item_ctx = mpd;
    for (size_t i = 0; i < count; i++) {
        snprintf(ui_items[i + 1].text, sizeof(ui_items[i + 1].text), "%s", items[i].name);
        ui_items[i + 1].chevron = true;
        ui_items[i + 1].on_select = on_playlist_select;
        ui_items[i + 1].item_ctx = &fetch->rows[i];
    }
    rpod_list_screen_build(stack, screen, ui_items, count + 1);
    free(ui_items);

    /* Attached to the freshly created list itself, not the screen -- so
     * the lv_obj_delete(old_list) above on the *next* refresh frees this
     * exact fetch at the right time instead of leaking it until the whole
     * screen is torn down. */
    lv_obj_t *list = lv_obj_get_child(screen, 0);
    lv_obj_add_event_cb(list, name_list_cleanup_cb, LV_EVENT_DELETE, fetch);
}

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_mpd_t *mpd;
} playlists_screen_ctx_t;

static void playlists_screen_ctx_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void playlist_list_loaded_cb(lv_event_t *e)
{
    playlists_screen_ctx_t *pc = lv_event_get_user_data(e);
    populate_playlist_list(pc->stack, lv_event_get_target(e), pc->mpd);
}

static void build_playlist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;

    playlists_screen_ctx_t *pc = malloc(sizeof(*pc));
    pc->stack = stack;
    pc->mpd = filter->mpd;
    lv_obj_add_event_cb(screen, playlist_list_loaded_cb, LV_EVENT_SCREEN_LOADED, pc);
    lv_obj_add_event_cb(screen, playlists_screen_ctx_free_cb, LV_EVENT_DELETE, pc);
}

/* A clicked song row's context: the connection, plus the whole fetched song
 * collection this row belongs to (owned by the fetch) and this row's index
 * within it. Selecting a row queues the entire collection and starts at
 * `index`, so playback carries on through the album/playlist rather than
 * stopping after the one tapped track. */
typedef struct {
    rpod_mpd_t *mpd;
    const rpod_mpd_song_t *songs;
    size_t count;
    size_t index;
} song_row_t;

/* One decoded thumbnail per unique (artist, album) pair encountered while
 * building a song list -- see the dedup loop in build_song_list_screen. */
typedef struct {
    char artist[256];
    char album[256];
    lv_image_dsc_t dsc;
    bool has_art;
} art_slot_t;

typedef struct {
    rpod_mpd_t *mpd;
    rpod_screen_stack_t *stack;
    rpod_mpd_song_t *songs;
    size_t count;
    song_row_t *rows;
    art_slot_t *art_slots;
    size_t art_slot_count;
    /* Cover tiles for the collection header (build_collection_header()
     * below): one full-size cover for a single album, or up to four distinct
     * album covers laid out as a 2x2 mosaic for a playlist. Kept separate
     * from art_slots' 40px row thumbnails above. header_art_count is 0 for
     * lists with no header (flat/artist song lists) or when nothing decoded. */
    lv_image_dsc_t header_art_dsc[4];
    size_t header_art_count;
} song_list_fetch_t;

static void song_list_cleanup_cb(lv_event_t *e)
{
    song_list_fetch_t *fetch = lv_event_get_user_data(e);
    for (size_t i = 0; i < fetch->art_slot_count; i++) {
        if (fetch->art_slots[i].has_art) {
            free((void *)fetch->art_slots[i].dsc.data);
        }
    }
    free(fetch->art_slots);
    for (size_t i = 0; i < fetch->header_art_count; i++) {
        free((void *)fetch->header_art_dsc[i].data);
    }
    rpod_mpd_free_songs(fetch->songs);
    free(fetch->rows);
    free(fetch);
}

/* Fills an lv_image_dsc_t backed by an rpod_cover_art_t's RGB565 pixels --
 * same shape as now_playing.c's set_image_desc(), duplicated here since art
 * decoded for a list row lives in a differently-owned art_slot_t. */
static void set_thumb_desc(lv_image_dsc_t *dsc, const rpod_cover_art_t *art)
{
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0;
    dsc->header.w = (uint16_t)art->w;
    dsc->header.h = (uint16_t)art->h;
    dsc->header.stride = (uint16_t)(art->w * 2);
    dsc->data_size = (uint32_t)(art->w * art->h * 2);
    dsc->data = (const uint8_t *)art->pixels;
}

static void on_song_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    song_row_t *row = item_ctx;
    rpod_mpd_play_songs_from(row->mpd, row->songs, row->count, row->index);
    rpod_screen_stack_push(stack, rpod_now_playing_build, row->mpd, NULL);
}

static void on_add_songs_to_playlist(rpod_screen_stack_t *stack, void *item_ctx)
{
    music_ctx_t *filter = item_ctx;
    rpod_push_add_songs_screen(stack, filter->mpd, filter->playlist);
}

/* Square size (px) the collection header's cover tile renders at -- bigger
 * than a row thumbnail (RPOD_LIST_ART_SIZE), smaller than Now Playing's
 * ART_SIZE, since it shares the screen with the title/subtitle, the
 * Play/Shuffle row, and the rows below -- all of which need to fit inside
 * the list's own viewport height (RPOD_SCREEN_HEIGHT - RPOD_HEADER_HEIGHT -
 * 16, see rpod_list_screen_create()) for Play/Shuffle to be visible without
 * scrolling on first load. A playlist's 2x2 mosaic splits this tile into
 * four RPOD_HEADER_ART_SIZE/2 quadrants. */
#define RPOD_HEADER_ART_SIZE 72

static void on_play_collection_clicked(lv_event_t *e)
{
    song_list_fetch_t *fetch = lv_event_get_user_data(e);
    if (fetch->count == 0) {
        return;
    }
    rpod_mpd_play_songs(fetch->mpd, fetch->songs, fetch->count);
    rpod_screen_stack_push(fetch->stack, rpod_now_playing_build, fetch->mpd, NULL);
}

static void on_shuffle_collection_clicked(lv_event_t *e)
{
    song_list_fetch_t *fetch = lv_event_get_user_data(e);
    if (fetch->count == 0) {
        return;
    }
    rpod_mpd_play_songs_shuffled(fetch->mpd, fetch->songs, fetch->count);
    rpod_screen_stack_push(fetch->stack, rpod_now_playing_build, fetch->mpd, NULL);
}

/* Play/Shuffle sit at the *bottom* of the header, below the cover and
 * title/subtitle -- LVGL's own scroll-on-focus (or row_focus_cb's pattern for
 * plain rows) only scrolls far enough to reveal the focused object itself,
 * so re-focusing Play/Shuffle after scrolling down into the song list would
 * otherwise leave just the button row in view with the cover scrolled past
 * the top edge. Scroll the whole header (btn's grandparent: btn -> btn_row
 * -> header) into view instead, so the cover comes back with it. */
static void collection_header_button_focused_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *header = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_obj_scroll_to_view_recursive(header, LV_ANIM_OFF);
}

static lv_obj_t *build_collection_action_button(lv_obj_t *parent, const char *label,
                                                lv_event_cb_t on_click, song_list_fetch_t *fetch)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    /* See row_focus_cb in list_screen.c: LVGL's default eased scroll-on-focus
     * reads as laggy against a click wheel, and here it would also fight
     * with collection_header_button_focused_cb's own scroll target. */
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(btn, (RPOD_SCREEN_WIDTH - 16 - 2 * 14 - 12) / 2, 36);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_GLASS_FILL, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, RPOD_COLOR_TEXT, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, fetch);
    lv_obj_add_event_cb(btn, collection_header_button_focused_cb, LV_EVENT_FOCUSED, NULL);
    return btn;
}

/* Decodes up to four representative album covers into fetch's
 * header_art_dsc[] and renders them inside the header's `tile`: a single
 * cover fills the whole tile (a plain album), two-to-four lay out as a 2x2
 * mosaic (a playlist collage), each cover one quadrant. Fewer than four
 * decoded covers cycle to fill all four quadrants -- a half-blank mosaic
 * reads as broken, a repeated tile reads as intentional. Nothing decodable
 * falls back to an audio-symbol placeholder, same as a row with no art.
 * Decoding each cover straight to its on-screen size (full tile vs. quadrant)
 * follows list_screen.h's note against leaning on lv_image to rescale. */
static void build_header_cover(lv_obj_t *tile, song_list_fetch_t *fetch,
                               const char **cover_uris, size_t n_covers)
{
    bool mosaic = n_covers > 1;
    int cell = mosaic ? RPOD_HEADER_ART_SIZE / 2 : RPOD_HEADER_ART_SIZE;

    fetch->header_art_count = 0;
    for (size_t i = 0; i < n_covers && i < 4; i++) {
        unsigned char *raw = NULL;
        size_t raw_size = 0;
        rpod_cover_art_t decoded = { 0 };
        if (cover_uris[i] != NULL &&
            rpod_mpd_get_cover_art(fetch->mpd, cover_uris[i], &raw, &raw_size) &&
            rpod_cover_art_decode(raw, raw_size, cell, cell, &decoded)) {
            set_thumb_desc(&fetch->header_art_dsc[fetch->header_art_count++], &decoded);
        }
        rpod_mpd_free_cover_art(raw);
    }

    size_t k = fetch->header_art_count;
    if (k == 0) {
        lv_obj_t *placeholder = lv_label_create(tile);
        lv_label_set_text(placeholder, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(placeholder, RPOD_COLOR_DIM_TEXT, 0);
        lv_obj_center(placeholder);
    } else if (!mosaic) {
        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, &fetch->header_art_dsc[0]);
        lv_obj_set_size(img, RPOD_HEADER_ART_SIZE, RPOD_HEADER_ART_SIZE);
        lv_obj_center(img);
    } else {
        for (int q = 0; q < 4; q++) {
            lv_obj_t *img = lv_image_create(tile);
            lv_image_set_src(img, &fetch->header_art_dsc[q % (int)k]);
            lv_obj_set_size(img, cell, cell);
            lv_obj_set_pos(img, (q % 2) * cell, (q / 2) * cell);
        }
    }
}

/* Builds the collection header (a cover tile, a title + subtitle, then a
 * Play/Shuffle row) as `list`'s first child -- called before any song row is
 * added, so it scrolls together with the rows below it (Apple Music's
 * album/playlist-view pattern) *and* so Play/Shuffle join the screen's input
 * group before any row does. Group membership order is focus order: the first
 * widget added becomes the group's default-focused one, and a row added first
 * (as it was before this function existed) meant the click wheel landed on
 * the first song instead of Play when the screen opened.
 *
 * Shared by a single album's song list (title=album, subtitle=artist, one
 * cover) and a stored playlist's (title=playlist name, subtitle=song count,
 * up to four distinct album covers as a mosaic). Both cases skip per-row art
 * for the album but keep it for the playlist -- see build_song_list_screen's
 * `show_art`. `cover_uris`/`n_covers` are the representative track URIs whose
 * art the tile shows; see build_header_cover(). */
static void build_collection_header(lv_obj_t *list, song_list_fetch_t *fetch,
                                    const char *title, const char *subtitle,
                                    const char **cover_uris, size_t n_covers)
{
    lv_obj_t *header = lv_obj_create(list);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 10, 0);
    lv_obj_set_style_pad_row(header, 6, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, RPOD_COLOR_SEPARATOR, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_COVER, 0);

    lv_obj_t *art = lv_obj_create(header);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, RPOD_HEADER_ART_SIZE, RPOD_HEADER_ART_SIZE);
    lv_obj_set_style_radius(art, 10, 0);
    lv_obj_set_style_bg_color(art, RPOD_COLOR_GLASS_FILL, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(art, true, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);

    build_header_cover(art, fetch, cover_uris, n_covers);

    /* LONG_MODE_DOTS only truncates a label with a *fixed* height -- left at
     * the default size-content height, a long title/subtitle just wraps
     * instead of truncating, growing the header further (see the same note
     * on row title/subtitle labels in list_screen.c). Pin each to exactly
     * one line's height so it truncates with "..." instead. */
    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_label, RPOD_COLOR_TEXT, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_height(title_label, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);

    if (subtitle != NULL && subtitle[0] != '\0') {
        lv_obj_t *subtitle_label = lv_label_create(header);
        lv_label_set_text(subtitle_label, subtitle);
        lv_obj_set_style_text_color(subtitle_label, RPOD_COLOR_DIM_TEXT, 0);
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(subtitle_label, LV_PCT(100));
        lv_obj_set_height(subtitle_label, lv_font_get_line_height(LV_FONT_DEFAULT));
        lv_obj_set_style_text_align(subtitle_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *btn_row = lv_obj_create(header);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    build_collection_action_button(btn_row, LV_SYMBOL_PLAY " Play", on_play_collection_clicked, fetch);
    build_collection_action_button(btn_row, LV_SYMBOL_SHUFFLE " Shuffle", on_shuffle_collection_clicked, fetch);
}

/* First representative track URI per distinct (artist, album) pair in
 * `songs`, in the order each album first appears, capped at `max` (<= 4 --
 * the collection header's 2x2 mosaic) -- the covers that make up a playlist
 * header's collage. Dedups against the covers already picked (like the row-art
 * loop in build_song_list_screen), so a playlist that opens with a long run of
 * one album still reaches into later tracks for a varied mix. */
static size_t collect_distinct_cover_uris(const rpod_mpd_song_t *songs, size_t count,
                                          const char **out_uris, size_t max)
{
    if (max > 4) {
        max = 4;
    }
    size_t chosen[4];
    size_t n = 0;
    for (size_t i = 0; i < count && n < max; i++) {
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (strcmp(songs[chosen[j]].artist, songs[i].artist) == 0 &&
                strcmp(songs[chosen[j]].album, songs[i].album) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            chosen[n] = i;
            out_uris[n] = songs[i].uri;
            n++;
        }
    }
    return n;
}

static void build_song_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;

    rpod_mpd_song_t *songs = NULL;
    size_t count = 0;
    if (filter->playlist != NULL) {
        rpod_mpd_list_playlist_songs(filter->mpd, filter->playlist, &songs, &count);
    } else {
        rpod_mpd_list_songs(filter->mpd, filter->artist, filter->album, &songs, &count);
    }

    song_list_fetch_t *fetch = malloc(sizeof(*fetch));
    fetch->mpd = filter->mpd;
    fetch->stack = stack;
    fetch->songs = songs;
    fetch->count = count;
    fetch->rows = count > 0 ? malloc(count * sizeof(*fetch->rows)) : NULL;
    for (size_t i = 0; i < count; i++) {
        fetch->rows[i].mpd = filter->mpd;
        fetch->rows[i].songs = songs;
        fetch->rows[i].count = count;
        fetch->rows[i].index = i;
    }
    fetch->art_slots = NULL;
    fetch->art_slot_count = 0;
    fetch->header_art_count = 0;

    /* Cover art on the left of each row -- except when every row is already
     * known to share the same art, i.e. this list is a single album's songs
     * (filter->album set). One thumbnail is fetched+decoded per unique
     * (artist, album) pair rather than per song, since flat/playlist song
     * lists commonly run several consecutive tracks from the same album. */
    bool show_art = filter->album == NULL;
    size_t *song_art_slot = NULL;
    if (show_art && count > 0) {
        fetch->art_slots = calloc(count, sizeof(*fetch->art_slots));
        song_art_slot = malloc(count * sizeof(*song_art_slot));
        for (size_t i = 0; i < count; i++) {
            size_t slot = SIZE_MAX;
            for (size_t j = 0; j < fetch->art_slot_count; j++) {
                if (strcmp(fetch->art_slots[j].artist, songs[i].artist) == 0 &&
                    strcmp(fetch->art_slots[j].album, songs[i].album) == 0) {
                    slot = j;
                    break;
                }
            }
            if (slot == SIZE_MAX) {
                slot = fetch->art_slot_count++;
                art_slot_t *s = &fetch->art_slots[slot];
                snprintf(s->artist, sizeof(s->artist), "%s", songs[i].artist);
                snprintf(s->album, sizeof(s->album), "%s", songs[i].album);

                unsigned char *raw = NULL;
                size_t raw_size = 0;
                rpod_cover_art_t art = { 0 };
                if (rpod_mpd_get_cover_art(filter->mpd, songs[i].uri, &raw, &raw_size) &&
                    rpod_cover_art_decode(raw, raw_size, RPOD_LIST_ART_SIZE, RPOD_LIST_ART_SIZE, &art)) {
                    set_thumb_desc(&s->dsc, &art);
                    s->has_art = true;
                }
                rpod_mpd_free_cover_art(raw);
            }
            song_art_slot[i] = slot;
        }
    }

    lv_obj_add_event_cb(screen, song_list_cleanup_cb, LV_EVENT_DELETE, fetch);

    /* Only a specific stored playlist's song list gets the "Add Songs..."
     * leading row -- not the flat Songs list or an artist/album's songs. */
    size_t lead = filter->playlist != NULL ? 1 : 0;
    rpod_list_item_t *ui_items = calloc(count + lead, sizeof(*ui_items));
    if (lead > 0) {
        snprintf(ui_items[0].text, sizeof(ui_items[0].text), "Add Songs...");
        ui_items[0].chevron = true;
        ui_items[0].on_select = on_add_songs_to_playlist;
        ui_items[0].item_ctx = filter;
    }
    for (size_t i = 0; i < count; i++) {
        rpod_list_item_t *item = &ui_items[i + lead];
        const char *label = songs[i].title[0] != '\0' ? songs[i].title : songs[i].uri;
        snprintf(item->text, sizeof(item->text), "%.*s", (int)sizeof(item->text) - 1, label);

        if (songs[i].artist[0] != '\0' && songs[i].album[0] != '\0') {
            snprintf(item->subtitle, sizeof(item->subtitle), "%s - %s", songs[i].artist, songs[i].album);
        } else if (songs[i].artist[0] != '\0') {
            snprintf(item->subtitle, sizeof(item->subtitle), "%s", songs[i].artist);
        } else if (songs[i].album[0] != '\0') {
            snprintf(item->subtitle, sizeof(item->subtitle), "%s", songs[i].album);
        }

        if (songs[i].duration_s > 0) {
            snprintf(item->accessory, sizeof(item->accessory), "%u:%02u",
                     songs[i].duration_s / 60u, songs[i].duration_s % 60u);
        }

        if (show_art) {
            item->has_art_slot = true;
            art_slot_t *s = &fetch->art_slots[song_art_slot[i]];
            item->thumb = s->has_art ? &s->dsc : NULL;
        }

        item->on_select = on_song_select;
        item->item_ctx = &fetch->rows[i];
    }
    free(song_art_slot);

    /* A single album or a stored playlist gets a header (cover + title +
     * subtitle + Play/Shuffle) above the track list; the flat Songs list and
     * artist-scoped lists don't. Built (and so group-registered) before the
     * rows below it -- see build_collection_header()'s comment on why order
     * matters here. The album shows one cover and its artist; the playlist a
     * 2x2 mosaic of up to four distinct album covers and its track count. */
    lv_obj_t *list = rpod_list_screen_create(screen);
    if (filter->album != NULL && count > 0) {
        const char *cover_uris[1] = { songs[0].uri };
        build_collection_header(list, fetch, filter->album, songs[0].artist, cover_uris, 1);
    } else if (filter->playlist != NULL && count > 0) {
        const char *cover_uris[4];
        size_t n_covers = collect_distinct_cover_uris(songs, count, cover_uris, 4);
        char subtitle[32];
        snprintf(subtitle, sizeof(subtitle), "%zu %s", count, count == 1 ? "song" : "songs");
        build_collection_header(list, fetch, filter->playlist, subtitle, cover_uris, n_covers);
    }
    rpod_list_screen_populate(stack, list, ui_items, count + lead);
    free(ui_items);
}

void rpod_music_push_artist_albums(rpod_screen_stack_t *stack, rpod_mpd_t *mpd, const char *artist)
{
    rpod_screen_stack_push(stack, build_album_list_screen,
                            music_ctx_new(mpd, artist, NULL, NULL, NULL), music_ctx_free);
}

void rpod_music_push_album_songs(rpod_screen_stack_t *stack, rpod_mpd_t *mpd,
                                  const char *artist_or_null, const char *album)
{
    rpod_screen_stack_push(stack, build_song_list_screen,
                            music_ctx_new(mpd, artist_or_null, album, NULL, NULL), music_ctx_free);
}

static void on_music_menu_playlists(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_playlist_list_screen,
                            music_ctx_new(item_ctx, NULL, NULL, NULL, NULL), music_ctx_free);
}

static void on_music_menu_artists(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_artist_list_screen,
                            music_ctx_new(item_ctx, NULL, NULL, NULL, NULL), music_ctx_free);
}

static void on_music_menu_albums(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_album_list_screen,
                            music_ctx_new(item_ctx, NULL, NULL, NULL, NULL), music_ctx_free);
}

static void on_music_menu_songs(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_song_list_screen,
                            music_ctx_new(item_ctx, NULL, NULL, NULL, NULL), music_ctx_free);
}

static void on_music_menu_genres(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_genre_list_screen,
                            music_ctx_new(item_ctx, NULL, NULL, NULL, NULL), music_ctx_free);
}

static void on_music_menu_search(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_search_screen_build, item_ctx, NULL);
}

void rpod_music_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    rpod_mpd_t *mpd = ctx;

    rpod_list_item_t items[] = {
        { .text = "Search",    .chevron = true, .on_select = on_music_menu_search,    .item_ctx = mpd },
        { .text = "Playlists", .chevron = true, .on_select = on_music_menu_playlists, .item_ctx = mpd },
        { .text = "Artists",   .chevron = true, .on_select = on_music_menu_artists,   .item_ctx = mpd },
        { .text = "Albums",    .chevron = true, .on_select = on_music_menu_albums,    .item_ctx = mpd },
        { .text = "Songs",     .chevron = true, .on_select = on_music_menu_songs,     .item_ctx = mpd },
        { .text = "Genres",    .chevron = true, .on_select = on_music_menu_genres,    .item_ctx = mpd },
    };
    rpod_list_screen_build(stack, screen, items, sizeof(items) / sizeof(items[0]));
}
