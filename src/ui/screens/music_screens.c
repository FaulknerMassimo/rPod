#include "music_screens.h"

#include "list_screen.h"
#include "now_playing.h"
#include "search_screen.h"
#include "playlist_edit_screens.h"
#include "playlist_picker.h"
#include "audio/mpd_client.h"
#include "ui/alpha_sort.h"
#include "ui/cover_cache.h"
#include "ui/heart_icon.h"
#include "ui/metrics.h"
#include "ui/playlist_membership.h"
#include "ui/scrub.h"
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

static int item_alpha_cmp(const void *a, const void *b)
{
    return rpod_alpha_compare(((const rpod_mpd_item_t *)a)->name, ((const rpod_mpd_item_t *)b)->name);
}

/* Shared plumbing for every name-only browse screen: takes ownership of
 * `items_raw` (already fetched by the caller), builds the on-screen list,
 * and wires each row's item_ctx to a name_row_t carrying `parent_artist`
 * through (NULL where it doesn't apply).
 *
 * Rows go in iPod order (ui/alpha_sort.h) rather than MPD's: its `list` is a
 * plain byte sort, which puts lowercase names after every capitalised one
 * and accented ones after Z -- and the alphabet scrub needs each letter's
 * rows together. */
static void build_name_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen,
                                    rpod_mpd_t *mpd, rpod_mpd_item_t *items_raw, size_t count,
                                    const char *parent_artist,
                                    void (*on_select)(rpod_screen_stack_t *, void *))
{
    if (count > 1) {
        qsort(items_raw, count, sizeof(*items_raw), item_alpha_cmp);
    }

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
    lv_obj_t *list = rpod_list_screen_build(stack, screen, ui_items, count);
    rpod_list_screen_enable_scrub(screen, list);
    free(ui_items);
}

static void build_song_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);
static void build_album_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);
static void build_genre_artist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);
static rpod_row_status_t status_for_uri(const rpod_playlist_index_t *idx, const char *uri);

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
    /* One extra row slot for the pinned "Liked Songs" entry (row index
     * `count`), whose name is a stable string literal rather than an
     * items_raw[] pointer. */
    fetch->rows = malloc((count + 1) * sizeof(*fetch->rows));
    for (size_t i = 0; i < count; i++) {
        fetch->rows[i].mpd = mpd;
        fetch->rows[i].name = items[i].name;
        fetch->rows[i].parent_artist = NULL;
    }
    fetch->rows[count].mpd = mpd;
    fetch->rows[count].name = RPOD_LIKED_PLAYLIST_NAME;
    fetch->rows[count].parent_artist = NULL;

    /* Leading rows: "New Playlist", then "Liked Songs" pinned above the rest
     * (shown even before it exists server-side). A real "Liked Songs" among
     * the fetched playlists is skipped below so it isn't listed twice. */
    rpod_list_item_t *ui_items = calloc(count + 2, sizeof(*ui_items));
    size_t ui = 0;
    snprintf(ui_items[ui].text, sizeof(ui_items[ui].text), "New Playlist");
    ui_items[ui].chevron = true;
    ui_items[ui].on_select = on_new_playlist;
    ui_items[ui].item_ctx = mpd;
    ui++;
    snprintf(ui_items[ui].text, sizeof(ui_items[ui].text), "%s", RPOD_LIKED_PLAYLIST_NAME);
    ui_items[ui].chevron = true;
    ui_items[ui].on_select = on_playlist_select;
    ui_items[ui].item_ctx = &fetch->rows[count];
    ui++;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i].name, RPOD_LIKED_PLAYLIST_NAME) == 0) {
            continue;
        }
        snprintf(ui_items[ui].text, sizeof(ui_items[ui].text), "%s", items[i].name);
        ui_items[ui].chevron = true;
        ui_items[ui].on_select = on_playlist_select;
        ui_items[ui].item_ctx = &fetch->rows[i];
        ui++;
    }
    rpod_list_screen_build(stack, screen, ui_items, ui);
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

