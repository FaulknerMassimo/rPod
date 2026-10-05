/*
 * Process-wide cache of decoded cover-art thumbnails, filled off the UI
 * thread, and backed by a cache directory on disk.
 *
 * A cover is an embedded picture fetched over MPD (readpicture/albumart)
 * and decoded to a small RGB565 tile (cover_art.h). Measured on the Pi 3B
 * dev board against real FLAC rips with 1-4 MB embedded 1400x1400 PNG
 * covers: 60-700 ms to fetch (MPD reads the whole picture off the SD card,
 * slower still while the card is busy with writes) plus ~100 ms to decode,
 * per album. The song lists used to do that inline on the LVGL thread for
 * every album in view, freezing the click wheel for seconds on each screen
 * open and on every scroll that brought a new album into view.
 *
 * So covers live in three tiers:
 *  - memory: finished thumbnails, looked up synchronously by the UI thread;
 *  - disk: one decoded "master" tile per album (37 KB), from which any
 *    smaller size is scaled in well under a millisecond -- read by a loader
 *    thread, which never waits behind a decode;
 *  - MPD: fetch + decode by worker threads, each with its own MPD
 *    connection (libmpdclient connections aren't thread-safe, and the UI's
 *    is busy polling status), writing the master back to disk.
 * When there's nothing else to do, a worker walks the whole library and
 * decodes every album that isn't on disk yet (again whenever MPD's database
 * changes), so browsing normally never meets the slow tier at all.
 *
 * The UI thread only looks finished thumbnails up and gets a callback when
 * new ones land -- screens build instantly with placeholder tiles, and art
 * fills in as it arrives (in the same frame, for art already on disk: see
 * rpod_cover_cache_settle()).
 *
 * The disk tier is keyed by album artist + album and never revalidated: re-tagging
 * an album with new art needs its cache directory cleared to show up.
 */

#ifndef RPOD_COVER_CACHE_H
#define RPOD_COVER_CACHE_H

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

/* Starts the workers. Call once, after lv_init() (it creates an lv_timer) --
 * rpod_app_run() does. `mpd_socket` is the same MPD socket the UI uses.
 * `cache_dir` is where decoded covers persist (created if missing); NULL,
 * or a directory that can't be created, keeps covers in memory only. */
void rpod_cover_cache_init(const char *mpd_socket, const char *cache_dir);

/* Returns the decoded `size` x `size` thumbnail for the album (album_artist,
 * album -- rpod_mpd_song_t's fields of those names), or NULL if there isn't
 * one yet. The first miss queues a
 * background load from `uri`, a representative track of that album (an
 * untagged track with no album is cached per-uri instead). Every later miss
 * while still queued bumps it ahead of older requests, so whatever is on
 * screen right now loads first.
 *
 * NULL means "show a placeholder": either still pending (*pending set true,
 * if non-NULL) or resolved to no art at all (*pending false -- untagged
 * file, unsupported format). The returned descriptor and its pixels stay
 * valid for the life of the process, so it's safe to hand straight to
 * lv_image_set_src(). Calling this just to warm the cache (ignoring the
 * result) is fine too. LVGL thread only. */
const lv_image_dsc_t *rpod_cover_cache_get(const char *album_artist, const char *album,
                                           const char *uri, int size, bool *pending);

/* Calls cb(user) on the LVGL thread whenever newly decoded covers are ready
 * (batched -- one call can cover several), until `owner` is deleted.
 * Typical use: `owner` is the screen, and cb re-runs rpod_cover_cache_get()
 * for whatever is still showing a placeholder. */
void rpod_cover_cache_watch(lv_obj_t *owner, void (*cb)(void *user), void *user);

/* Waits up to `max_ms` for the loader to finish looking up the covers
 * requested so far on disk, then delivers whatever landed to the watchers
 * right away. Called once a new screen is built, before its first frame
 * (rpod_screen_stack_push()), so covers already on disk appear with the
 * screen instead of a frame or two after it. Covers that need decoding
 * still arrive later, as usual. LVGL thread only. */
void rpod_cover_cache_settle(uint32_t max_ms);

#endif /* RPOD_COVER_CACHE_H */
