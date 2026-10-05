/*
 * iOS-style HUD: a small glass capsule that drops in over the top of the
 * screen and tucks itself away again once it's left alone -- the compact
 * pill iOS shows at the top of the screen in landscape. It has two faces:
 *
 *   - Volume: an icon and a level bar, while the volume is being turned (the
 *     wheel on Now Playing, or a headset's own buttons -- see
 *     ui/volume_control.h).
 *   - Message: an icon and a short line of text, e.g. the AirPods switching
 *     to Transparency or running low (ui/airpods_notify.h).
 *
 * One per process, on LVGL's system layer above the status bar
 * (ui/status_bar.c, which sits there too), so it shows over whatever screen
 * is up.
 */

#ifndef RPOD_HUD_H
#define RPOD_HUD_H

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    RPOD_HUD_ICON_SPEAKER, /* the volume face only: muted / low / high by level */
    RPOD_HUD_ICON_AIRPODS,
    RPOD_HUD_ICON_BLUETOOTH,
} rpod_hud_icon_t;

/* Creates the HUD, hidden. Must come after rpod_status_bar_create() so it
 * draws above the bar. Call once. */
void rpod_hud_init(void);

/* Shows the volume face at `percent` (0-100; < 0 means there's no volume
 * control to show, drawn as a dimmed, empty bar), dropping the HUD in if it
 * isn't already up, and restarts its auto-hide countdown. */
void rpod_hud_volume(int percent, rpod_hud_icon_t icon);

/* Rubber-band stretch toward `dir` (> 0 right, < 0 left): feedback for
 * turning past either end of the range. Call after rpod_hud_volume(). */
void rpod_hud_volume_bump(int dir);

/* True from rpod_hud_volume() until the volume face starts tucking away (or
 * gives way to a message). */
bool rpod_hud_volume_shown(void);

/* Shows `text` (copied) beside `icon` for a couple of seconds. While the
 * volume is being turned it waits for the volume face to finish rather than
 * cutting in; a newer message replaces one that's waiting. */
void rpod_hud_message(rpod_hud_icon_t icon, const char *text);

#endif /* RPOD_HUD_H */