typedef struct {
    rpod_mpd_t *mpd;
    rpod_screen_stack_t *stack;
    rpod_mpd_song_t *songs;
    size_t count;
    song_row_t *rows;
    bool show_art; /* rows carry a cover column (see build_song_list_screen) */
    /* The collection header's cover tile (build_collection_header() below)
     * and the songs whose album covers it shows: one for a single album, up
     * to four distinct albums as a 2x2 mosaic for a playlist. Covers decode in
     * the background (ui/cover_cache.h); refresh_header_cover() fills the tile
     * once they've all resolved. header_tile is NULL for lists with no header
     * (artist song lists). */
    lv_obj_t *header_tile;
    size_t header_cover_songs[4];
    size_t header_cover_count;
    bool header_filled;

    /* Liked/playlist indicators. The index is (re)built and the rows'
     * trailing marks refreshed whenever the screen regains focus (see
     * song_list_loaded_cb), so a like made on Now Playing or an edit made in
     * the Add-to-Playlist picker shows on return. `list`, `lead`, and
     * `header_present` locate the song rows among the list's children. */
    rpod_playlist_index_t *index;
    lv_obj_t *list;
    size_t lead;
    bool header_present;
} song_list_fetch_t;

static void song_list_cleanup_cb(lv_event_t *e)
{
    song_list_fetch_t *fetch = lv_event_get_user_data(e);
    rpod_playlist_index_free(fetch->index);
    rpod_mpd_free_songs(fetch->songs);
    free(fetch->rows);
    free(fetch);
}

/* On (re)gaining focus: refresh the playlist index and every song row's mark.
 * Built here rather than at build time so the first focus and every return
 * share one path -- see the same pattern in build_playlist_list_screen. */
static void song_list_loaded_cb(lv_event_t *e)
{
    song_list_fetch_t *fetch = lv_event_get_user_data(e);
    if (fetch->index == NULL) {
        fetch->index = rpod_playlist_index_build(fetch->mpd);
    } else {
        rpod_playlist_index_rebuild(fetch->index, fetch->mpd);
    }

    size_t base = (fetch->header_present ? 1 : 0) + fetch->lead;
    for (size_t j = 0; j < fetch->count; j++) {
        lv_obj_t *btn = lv_obj_get_child(fetch->list, (int32_t)(base + j));
        if (btn == NULL) {
            break;
        }
        rpod_list_row_set_status(btn, status_for_uri(fetch->index, fetch->songs[j].uri));
    }
}

static void on_song_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    song_row_t *row = item_ctx;
    rpod_mpd_play_songs_from(row->mpd, row->songs, row->count, row->index);
    rpod_screen_stack_push(stack, rpod_now_playing_build, row->mpd, NULL);
}

/* Press-and-hold a song row: open the Add-to-Playlist picker for it. */
static void on_song_long_press(rpod_screen_stack_t *stack, void *item_ctx)
{
    song_row_t *row = item_ctx;
    const rpod_mpd_song_t *s = &row->songs[row->index];
    rpod_playlist_picker_push(stack, row->mpd, s->uri, s->title);
}

/* Heart for a liked song, checkmark for one in some other playlist, else
 * nothing -- the heart wins when a song is both liked and in a playlist. */
static rpod_row_status_t status_for_uri(const rpod_playlist_index_t *idx, const char *uri)
{
    if (rpod_playlist_index_is_liked(idx, uri)) {
        return RPOD_ROW_STATUS_HEART;
    }
    if (rpod_playlist_index_in_other(idx, uri)) {
        return RPOD_ROW_STATUS_CHECK;
    }
    return RPOD_ROW_STATUS_NONE;
}

static void on_add_songs_to_playlist(rpod_screen_stack_t *stack, void *item_ctx)
{
    music_ctx_t *filter = item_ctx;
    rpod_push_add_songs_screen(stack, filter->mpd, filter->playlist);
}

/* Square size (px) the collection header's cover tile renders at -- bigger
 * than a row thumbnail (rpod_metrics()->list_art_size), smaller than Now
 * Playing's ART_SIZE, since it shares the screen with the title/subtitle, the
 * Play/Shuffle row, and the rows below -- all of which need to fit inside
 * the list's own viewport height (screen_h - header_h - 16, see
 * rpod_list_screen_create()) for Play/Shuffle to be visible without scrolling
 * on first load. A playlist's 2x2 mosaic splits this tile into four
 * RPOD_HEADER_ART_SIZE/2 quadrants. */
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
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_set_size(btn, (m->screen_w - 16 - 2 * 14 - 12) / 2, 36);
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

