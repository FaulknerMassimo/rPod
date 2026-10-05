/*
 * A list screen that rebuilds itself from live state whenever that state
 * changes -- BlueZ's devices (bluetooth_screens.c), the AirPods' battery and
 * settings (airpods_screens.c). A rebuild throws the old rows away, so each
 * row carries a key -- a device path, or a fixed tag like "power" -- and the
 * wheel's highlight goes back to the row with the same key afterwards, or to
 * whatever took its place.
 *
 * Only the visible screen rebuilds straight away; one underneath catches up
 * when it's shown again.
 */

#ifndef RPOD_LIVE_LIST_H
#define RPOD_LIVE_LIST_H

#include "list_screen.h"
#include "screen_stack.h"

typedef struct rpod_live_list rpod_live_list_t;

/* What a row's on_select gets as item_ctx. Gone once the screen rebuilds --
 * which an action that changes state may well trigger, so read it first. */
typedef struct {
    rpod_live_list_t *ll;
    char key[64];
} rpod_live_row_t;

/* Builds the screen's whole content from current state, with the
 * rpod_live_list_*() calls below. */
typedef void (*rpod_live_fill_fn)(rpod_live_list_t *ll);

/* Takes over `screen` (call it from an rpod_screen_build_fn) and fills it.
 * `ctx` is the screen's own state, for its fill function and row actions;
 * ctx_free, if not NULL, gets it when the screen is deleted. Hook the
 * state's watcher up to rpod_live_list_changed(), with
 * rpod_live_list_screen() as its owner. */
rpod_live_list_t *rpod_live_list_create(rpod_screen_stack_t *stack, lv_obj_t *screen,
                                        rpod_live_fill_fn fill, void *ctx,
                                        void (*ctx_free)(void *ctx));

/* Watcher callback, `ll` being the rpod_live_list_t: the state changed. */
void rpod_live_list_changed(void *ll);

void *rpod_live_list_ctx(const rpod_live_list_t *ll);
lv_obj_t *rpod_live_list_screen(const rpod_live_list_t *ll);

/* --- For fill functions --------------------------------------------------- */

/* A screen with nothing to select, just `msg` under `title`. Use instead of
 * everything below. */
void rpod_live_list_message(rpod_live_list_t *ll, const char *title, const char *msg);

/* Non-selectable block: a title line and/or a dim wrapped note (either may
 * be NULL). */
void rpod_live_list_header(rpod_live_list_t *ll, const char *title, const char *note);

/* Small dim caption over the rows that follow it -- or, for NULL, just a
 * gap to set them apart. */
void rpod_live_list_section(rpod_live_list_t *ll, const char *caption);

/* Puts the highlight on the first row instead of back on the row it was
 * on, and keeps it there through later rebuilds until the wheel moves it --
 * for when the screen's content changes character (a device finished
 * connecting) and its new rows may still be arriving. */
void rpod_live_list_reset_focus(rpod_live_list_t *ll);

/* Adds a row keyed `key`; fill in the returned item's text and the rest
 * before the next call. NULL if out of memory. */
rpod_list_item_t *rpod_live_list_add(rpod_live_list_t *ll, const char *key,
                                     void (*on_select)(rpod_screen_stack_t *stack, void *item_ctx));

#endif /* RPOD_LIVE_LIST_H */
