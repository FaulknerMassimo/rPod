/*
 * Generic vertical list screen: a title header + a full-height lv_list,
 * used directly for Main Menu / Music submenu / Settings menu / Extras, and
 * reused by the MPD-backed browse screens for whatever rows they fetch.
 */

#ifndef RPOD_LIST_SCREEN_H
#define RPOD_LIST_SCREEN_H

#include "screen_stack.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char text[256];      /* primary label */
    char subtitle[256];  /* optional dim second line below text; "" = none */
    char accessory[32];  /* optional dim right-aligned text (e.g. a duration); "" = none */
    bool chevron;        /* show an iOS-style ">" disclosure indicator on the right */
    void (*on_select)(rpod_screen_stack_t *stack, void *item_ctx);
    void *item_ctx;
} rpod_list_item_t;

/* Populates `screen` (already created by the caller, typically as the
 * build_fn passed to rpod_screen_stack_push) with a header showing `title`
 * and a list of `items` (`count` entries, copied internally -- the array
 * passed in doesn't need to outlive this call). An empty list renders a
 * single disabled "(empty)" row rather than a blank screen. */
void rpod_list_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, const char *title,
                             const rpod_list_item_t *items, size_t count);

#endif /* RPOD_LIST_SCREEN_H */