/* Fills the header's cover tile once every requested cover has resolved:
 * a single cover fills the whole tile (a plain album), two-to-four lay out
 * as a 2x2 mosaic (a playlist collage), each cover one quadrant. Fewer than
 * four decoded covers cycle to fill all four quadrants -- a half-blank
 * mosaic reads as broken, a repeated tile reads as intentional. Nothing
 * decodable keeps the audio-symbol placeholder, same as a row with no art.
 * Waits for all of them rather than drawing progressively, so a mosaic
 * doesn't visibly reshuffle as each cover lands. Each cover is requested at
 * its on-screen size (full tile vs. quadrant), per list_screen.h's note
 * against leaning on lv_image to rescale. Called at build time (covers may
 * already be cached) and again as background decodes finish. */
static void refresh_header_cover(song_list_fetch_t *fetch)
{
    if (fetch->header_tile == NULL || fetch->header_filled) {
        return;
    }
    bool mosaic = fetch->header_cover_count > 1;
    int cell = mosaic ? RPOD_HEADER_ART_SIZE / 2 : RPOD_HEADER_ART_SIZE;

    const lv_image_dsc_t *ready[4];
    size_t k = 0;
    for (size_t i = 0; i < fetch->header_cover_count; i++) {
        const rpod_mpd_song_t *s = &fetch->songs[fetch->header_cover_songs[i]];
        bool pending = false;
        const lv_image_dsc_t *dsc = rpod_cover_cache_get(s->artist, s->album, s->uri, cell, &pending);
        if (pending) {
            return;
        }
        if (dsc != NULL) {
            ready[k++] = dsc;
        }
    }
    fetch->header_filled = true;
    if (k == 0) {
        return;
    }

    lv_obj_t *tile = fetch->header_tile;
    lv_obj_clean(tile);
    if (!mosaic) {
        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, ready[0]);
        lv_obj_set_size(img, RPOD_HEADER_ART_SIZE, RPOD_HEADER_ART_SIZE);
        lv_obj_center(img);
    } else {
        for (int q = 0; q < 4; q++) {
            lv_obj_t *img = lv_image_create(tile);
            lv_image_set_src(img, ready[q % (int)k]);
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
 * `show_art`. `cover_songs`/`n_covers` index the songs whose album art the
 * tile shows; see refresh_header_cover(). */
static void build_collection_header(lv_obj_t *list, song_list_fetch_t *fetch,
                                    const char *title, const char *subtitle,
                                    const size_t *cover_songs, size_t n_covers)
{
    const rpod_metrics_t *m = rpod_metrics();

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

    lv_obj_t *placeholder = lv_label_create(art);
    lv_label_set_text(placeholder, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(placeholder, m->font_np_glyph, 0);
    lv_obj_set_style_text_color(placeholder, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_center(placeholder);

    fetch->header_tile = art;
    fetch->header_cover_count = n_covers < 4 ? n_covers : 4;
    memcpy(fetch->header_cover_songs, cover_songs, fetch->header_cover_count * sizeof(*cover_songs));
    refresh_header_cover(fetch);

    /* LONG_MODE_DOTS only truncates a label with a *fixed* height -- left at
     * the default size-content height, a long title/subtitle just wraps
     * instead of truncating, growing the header further (see the same note
     * on row title/subtitle labels in list_screen.c). Pin each to exactly
     * one line's height so it truncates with "..." instead. */
    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, m->font_body, 0);
    lv_obj_set_style_text_color(title_label, RPOD_COLOR_TEXT, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_height(title_label, lv_font_get_line_height(m->font_body));
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);

    if (subtitle != NULL && subtitle[0] != '\0') {
        lv_obj_t *subtitle_label = lv_label_create(header);
        lv_label_set_text(subtitle_label, subtitle);
        lv_obj_set_style_text_color(subtitle_label, RPOD_COLOR_DIM_TEXT, 0);
        lv_obj_set_style_text_font(subtitle_label, m->font_small, 0);
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(subtitle_label, LV_PCT(100));
        lv_obj_set_height(subtitle_label, lv_font_get_line_height(m->font_small));
        lv_obj_set_style_text_align(subtitle_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *btn_row = lv_obj_create(header);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    build_collection_action_button(btn_row, LV_SYMBOL_PLAY " Play",
                                   on_play_collection_clicked, fetch);
    build_collection_action_button(btn_row, LV_SYMBOL_SHUFFLE " Shuffle",
                                   on_shuffle_collection_clicked, fetch);
}

/* Index of the first track per distinct (artist, album) pair in `songs`,
 * in the order each album first appears, capped at `max` (<= 4 -- the
 * collection header's 2x2 mosaic) -- the covers that make up a playlist
 * header's collage. Dedups against the covers already picked, so a playlist
 * that opens with a long run of one album still reaches into later tracks
 * for a varied mix. */
static size_t collect_distinct_cover_songs(const rpod_mpd_song_t *songs, size_t count,
                                           size_t *out, size_t max)
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
            out[n] = i;
            n++;
        }
    }
    return n;
}

/* ---- Virtualized flat "Songs" list ------------------------------------
 *
 * The whole-library song list runs into the hundreds/thousands of rows; the
 * plain list_screen builds one widget subtree per row, which makes the screen
 * take seconds to build and drops scrolling to single-digit fps (both are
 * O(row-count): moving/invalidating every child on scroll, and 258 synchronous
 * cover-art decodes up front). This view instead keeps a small fixed pool of
 * ~4 row widgets and rebinds them to a sliding data window as you scroll
 * (whole-row shift), so only a handful of widgets ever exist. Covers decode
 * in the background (ui/cover_cache.h), so the screen appears instantly and
 * art fills in.
 *
 * Navigation can't use lv_group focus (there's no per-row widget to focus):
 * a single proxy object is the group's only member, put in edit mode so the
 * encoder delivers rotation as LV_KEY_LEFT/RIGHT and select as LV_EVENT_CLICKED
 * to vsong_proxy_event(); selection + highlight are drawn by hand. The first
 * two selectable items are "Play All" / "Shuffle All" (this list's stand-in
 * for the album/playlist header's Play/Shuffle).
 *
 * Songs are in title order (ui/alpha_sort.h), like the iPod's Songs list --
 * MPD's listing comes in file order -- and a fast flick scrubs by letter
 * (vsong_scrub_cb). */

/* Fixed pooled-row height for the virtual list's geometry math -- must match
 * the row vsong_row_create() actually builds: art (list_art_size) + vertical
 * padding, or the taller title+subtitle text column, whichever wins. */
#define VSONG_ROW_H 56
#define VSONG_LEAD  2    /* leading items: 0 = Play All, 1 = Shuffle All */

/* One pooled row widget: built once, rebound to different data as we scroll. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *art;
    lv_obj_t *art_img;   /* hidden until a cover is available */
    lv_obj_t *art_ph;    /* placeholder symbol (audio / play / shuffle) */
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *accessory;
    lv_obj_t *heart;     /* liked indicator; hidden unless the song is liked */
    lv_obj_t *check;     /* in-a-playlist indicator; hidden unless applicable */
    long bound;          /* selection index shown, or -1 if hidden */
} vsong_row_t;

typedef struct {
    rpod_mpd_t *mpd;
    rpod_screen_stack_t *stack;
    rpod_mpd_song_t *songs;   /* owned */
    size_t count;
    size_t n_items;           /* VSONG_LEAD + count */
    size_t sel;               /* selected item */
    size_t win_start;         /* item shown by pool[0] */
    size_t vis;               /* fully-visible rows (selection stays within) */
    size_t pool_n;            /* vis + 1 (last is a partly-clipped peek row) */
    vsong_row_t *pool;
    lv_obj_t *panel;
    lv_obj_t *proxy;
    rpod_playlist_index_t *index; /* liked/playlist marks; refreshed on focus */
} vsong_t;

static vsong_row_t vsong_row_create(lv_obj_t *panel)
{
    const rpod_metrics_t *m = rpod_metrics();
    vsong_row_t r = { 0 };
    r.row = lv_obj_create(panel);
    lv_obj_remove_style_all(r.row);
    lv_obj_set_size(r.row, LV_PCT(100), VSONG_ROW_H);
    lv_obj_set_style_pad_hor(r.row, m->row_pad_x, 0);
    lv_obj_set_style_pad_column(r.row, m->row_gap, 0);
    lv_obj_set_flex_flow(r.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r.row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(r.row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r.row, 1, 0);
    lv_obj_set_style_border_color(r.row, RPOD_COLOR_SEPARATOR, 0);
    lv_obj_set_style_border_opa(r.row, LV_OPA_COVER, 0);

    r.art = lv_obj_create(r.row);
    lv_obj_remove_style_all(r.art);
    lv_obj_set_size(r.art, m->list_art_size, m->list_art_size);
    lv_obj_set_style_radius(r.art, 6, 0);
    lv_obj_set_style_bg_color(r.art, RPOD_COLOR_GLASS_FILL, 0);
    lv_obj_set_style_bg_opa(r.art, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(r.art, true, 0);
    lv_obj_remove_flag(r.art, LV_OBJ_FLAG_SCROLLABLE);

    r.art_ph = lv_label_create(r.art);
    lv_label_set_text(r.art_ph, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(r.art_ph, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_center(r.art_ph);

    r.art_img = lv_image_create(r.art);
    lv_obj_set_size(r.art_img, m->list_art_size, m->list_art_size);
    lv_obj_center(r.art_img);
    lv_obj_add_flag(r.art_img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *col = lv_obj_create(r.row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(col, 2, 0);

    r.title = lv_label_create(col);
    lv_obj_set_style_text_font(r.title, m->font_body, 0);
    lv_obj_set_style_text_color(r.title, RPOD_COLOR_TEXT, 0);
    lv_label_set_long_mode(r.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(r.title, LV_PCT(100));
    lv_obj_set_height(r.title, lv_font_get_line_height(m->font_body));

    r.subtitle = lv_label_create(col);
    lv_obj_set_style_text_color(r.subtitle, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_style_text_font(r.subtitle, m->font_small, 0);
    lv_label_set_long_mode(r.subtitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(r.subtitle, LV_PCT(100));
    lv_obj_set_height(r.subtitle, lv_font_get_line_height(m->font_small));

    r.accessory = lv_label_create(r.row);
    lv_obj_set_style_text_color(r.accessory, RPOD_COLOR_DIM_TEXT, 0);

    /* Trailing liked/playlist marks -- both built once and shown/hidden per
     * bind, since a hidden object is skipped by the row's flex layout. */
    r.check = lv_label_create(r.row);
    lv_label_set_text(r.check, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(r.check, RPOD_COLOR_TEXT, 0);
    lv_obj_add_flag(r.check, LV_OBJ_FLAG_HIDDEN);

    r.heart = rpod_heart_create(r.row, m->row_heart_size);
    rpod_heart_set_liked(r.heart, true, false);
    lv_obj_add_flag(r.heart, LV_OBJ_FLAG_HIDDEN);

    r.bound = -1;
    return r;
}

/* Shows `s`'s album cover on a pooled row, or the placeholder glyph while
 * it's still decoding (or if it has none). */
static void vsong_show_cover(vsong_row_t *r, const rpod_mpd_song_t *s)
{
    const lv_image_dsc_t *dsc = rpod_cover_cache_get(s->artist, s->album, s->uri,
                                                     rpod_metrics()->list_art_size, NULL);
    if (dsc != NULL) {
        lv_image_set_src(r->art_img, dsc);
        lv_obj_remove_flag(r->art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->art_ph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r->art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->art_ph, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(r->art_ph, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(r->art_ph, RPOD_COLOR_DIM_TEXT, 0);
    }
}

static void vsong_bind(vsong_t *v, vsong_row_t *r, long item)
{
    if (item < 0 || (size_t)item >= v->n_items) {
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        r->bound = -1;
        return;
    }
    lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    r->bound = item;

    bool selected = ((size_t)item == v->sel);
    lv_obj_set_style_bg_color(r->row, RPOD_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(r->row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_color_t dim = selected ? RPOD_COLOR_TEXT : RPOD_COLOR_DIM_TEXT;

    if (item < VSONG_LEAD) {
        lv_obj_add_flag(r->art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->art_ph, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(r->art_ph, item == 0 ? LV_SYMBOL_PLAY : LV_SYMBOL_SHUFFLE);
        lv_obj_set_style_text_color(r->art_ph, selected ? RPOD_COLOR_TEXT : RPOD_COLOR_ACCENT, 0);
        lv_label_set_text(r->title, item == 0 ? "Play All" : "Shuffle All");
        lv_obj_add_flag(r->subtitle, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(r->accessory, "");
        lv_obj_add_flag(r->heart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->check, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const rpod_mpd_song_t *s = &v->songs[item - VSONG_LEAD];

    /* Liked heart wins over the in-a-playlist checkmark; a song in neither
     * shows nothing. */
    rpod_row_status_t status = status_for_uri(v->index, s->uri);
    if (status == RPOD_ROW_STATUS_HEART) {
        lv_obj_remove_flag(r->heart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->check, LV_OBJ_FLAG_HIDDEN);
    } else if (status == RPOD_ROW_STATUS_CHECK) {
        lv_obj_add_flag(r->heart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->check, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r->heart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->check, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(r->title, s->title[0] != '\0' ? s->title : s->uri);

    char sub[520];
    if (s->artist[0] != '\0' && s->album[0] != '\0') {
        snprintf(sub, sizeof(sub), "%s - %s", s->artist, s->album);
    } else if (s->artist[0] != '\0') {
        snprintf(sub, sizeof(sub), "%s", s->artist);
    } else if (s->album[0] != '\0') {
        snprintf(sub, sizeof(sub), "%s", s->album);
    } else {
        sub[0] = '\0';
    }
    lv_obj_remove_flag(r->subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(r->subtitle, sub);
    lv_obj_set_style_text_color(r->subtitle, dim, 0);

    if (s->duration_s > 0) {
        char d[16];
        snprintf(d, sizeof(d), "%u:%02u", s->duration_s / 60u, s->duration_s % 60u);
        lv_label_set_text(r->accessory, d);
    } else {
        lv_label_set_text(r->accessory, "");
    }
    lv_obj_set_style_text_color(r->accessory, dim, 0);

    vsong_show_cover(r, s);
}

static void vsong_relayout(vsong_t *v)
{
    for (size_t i = 0; i < v->pool_n; i++) {
        vsong_bind(v, &v->pool[i], (long)v->win_start + (long)i);
        lv_obj_set_pos(v->pool[i].row, 0, (int32_t)(i * VSONG_ROW_H));
    }
}

static void vsong_move(vsong_t *v, int delta)
{
    long ns = (long)v->sel + delta;
    if (ns < 0) {
        ns = 0;
    }
    if ((size_t)ns >= v->n_items) {
        ns = (long)v->n_items - 1;
    }
    if ((size_t)ns == v->sel) {
        return;
    }
    v->sel = (size_t)ns;
    /* Keep the selection inside the fully-visible rows, shifting the window
     * (and so rebinding every pooled row) only when it would fall off an edge. */
    if (v->sel < v->win_start) {
        v->win_start = v->sel;
    } else if (v->sel >= v->win_start + v->vis) {
        v->win_start = v->sel - v->vis + 1;
    }
    vsong_relayout(v);
}

static void vsong_activate(vsong_t *v)
{
    if (v->count == 0) {
        return;
    }
    if (v->sel == 0) {
        rpod_mpd_play_songs(v->mpd, v->songs, v->count);
    } else if (v->sel == 1) {
        rpod_mpd_play_songs_shuffled(v->mpd, v->songs, v->count);
    } else {
        rpod_mpd_play_songs_from(v->mpd, v->songs, v->count, v->sel - VSONG_LEAD);
    }
    rpod_screen_stack_push(v->stack, rpod_now_playing_build, v->mpd, NULL);
}

/* Press-and-hold the selected song: open the Add-to-Playlist picker (the two
 * leading Play All / Shuffle All items have no hold action). */
static void vsong_open_picker(vsong_t *v)
{
    if (v->sel < VSONG_LEAD) {
        return;
    }
    const rpod_mpd_song_t *s = &v->songs[v->sel - VSONG_LEAD];
    rpod_playlist_picker_push(v->stack, v->mpd, s->uri, s->title);
}

/* Select is on SHORT_CLICKED; a hold opens the picker immediately instead of
 * also playing -- same split, and the same lv_indev_wait_release() reasoning,
 * as the plain list rows (see list_screen.c's row handlers). */
static void vsong_proxy_event(lv_event_t *e)
{
    vsong_t *v = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY) {
        uint32_t k = lv_event_get_key(e);
        if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN) {
            vsong_move(v, +1);
        } else if (k == LV_KEY_LEFT || k == LV_KEY_UP) {
            vsong_move(v, -1);
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        vsong_activate(v);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        lv_indev_wait_release(lv_event_get_indev(e));
        vsong_open_picker(v);
    }
}

/* Refresh the liked/playlist marks whenever the list regains focus (first
 * push, and every return from the picker or Now Playing). */
static void vsong_loaded_cb(lv_event_t *e)
{
    vsong_t *v = lv_event_get_user_data(e);
    if (v->index == NULL) {
        v->index = rpod_playlist_index_build(v->mpd);
    } else {
        rpod_playlist_index_rebuild(v->index, v->mpd);
    }
    vsong_relayout(v);
}

static const char *vsong_title(const rpod_mpd_song_t *s)
{
    return s->title[0] != '\0' ? s->title : s->uri;
}

static int song_title_cmp(const void *a, const void *b)
{
    return rpod_alpha_compare(vsong_title(a), vsong_title(b));
}

static const char *vsong_scrub_name(const void *ctx, size_t i)
{
    return vsong_title(&((const vsong_t *)ctx)->songs[i]);
}

/* Alphabet scrub (ui/scrub.h) over the songs; Play All / Shuffle All sit
 * before the first letter. A jump puts the letter's first song at the top
 * of the window, so as much of the letter as fits is on screen. */
static void vsong_scrub_cb(lv_event_t *e)
{
    vsong_t *v = lv_event_get_user_data(e);
    rpod_scrub_param_t *param = lv_event_get_param(e);
    if (v->count < RPOD_SCRUB_MIN_ROWS) {
        return;
    }

    size_t cur = v->sel < VSONG_LEAD ? 0 : v->sel - VSONG_LEAD;
    size_t target = cur;
    bool move;
    if (v->sel < VSONG_LEAD) {
        move = param->dir > 0; /* onto the first letter; back has nowhere to go */
    } else {
        target = rpod_alpha_jump(vsong_scrub_name, v, v->count, cur, param->dir);
        move = target != cur;
    }

    if (move) {
        v->sel = VSONG_LEAD + target;
        size_t last_start = v->n_items > v->vis ? v->n_items - v->vis : 0;
        v->win_start = v->sel < last_start ? v->sel : last_start;
        vsong_relayout(v);
    }
    param->letter = rpod_alpha_letter(vsong_scrub_name(v, target));
    param->handled = true;
}

/* Newly decoded covers: re-show art on every pooled row bound to a song. */
static void vsong_covers_ready_cb(void *user)
{
    vsong_t *v = user;
    for (size_t i = 0; i < v->pool_n; i++) {
        long b = v->pool[i].bound;
        if (b >= VSONG_LEAD) {
            vsong_show_cover(&v->pool[i], &v->songs[b - VSONG_LEAD]);
        }
    }
}

static void vsong_cleanup(lv_event_t *e)
{
    vsong_t *v = lv_event_get_user_data(e);
    free(v->pool);
    rpod_playlist_index_free(v->index);
    rpod_mpd_free_songs(v->songs);
    free(v);
}

/* Takes ownership of `songs`. */
static void build_virtual_song_list(rpod_screen_stack_t *stack, lv_obj_t *screen,
                                    rpod_mpd_t *mpd, rpod_mpd_song_t *songs, size_t count)
{
    if (count > 1) {
        qsort(songs, count, sizeof(*songs), song_title_cmp);
    }

    vsong_t *v = calloc(1, sizeof(*v));
    v->mpd = mpd;
    v->stack = stack;
    v->songs = songs;
    v->count = count;
    v->n_items = VSONG_LEAD + count;

    const rpod_metrics_t *m = rpod_metrics();
    int32_t viewport = m->screen_h - m->header_h - 16;
    v->vis = (size_t)(viewport / VSONG_ROW_H);
    if (v->vis < 1) {
        v->vis = 1;
    }
    v->pool_n = v->vis + 1;

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, m->screen_w - 16, viewport);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -8);
    rpod_theme_style_glass_panel(panel, 12);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    v->panel = panel;

    v->pool = calloc(v->pool_n, sizeof(*v->pool));
    for (size_t i = 0; i < v->pool_n; i++) {
        v->pool[i] = vsong_row_create(panel);
    }

    /* A single off-screen proxy is the group's only member; edit mode routes
     * the encoder's rotation/press to vsong_proxy_event (see the block comment
     * above). Not scrollable, so it never itself scrolls or leaves edit mode. */
    lv_obj_t *proxy = lv_obj_create(screen);
    lv_obj_remove_style_all(proxy);
    lv_obj_set_size(proxy, 1, 1);
    lv_obj_remove_flag(proxy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(proxy, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(proxy, vsong_proxy_event, LV_EVENT_KEY, v);
    lv_obj_add_event_cb(proxy, vsong_proxy_event, LV_EVENT_SHORT_CLICKED, v);
    lv_obj_add_event_cb(proxy, vsong_proxy_event, LV_EVENT_LONG_PRESSED, v);
    v->proxy = proxy;

    lv_group_t *g = lv_group_get_default();
    if (g != NULL) {
        lv_group_add_obj(g, proxy);
        lv_group_focus_obj(proxy);
        lv_group_set_editing(g, true);
    }

    vsong_relayout(v);
    rpod_cover_cache_watch(screen, vsong_covers_ready_cb, v);

    lv_obj_add_event_cb(screen, vsong_loaded_cb, LV_EVENT_SCREEN_LOADED, v);
    lv_obj_add_event_cb(screen, vsong_scrub_cb, (lv_event_code_t)rpod_scrub_event(), v);
    lv_obj_add_event_cb(screen, vsong_cleanup, LV_EVENT_DELETE, v);
}

/* Newly decoded covers: fill any row art tile still on its placeholder, and
 * the header's cover tile once all of its covers have resolved. */
static void song_list_covers_ready_cb(void *user)
{
    song_list_fetch_t *fetch = user;
    refresh_header_cover(fetch);
    if (!fetch->show_art) {
        return;
    }
    size_t base = (fetch->header_present ? 1 : 0) + fetch->lead;
    for (size_t j = 0; j < fetch->count; j++) {
        lv_obj_t *btn = lv_obj_get_child(fetch->list, (int32_t)(base + j));
        if (btn == NULL) {
            break;
        }
        if (!rpod_list_row_has_thumb(btn)) {
            const rpod_mpd_song_t *s = &fetch->songs[j];
            rpod_list_row_set_thumb(btn, rpod_cover_cache_get(s->artist, s->album, s->uri,
                                                              rpod_metrics()->list_art_size, NULL));
        }
    }
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

    /* The whole-library "Songs" list (no artist/album/playlist filter) can be
     * huge, so it gets the virtualized view above instead of one widget per
     * row. Album/playlist song lists stay on the plain path below (small, and
     * they carry the richer cover+title header). */
    if (filter->album == NULL && filter->playlist == NULL && filter->artist == NULL) {
        build_virtual_song_list(stack, screen, filter->mpd, songs, count);
        return;
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
    fetch->index = NULL;
    fetch->list = NULL;
    fetch->lead = 0;
    fetch->header_present = false;
    fetch->header_tile = NULL;
    fetch->header_cover_count = 0;
    fetch->header_filled = false;

    /* Cover art on the left of each row -- except when every row is already
     * known to share the same art, i.e. this list is a single album's songs
     * (filter->album set). Covers decode in the background, one per unique
     * album (ui/cover_cache.h): rows build with a placeholder tile and
     * song_list_covers_ready_cb() fills them in as covers land. */
    fetch->show_art = filter->album == NULL;

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

        if (fetch->show_art) {
            item->has_art_slot = true;
            item->thumb = rpod_cover_cache_get(songs[i].artist, songs[i].album, songs[i].uri,
                                               rpod_metrics()->list_art_size, NULL);
        }

        item->on_select = on_song_select;
        item->on_long_press = on_song_long_press;
        item->item_ctx = &fetch->rows[i];
    }

    /* A single album or a stored playlist gets a header (cover + title +
     * subtitle + Play/Shuffle) above the track list; an artist-scoped list
     * doesn't. (The flat Songs list took the virtualized path above.) Built
     * (and so group-registered) before the rows below it -- see
     * build_collection_header()'s comment on why order matters here. The album
     * shows one cover and its artist; the playlist a 2x2 mosaic of up to four
     * distinct album covers and its track count. */
    lv_obj_t *list = rpod_list_screen_create(screen);
    if (filter->album != NULL && count > 0) {
        const size_t cover_songs[1] = { 0 };
        build_collection_header(list, fetch, filter->album, songs[0].artist, cover_songs, 1);
    } else if (filter->playlist != NULL && count > 0) {
        size_t cover_songs[4];
        size_t n_covers = collect_distinct_cover_songs(songs, count, cover_songs, 4);
        char subtitle[32];
        snprintf(subtitle, sizeof(subtitle), "%zu %s", count, count == 1 ? "song" : "songs");
        build_collection_header(list, fetch, filter->playlist, subtitle, cover_songs, n_covers);
    }
    rpod_list_screen_populate(stack, list, ui_items, count + lead);
    free(ui_items);

    /* Locate the song rows for the on-focus indicator refresh, and do the
     * first pass (which also builds the playlist index) via the same handler. */
    fetch->list = list;
    fetch->lead = lead;
    fetch->header_present = (filter->album != NULL || filter->playlist != NULL) && count > 0;
    lv_obj_add_event_cb(screen, song_list_loaded_cb, LV_EVENT_SCREEN_LOADED, fetch);
    rpod_cover_cache_watch(screen, song_list_covers_ready_cb, fetch);
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
