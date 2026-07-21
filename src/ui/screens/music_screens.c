#include "music_screens.h"

#include "list_screen.h"
#include "now_playing.h"
#include "audio/mpd_client.h"

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
static void build_name_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, const char *title,
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
    rpod_list_screen_build(stack, screen, title, ui_items, count);
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
    build_name_list_screen(stack, screen, filter->artist != NULL ? filter->artist : "Albums",
                            filter->mpd, items, count, filter->artist, on_album_select);
}

static void build_artist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_artists(filter->mpd, &items, &count);
    build_name_list_screen(stack, screen, "Artists", filter->mpd, items, count, NULL, on_artist_select);
}

static void build_genre_artist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_artists_in_genre(filter->mpd, filter->genre, &items, &count);
    /* Same next screen as the plain Artists list: pick an artist, browse
     * their albums -- the genre filter has done its job by here. */
    build_name_list_screen(stack, screen, filter->genre, filter->mpd, items, count, NULL, on_artist_select);
}

static void build_genre_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_genres(filter->mpd, &items, &count);
    build_name_list_screen(stack, screen, "Genres", filter->mpd, items, count, NULL, on_genre_select);
}

static void build_playlist_list_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    music_ctx_t *filter = ctx;
    rpod_mpd_item_t *items = NULL;
    size_t count = 0;
    rpod_mpd_list_playlists(filter->mpd, &items, &count);
    build_name_list_screen(stack, screen, "Playlists", filter->mpd, items, count, NULL, on_playlist_select);
}

/* A clicked song row's context: which connection and which fetched song
 * (pointing into the fetch's owned `songs` array). */
typedef struct {
    rpod_mpd_t *mpd;
    const rpod_mpd_song_t *song;
} song_row_t;

typedef struct {
    rpod_mpd_song_t *songs;
    song_row_t *rows;
} song_list_fetch_t;

static void song_list_cleanup_cb(lv_event_t *e)
{
    song_list_fetch_t *fetch = lv_event_get_user_data(e);
    rpod_mpd_free_songs(fetch->songs);
    free(fetch->rows);
    free(fetch);
}

static void on_song_select(rpod_screen_stack_t *stack, void *item_ctx)
{
    song_row_t *row = item_ctx;
    rpod_mpd_play_uri(row->mpd, row->song->uri);
    rpod_screen_stack_push(stack, rpod_now_playing_build, row->mpd, NULL);
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
    lv_obj_add_event_cb(screen, song_list_cleanup_cb, LV_EVENT_DELETE, fetch);

    rpod_list_item_t *ui_items = count > 0 ? calloc(count, sizeof(*ui_items)) : NULL;
    for (size_t i = 0; i < count; i++) {
        const char *label = songs[i].title[0] != '\0' ? songs[i].title : songs[i].uri;
        snprintf(ui_items[i].text, sizeof(ui_items[i].text), "%.*s", (int)sizeof(ui_items[i].text) - 1, label);

        if (songs[i].artist[0] != '\0' && songs[i].album[0] != '\0') {
            snprintf(ui_items[i].subtitle, sizeof(ui_items[i].subtitle), "%s - %s", songs[i].artist,
                     songs[i].album);
        } else if (songs[i].artist[0] != '\0') {
            snprintf(ui_items[i].subtitle, sizeof(ui_items[i].subtitle), "%s", songs[i].artist);
        } else if (songs[i].album[0] != '\0') {
            snprintf(ui_items[i].subtitle, sizeof(ui_items[i].subtitle), "%s", songs[i].album);
        }

        if (songs[i].duration_s > 0) {
            snprintf(ui_items[i].accessory, sizeof(ui_items[i].accessory), "%u:%02u",
                     songs[i].duration_s / 60u, songs[i].duration_s % 60u);
        }

        ui_items[i].on_select = on_song_select;
        ui_items[i].item_ctx = &fetch->rows[i];
    }

    const char *title = "Songs";
    if (filter->playlist != NULL) {
        title = filter->playlist;
    } else if (filter->album != NULL) {
        title = filter->album;
    } else if (filter->artist != NULL) {
        title = filter->artist;
    }
    rpod_list_screen_build(stack, screen, title, ui_items, count);
    free(ui_items);
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

void rpod_music_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    rpod_mpd_t *mpd = ctx;

    rpod_list_item_t items[] = {
        { .text = "Playlists", .chevron = true, .on_select = on_music_menu_playlists, .item_ctx = mpd },
        { .text = "Artists",   .chevron = true, .on_select = on_music_menu_artists,   .item_ctx = mpd },
        { .text = "Albums",    .chevron = true, .on_select = on_music_menu_albums,    .item_ctx = mpd },
        { .text = "Songs",     .chevron = true, .on_select = on_music_menu_songs,     .item_ctx = mpd },
        { .text = "Genres",    .chevron = true, .on_select = on_music_menu_genres,    .item_ctx = mpd },
    };
    rpod_list_screen_build(stack, screen, "Music", items, sizeof(items) / sizeof(items[0]));
}
