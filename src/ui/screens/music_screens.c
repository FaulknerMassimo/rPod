#include "music_screens.h"

#include "list_screen.h"
#include "now_playing.h"
#include "search_screen.h"
#include "playlist_edit_screens.h"
#include "audio/mpd_client.h"
#include "ui/cover_art.h"

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

/* A clicked song row's context: which connection and which fetched song
 * (pointing into the fetch's owned `songs` array). */
typedef struct {
    rpod_mpd_t *mpd;
    const rpod_mpd_song_t *song;
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
    rpod_mpd_song_t *songs;
    song_row_t *rows;
    art_slot_t *art_slots;
    size_t art_slot_count;
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
    rpod_mpd_play_uri(row->mpd, row->song->uri);
    rpod_screen_stack_push(stack, rpod_now_playing_build, row->mpd, NULL);
}

static void on_add_songs_to_playlist(rpod_screen_stack_t *stack, void *item_ctx)
{
    music_ctx_t *filter = item_ctx;
    rpod_push_add_songs_screen(stack, filter->mpd, filter->playlist);
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
    fetch->songs = songs;
    fetch->rows = count > 0 ? malloc(count * sizeof(*fetch->rows)) : NULL;
    for (size_t i = 0; i < count; i++) {
        fetch->rows[i].mpd = filter->mpd;
        fetch->rows[i].song = &songs[i];
    }
    fetch->art_slots = NULL;
    fetch->art_slot_count = 0;

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

    rpod_list_screen_build(stack, screen, ui_items, count + lead);
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
