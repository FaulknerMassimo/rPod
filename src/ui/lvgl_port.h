/*
 * LVGL init + display driver binding for the on-device build.
 * See docs/PLAN.md §5.
 */

#ifndef RPOD_LVGL_PORT_H
#define RPOD_LVGL_PORT_H

#include "lvgl.h"

/*
 * Initialises LVGL and binds it to the fbtft framebuffer at `fb_path`
 * (typically "/dev/fb1" — see docs/PLAN.md §5.2). Falls back to the DRM
 * path once panel-mipi-dbi is validated (§5.1); that will be a second
 * lv_display_t constructor here, not a change to this signature.
 *
 * Returns the created display, or NULL on failure (framebuffer device
 * missing/unopenable).
 */
lv_display_t *rpod_lvgl_port_init(const char *fb_path);

#endif /* RPOD_LVGL_PORT_H */
