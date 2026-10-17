/*
 * The glass popup sheet shared by the overlays (rpod_screen_stack_open_overlay):
 * a dimmed scrim over the live screen, one floating "liquid glass" card with a
 * title, a dim line under it, and a list of rows. The Add to Playlist picker
 * and Play On both use it.
 */

#ifndef RPOD_SHEET_H
#define RPOD_SHEET_H

#include "lvgl.h"

#include <stdbool.h>

/* Builds the scrim and sheet into an overlay's `root`; returns the sheet's
 * (empty) lv_list for the rows. `subtitle` may be NULL. */
lv_obj_t *rpod_sheet_create(lv_obj_t *root, const char *title, const char *subtitle);

/* A plain row: `label`, an optional dim `tag` on the right, and a check
 * while `checked`. Themed like the Add to Playlist rows. Returns the button;
 * attach the select handler to its LV_EVENT_CLICKED. */
lv_obj_t *rpod_sheet_add_row(lv_obj_t *list, const char *label, const char *tag, bool checked,
                             bool is_last);

#endif /* RPOD_SHEET_H */
