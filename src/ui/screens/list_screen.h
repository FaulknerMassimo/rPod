/*
 * Generic vertical list screen: a full-height lv_list docked below the
 * persistent status bar (ui/status_bar.h), used directly for Main Menu /
 * Music submenu / Settings menu / Extras, and reused by the MPD-backed
 * browse screens for whatever rows they fetch.
 */

#ifndef RPOD_LIST_SCREEN_H
#define RPOD_LIST_SCREEN_H

#include "screen_stack.h"
#include <stdbool.h>
#include <stddef.h>

/* Square size (px) a row's art column renders at -- callers populating
 * `thumb` below should decode/scale to this directly rather than relying on
 * lv_image to rescale a differently-sized source. */
#define RPOD_LIST_ART_SIZE 40

typedef struct {
    char text[256];      /* primary label */
    char subtitle[256];  /* optional dim second line below text; "" = none */
    char accessory[32];  /* optional dim right-aligned text (e.g. a duration); "" = none */
    bool chevron;        /* show an iOS-style ">" disclosure indicator on the right */
    /* Optional cover-art column on the left of the row. `has_art_slot` says
     * whether this row reserves the column at all (rows within one list
     * should agree); `thumb` is the decoded RGB565 thumbnail to show there,
     * or NULL to show a placeholder tile instead (e.g. an untagged track) --
     * NULL only makes sense to a reader when `has_art_slot` is true. Must
     * outlive the built list -- copied by pointer, not by value. */
    bool has_art_slot;
    const lv_image_dsc_t *thumb;
    void (*on_select)(rpod_screen_stack_t *stack, void *item_ctx);
    void *item_ctx;
} rpod_list_item_t;

/* Populates `screen` (already created by the caller, typically as the
 * build_fn passed to rpod_screen_stack_push) with a list of `items`
 * (`count` entries, copied internally -- the array passed in doesn't need
 * to outlive this call). An empty list renders a single disabled "(empty)"
 * row rather than a blank screen. */
void rpod_list_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen,
                             const rpod_list_item_t *items, size_t count);

#endif /* RPOD_LIST_SCREEN_H */
