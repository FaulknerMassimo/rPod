/*
 * The Wii theme's look (ui/theme.h): the Rockbox "Wii_menu" theme by Monica
 * G. (CC BY-SA 3.0) laid out for rPod's 320x240 panel -- the Wii Menu's
 * white and teal, a wave of a status bar along the bottom, the channel tiles
 * for a main menu, its panels for the playback state, and Now Playing's
 * backdrop with a Mii where the cover would be.
 *
 * The art is that theme's, converted by re/wii/tools/extract.py (`make
 * wii-assets`) and never committed -- the licence (CC BY-SA 3.0) isn't
 * rPod's (CC BY-NC 4.0), and the pictures aren't ours either. The Makefile
 * builds it in when it finds it (RPOD_WII_ASSETS); without it the Wii theme
 * is only its palette on Glass's layout, and nothing here is called:
 * screens check rpod_wii_layout() first.
 *
 * Geometry is in the theme's own coordinates (the 320x240 panel): every
 * screen but Now Playing has the bottom RPOD_WII_BAR_H pixels taken by the
 * bar (rpod_metrics()'s footer_h), and Now Playing the whole panel.
 */

#ifndef RPOD_WII_H
#define RPOD_WII_H

#include "lvgl.h"
#include "wii_frames.h"
#include "audio/mpd_client.h"
#include "power/battery.h"

#include <stdbool.h>
#include <time.h>

/* True when the Wii art is built in. */
bool rpod_wii_has_assets(void);

/* The Wii theme is active and has its art: the layout below is in use. */
bool rpod_wii_layout(void);

#define RPOD_WII_BAR_H 70

/* The list's card, left of the playback panels (rpod_wii_column_create()). */
#define RPOD_WII_LIST_X 8
#define RPOD_WII_LIST_Y 4
#define RPOD_WII_LIST_W 218
#define RPOD_WII_LIST_H 164

/* The tile menu: 3x3 channel tiles of 71x55. */
#define RPOD_WII_TILE_W    71
#define RPOD_WII_TILE_H    55
#define RPOD_WII_TILE_COLS 3
#define RPOD_WII_TILE_X    8
#define RPOD_WII_TILE_Y    3

/* --- The bar ---------------------------------------------------------------- */

/* Whether the bar is away (status_bar.c's rpod_status_bar_set_hidden()):
 * full-screen screens lay out against the whole panel then, so
 * rpod_metrics() reports no footer. */
void rpod_wii_set_bar_hidden(bool hidden);
bool rpod_wii_bar_hidden(void);

/* Creates the wave along the bottom on `layer` (the status bar's system
 * layer): the clock in its dip, the date, the screen's title below it and the
 * hold icon on the right-hand button. One at a time. */
lv_obj_t *rpod_wii_bar_create(lv_obj_t *layer);
void rpod_wii_bar_set_time(const struct tm *tm);
void rpod_wii_bar_set_title(const char *title);
void rpod_wii_bar_set_locked(bool locked);

/* --- The playback panels ------------------------------------------------------- */

/* The three panels at the right of a list: the battery, the play state, and
 * repeat and shuffle. They follow rpod_wii_set_player() and the battery for
 * as long as the object lives. */
lv_obj_t *rpod_wii_column_create(lv_obj_t *screen);

/* The player's state, from the status bar's poll (NULL: no MPD): kept for the
 * panels, which are redrawn. */
void rpod_wii_set_player(const rpod_mpd_status_t *status);

/* The battery changed (status_bar.c's watcher): redraws the panels. */
void rpod_wii_battery_changed(void);

/* --- Tiles --------------------------------------------------------------------- */

/* In the order of the art's 3x3 sheet. */
typedef enum {
    RPOD_WII_TILE_FILES,     /* a CD */
    RPOD_WII_TILE_DATABASE,  /* a disc drive */
    RPOD_WII_TILE_NOWPLAYING,
    RPOD_WII_TILE_SETTINGS,
    RPOD_WII_TILE_RECORDING, /* a camera */
    RPOD_WII_TILE_PLAYLISTS,
    RPOD_WII_TILE_PLUGINS,   /* the Wii shopping bag */
    RPOD_WII_TILE_SYSTEM,
    RPOD_WII_TILE_SHORTCUTS,
    RPOD_WII_TILE_COUNT,
} rpod_wii_tile_t;

/* A tile, as a clickable object of RPOD_WII_TILE_W x RPOD_WII_TILE_H that
 * the caller wires to its group and events. */
lv_obj_t *rpod_wii_tile_create(lv_obj_t *parent, rpod_wii_tile_t tile);
/* Selected tiles have the teal frame. */
void rpod_wii_tile_set_selected(lv_obj_t *tile, bool selected);

/* --- Now Playing --------------------------------------------------------------- */

/* Now Playing's full-panel backdrop: the top row of six tiles, the card for
 * the track's details and the bar for the progress. */
lv_obj_t *rpod_wii_np_bg_create(lv_obj_t *parent);

/* A Mii, for a track without a cover: 0 or 1. 133x140. */
lv_obj_t *rpod_wii_mii_create(lv_obj_t *parent, int which);

typedef enum {
    RPOD_WII_SERIES_VOLUME,   /* 21 rings, mute to full: 36x36 */
    RPOD_WII_SERIES_PLAYMODE, /* stop, play, pause...: 35x35 */
    RPOD_WII_SERIES_REPEAT,   /* off, all, one: 33x33 */
    RPOD_WII_SERIES_SHUFFLE,  /* off, on: 33x33 */
    RPOD_WII_SERIES_BATTERY,  /* 25 frames of 42x16 */
    RPOD_WII_SERIES_LOCK,     /* 12x16 */
} rpod_wii_series_t;

/* An image showing frame `frame` of `series` (clamped), and the same
 * change in place. */
lv_obj_t *rpod_wii_frame_create(lv_obj_t *parent, rpod_wii_series_t series, int frame);
void rpod_wii_frame_set(lv_obj_t *img, rpod_wii_series_t series, int frame);

/* The battery's frame (wii_frames.h has the rest): -1 without one. */
int rpod_wii_battery_frame(const rpod_battery_t *battery);

#endif /* RPOD_WII_H */
