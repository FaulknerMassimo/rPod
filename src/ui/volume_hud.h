/*
 * iOS-style volume HUD: a small glass capsule (speaker glyph + level bar)
 * that drops in over the top of the screen while the volume is being turned
 * and tucks itself away again once it's left alone -- the compact pill iOS
 * shows at the top of the screen in landscape. Lives on LVGL's system layer,
 * above the status bar (ui/status_bar.c, which sits there too), rather than
 * on any one screen. Now Playing owns one while it's on the stack.
 */

#ifndef RPOD_VOLUME_HUD_H
#define RPOD_VOLUME_HUD_H

#include "lvgl.h"
#include <stdbool.h>

typedef struct rpod_volume_hud rpod_volume_hud_t;

/* Creates the HUD, hidden, on the default display's system layer. Must come
 * after rpod_status_bar_create() so it draws above the bar. */
rpod_volume_hud_t *rpod_volume_hud_create(void);
void rpod_volume_hud_delete(rpod_volume_hud_t *hud);

/* Shows the HUD at `percent` (0-100; < 0 means there's no volume control to
 * show, drawn as a dimmed, empty bar), dropping it in if it isn't already up,
 * and restarts its auto-hide countdown. */
void rpod_volume_hud_show(rpod_volume_hud_t *hud, int percent);

/* Rubber-band stretch toward `dir` (> 0 right, < 0 left): feedback for
 * turning past either end of the range. Call after rpod_volume_hud_show(). */
void rpod_volume_hud_bump(rpod_volume_hud_t *hud, int dir);

/* True from rpod_volume_hud_show() until the HUD starts tucking away. */
bool rpod_volume_hud_is_shown(const rpod_volume_hud_t *hud);

#endif /* RPOD_VOLUME_HUD_H */
