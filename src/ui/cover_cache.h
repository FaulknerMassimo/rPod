/*
 * Process-wide cache of decoded cover-art thumbnails, filled off the UI
 * thread.
 *
 * A cover is an embedded picture fetched over MPD (readpicture/albumart)
 * and decoded to a small RGB565 tile (cover_art.h). Measured on the Pi 3B
 * dev board against real FLAC rips with 1-3 MB embedded PNG covers: ~20-40
 * ms to fetch plus ~280 ms to decode, per album. The song lists used to do
 * that inline on the LVGL thread for every album in view, freezing the
 * click wheel for seconds on each screen open and on every scroll that
 * brought a new album into view.
 *
 * Here a worker thread with its own MPD connection (libmpdclient
 * connections aren't thread-safe, and the UI's is busy polling status)
 * does the fetch + decode. The UI thread only looks finished thumbnails up
 * and gets a callback when new ones land -- so screens build instantly with
 * placeholder tiles, and art fills in as it arrives.
 */

#ifndef RPOD_COVER_CACHE_H
#define RPOD_COVER_CACHE_H

#include "lvgl.h"

#include <stdbool.h>

/* Starts the worker. Call once, after lv_init() (it creates an lv_timer) --
 * rpod_app_run() does. `mpd_socket` is the same MPD socket the UI uses. */
void rpod_cover_cache_init(const char *mpd_socket);

/* Returns the decoded `size` x `size` thumbnail for the album (artist,
 * album), or NULL if there isn't one yet. The first miss queues a
 * background fetch from `uri`, a representative track of that album (an
 * untagged track with no album is cached per-uri instead). Every later miss
 * while still queued bumps it ahead of older requests, so whatever is on
 * screen right now decodes first.
 *
 * NULL means "show a placeholder": either still pending (*pending set true,
 * if non-NULL) or resolved to no art at all (*pending false -- untagged
 * file, unsupported format). The returned descriptor and its pixels stay
 * valid for the life of the process, so it's safe to hand straight to
 * lv_image_set_src(). LVGL thread only. */
const lv_image_dsc_t *rpod_cover_cache_get(const char *artist, const char *album,
                                           const char *uri, int size, bool *pending);

/* Calls cb(user) on the LVGL thread whenever newly decoded covers are ready
 * (batched -- one call can cover several), until `owner` is deleted.
 * Typical use: `owner` is the screen, and cb re-runs rpod_cover_cache_get()
 * for whatever is still showing a placeholder. */
void rpod_cover_cache_watch(lv_obj_t *owner, void (*cb)(void *user), void *user);

#endif /* RPOD_COVER_CACHE_H */
