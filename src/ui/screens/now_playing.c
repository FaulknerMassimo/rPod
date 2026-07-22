#include "now_playing.h"

#include "audio/mpd_client.h"
#include "audio/visualizer.h"
#include "ui/cover_art.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Square cover art tile, center-cropped ("aspect fill") from whatever the
 * source image's aspect ratio actually is -- see cover_art.c. */
#define ART_SIZE 136

/* The blurred backdrop is decoded tiny (screen res / BG_SCALE) and then
 * upscaled to fill the screen -- the downsample itself does most of the
 * "melted" softening a real backdrop blur would, on top of which
 * rpod_cover_art_decode_background()'s own box blur smooths out the
 * remaining block edges. Blurring at full 320x240 res directly (tried
 * first) barely read as blurred at all: a small-radius box blur is too
 * subtle against fine detail at that resolution to look like more than a
 * slightly muddy version of the sharp art. */
#define BG_SCALE  8
#define BG_SRC_W  (RPOD_SCREEN_WIDTH / BG_SCALE)
#define BG_SRC_H  (RPOD_SCREEN_HEIGHT / BG_SCALE)

/* Small equalizer-style visualizer living in the scrubber panel's own
 * headroom, above the progress bar -- Apple Music style, but actually
 * audio-reactive (src/audio/visualizer.c reads real PCM off an MPD fifo
 * output, not a canned animation). */
#define VIS_H         14
#define VIS_BAR_W     3
#define VIS_BAR_GAP   3
#define VIS_MIN_H     2 /* still show a sliver of each bar at zero level */
#define VIS_PERIOD_MS 33 /* ~30Hz -- plenty for a handful of small bars */

typedef struct {
    rpod_mpd_t *mpd;

    lv_obj_t *bg_img;       /* full-bleed blurred/darkened backdrop, iOS lock-screen style */
    lv_image_dsc_t bg_dsc;  /* backing store for bg_img's LV_IMAGE_SRC_VARIABLE */
    bool have_bg;

    lv_obj_t *art_container;
    lv_obj_t *art_img;
    lv_obj_t *art_placeholder; /* LV_SYMBOL_AUDIO tile shown when there's no art */
    lv_image_dsc_t art_dsc;    /* backing store for art_img's LV_IMAGE_SRC_VARIABLE */

    lv_obj_t *title_label;
    lv_obj_t *artist_label;
    lv_obj_t *album_label;

    rpod_visualizer_t *vis;
    lv_obj_t *vis_container;
    lv_obj_t *vis_bars[RPOD_VIS_BANDS];
    lv_timer_t *vis_timer;

    lv_obj_t *scrubber_panel;
    lv_obj_t *bar;
    lv_obj_t *thumb;
    lv_obj_t *elapsed_label;
    lv_obj_t *remaining_label;

    char last_uri[512]; /* which song the current art/placeholder reflects */
    bool have_art;

    lv_timer_t *timer;
} now_playing_state_t;

/* Fetches + decodes cover art for `uri` (skipped entirely if it's the same
 * song the screen already reflects -- both the MPD round trip and the JPEG
 * decode are too costly to repeat every 1s refresh tick) and updates the
 * art tile in place, falling back to a placeholder tile when there's no
 * art (untagged file, unsupported MPD version, non-JPEG folder art, etc). */
/* Fills an lv_image_dsc_t backed by an rpod_cover_art_t's RGB565 pixels
 * (both the sharp foreground tile and the blurred background reuse this --
 * same header shape, different source buffer). */
static void set_image_desc(lv_image_dsc_t *dsc, const rpod_cover_art_t *art)
{
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0;
    dsc->header.w = (uint16_t)art->w;
    dsc->header.h = (uint16_t)art->h;
    dsc->header.stride = (uint16_t)(art->w * 2);
    dsc->data_size = (uint32_t)(art->w * art->h * 2);
    dsc->data = (const uint8_t *)art->pixels;
}

static void update_art(now_playing_state_t *np, const char *uri)
{
    if (strcmp(np->last_uri, uri) == 0) {
        return;
    }
    snprintf(np->last_uri, sizeof(np->last_uri), "%s", uri);

    if (np->have_art) {
        free((void *)np->art_dsc.data);
        np->art_dsc.data = NULL;
        np->have_art = false;
    }
    if (np->have_bg) {
        free((void *)np->bg_dsc.data);
        np->bg_dsc.data = NULL;
        np->have_bg = false;
    }

    unsigned char *raw = NULL;
    size_t raw_size = 0;
    bool fetched = rpod_mpd_get_cover_art(np->mpd, uri, &raw, &raw_size);

    rpod_cover_art_t art = { 0 };
    bool decoded = fetched && rpod_cover_art_decode(raw, raw_size, ART_SIZE, ART_SIZE, &art);
    if (decoded) {
        set_image_desc(&np->art_dsc, &art);
        np->have_art = true;

        lv_image_set_src(np->art_img, &np->art_dsc);
        lv_obj_remove_flag(np->art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(np->art_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(np->art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(np->art_placeholder, LV_OBJ_FLAG_HIDDEN);
    }

    rpod_cover_art_t bg = { 0 };
    bool bg_decoded = fetched && rpod_cover_art_decode_background(raw, raw_size, BG_SRC_W, BG_SRC_H, &bg);
    if (bg_decoded) {
        set_image_desc(&np->bg_dsc, &bg);
        np->have_bg = true;
        lv_image_set_src(np->bg_img, &np->bg_dsc);
        lv_obj_remove_flag(np->bg_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(np->bg_img, LV_OBJ_FLAG_HIDDEN);
    }

    rpod_mpd_free_cover_art(raw);
}

static void refresh_cb(lv_timer_t *timer)
{
    now_playing_state_t *np = lv_timer_get_user_data(timer);

    rpod_mpd_status_t status;
    /* _settled() so this screen's own 1s timer never catches (and paints) the
     * momentary end-of-queue stop before the status bar's poll re-cues -- the
     * "split second of unknown title" otherwise seen when a collection ends
     * while Now Playing is up. Whichever poll fires first on the stop re-cues
     * to the first song paused and reads it back in the same tick. */
    if (!rpod_mpd_get_status_settled(np->mpd, &status)) {
        lv_label_set_text(np->title_label, "(disconnected)");
        lv_label_set_text(np->artist_label, "");
        lv_label_set_text(np->album_label, "");
        return;
    }

    lv_label_set_text(np->title_label, status.title[0] != '\0' ? status.title : "(unknown title)");
    lv_label_set_text(np->artist_label, status.artist[0] != '\0' ? status.artist : "Unknown artist");
    lv_label_set_text(np->album_label, status.album[0] != '\0' ? status.album : "Unknown album");

    int pct = status.duration_s > 0 ? (int)((status.elapsed_s * 100u) / status.duration_s) : 0;
    lv_bar_set_value(np->bar, pct, LV_ANIM_OFF);
    lv_obj_align_to(np->thumb, np->bar, LV_ALIGN_LEFT_MID, (lv_obj_get_width(np->bar) * pct) / 100 - 5, 0);

    char elapsed[32];
    snprintf(elapsed, sizeof(elapsed), "%u:%02u", status.elapsed_s / 60u, status.elapsed_s % 60u);
    lv_label_set_text(np->elapsed_label, elapsed);

    unsigned remaining = status.duration_s > status.elapsed_s ? status.duration_s - status.elapsed_s : 0;
    char remaining_str[32];
    snprintf(remaining_str, sizeof(remaining_str), "-%u:%02u", remaining / 60u, remaining % 60u);
    lv_label_set_text(np->remaining_label, remaining_str);

    if (status.uri[0] != '\0') {
        update_art(np, status.uri);
    }
}

static void vis_timer_cb(lv_timer_t *timer)
{
    now_playing_state_t *np = lv_timer_get_user_data(timer);

    float levels[RPOD_VIS_BANDS];
    rpod_visualizer_get_levels(np->vis, levels);
    for (int i = 0; i < RPOD_VIS_BANDS; i++) {
        int32_t h = VIS_MIN_H + (int32_t)(levels[i] * (VIS_H - VIS_MIN_H));
        /* No flex here -- set height *and* y explicitly every tick so the
         * bottom edge is pinned at a fixed pixel regardless of height,
         * rather than trusting a layout engine to hold it there. */
        lv_obj_set_height(np->vis_bars[i], h);
        lv_obj_set_y(np->vis_bars[i], VIS_H - h);
    }
}

static void screen_delete_cb(lv_event_t *e)
{
    now_playing_state_t *np = lv_event_get_user_data(e);
    lv_timer_delete(np->timer);
    lv_timer_delete(np->vis_timer);
    /* np->vis is the status bar's shared handle (see rpod_now_playing_build
     * below), not one this screen started -- must not stop it here, or the
     * status bar's own mini-visualizer dies with the first Now Playing
     * screen visit. */
    if (np->have_art) {
        free((void *)np->art_dsc.data);
    }
    if (np->have_bg) {
        free((void *)np->bg_dsc.data);
    }
    free(np);
}

void rpod_now_playing_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;

    now_playing_state_t *np = calloc(1, sizeof(*np));
    np->mpd = ctx;

    /* Reuse the status bar's single FIFO reader rather than starting a
     * second one -- see ui/status_bar.h's warning about two readers
     * splitting the same byte stream. */
    np->vis = rpod_status_bar_shared_visualizer();

    /* --- Backdrop: a blurred, darkened crop of the current cover art,
     * full-bleed behind the whole screen -- iOS lock-screen style. Created
     * first (and so drawn first / bottom of the z-order) so every other
     * widget floats on top of it as glass. Hidden until update_art() below
     * has something to show; falls back to the screen's plain background
     * colour otherwise. --- */
    np->bg_img = lv_image_create(screen);
    /* Native (unscaled) size matches the tiny decoded backdrop -- scale
     * transforms are applied around the object's own box, so centering
     * this small box on the screen and zooming by exactly BG_SCALE fills
     * the screen precisely. */
    lv_obj_set_size(np->bg_img, BG_SRC_W, BG_SRC_H);
    lv_obj_center(np->bg_img);
    lv_image_set_scale(np->bg_img, BG_SCALE * 256);
    lv_obj_add_flag(np->bg_img, LV_OBJ_FLAG_HIDDEN);

    /* --- Cover art tile, top-left: rounded square, clipped so the image
     * (or the placeholder glyph) can't peek past the corners. --- */
    np->art_container = lv_obj_create(screen);
    lv_obj_remove_style_all(np->art_container);
    lv_obj_set_size(np->art_container, ART_SIZE, ART_SIZE);
    lv_obj_align(np->art_container, LV_ALIGN_TOP_LEFT, 14, RPOD_HEADER_HEIGHT + 10);
    rpod_theme_style_glass_panel(np->art_container, 14);
    lv_obj_set_style_clip_corner(np->art_container, true, 0);
    lv_obj_clear_flag(np->art_container, LV_OBJ_FLAG_SCROLLABLE);

    np->art_img = lv_image_create(np->art_container);
    lv_obj_set_size(np->art_img, ART_SIZE, ART_SIZE);
    lv_obj_center(np->art_img);
    lv_obj_add_flag(np->art_img, LV_OBJ_FLAG_HIDDEN);

    np->art_placeholder = lv_label_create(np->art_container);
    lv_label_set_text(np->art_placeholder, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(np->art_placeholder, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(np->art_placeholder, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_center(np->art_placeholder);

    /* --- Track info, to the right of the art: bold title over separate
     * dimmer artist / album lines, iOS Now-Playing style. --- */
    int info_x = 14 + ART_SIZE + 14;
    int info_w = RPOD_SCREEN_WIDTH - info_x - 14;

    np->title_label = lv_label_create(screen);
    lv_obj_set_width(np->title_label, info_w);
    lv_label_set_long_mode(np->title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(np->title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(np->title_label, LV_ALIGN_TOP_LEFT, info_x, RPOD_HEADER_HEIGHT + 12);

    np->artist_label = lv_label_create(screen);
    lv_obj_set_width(np->artist_label, info_w);
    lv_label_set_long_mode(np->artist_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(np->artist_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->artist_label, LV_ALIGN_TOP_LEFT, info_x, RPOD_HEADER_HEIGHT + 44);

    np->album_label = lv_label_create(screen);
    lv_obj_set_width(np->album_label, info_w);
    lv_label_set_long_mode(np->album_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(np->album_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->album_label, LV_ALIGN_TOP_LEFT, info_x, RPOD_HEADER_HEIGHT + 66);

    /* --- Scrubber: thin pill-shaped track with a small round thumb, plus
     * elapsed / time-remaining labels either end -- iOS shows the right
     * side as time *remaining* (a negative count-down), not the total.
     * No transport controls here -- the click wheel's Menu/Play/Next/Prev
     * keys already drive playback globally regardless of which screen is
     * on top, so this screen is display-only. Floats over its own glass
     * panel so it reads as a control strip rather than bare text/bar
     * sitting directly on the blurred backdrop. --- */
    np->scrubber_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(np->scrubber_panel);
    lv_obj_set_size(np->scrubber_panel, RPOD_SCREEN_WIDTH - 16, 48);
    lv_obj_align(np->scrubber_panel, LV_ALIGN_BOTTOM_MID, 0, -6);
    rpod_theme_style_glass_panel(np->scrubber_panel, 16);
    lv_obj_clear_flag(np->scrubber_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Visualizer: a small row of bars to the left of the progress bar,
     * on the same row -- the bar itself is narrowed to leave room instead
     * of the two overlapping. Both are children of `screen` (not the
     * panel) so they share one coordinate system and are trivial to keep
     * vertically centered on the same line. Each visualizer bar's x is
     * fixed at creation and only height/y touched per tick: flex's
     * cross-axis alignment (tried first) ended up visibly reflowing the
     * whole row on every height change instead of holding a fixed
     * baseline, so this pins the bottom edge by hand instead. */
    int vis_total_w = RPOD_VIS_BANDS * VIS_BAR_W + (RPOD_VIS_BANDS - 1) * VIS_BAR_GAP;
    int bar_inset = 14;   /* left/right margin the seek bar always had */
    int vis_to_bar_gap = 8;
    int bar_x = bar_inset + vis_total_w + vis_to_bar_gap;
    int bar_w = (RPOD_SCREEN_WIDTH - bar_inset) - bar_x;

    np->vis_container = lv_obj_create(screen);
    lv_obj_remove_style_all(np->vis_container);
    lv_obj_set_size(np->vis_container, vis_total_w, VIS_H);
    /* Shares a centerline with the 4px seek bar at y=-30 below (both sit
     * on the same row), and together with the -10 on the time labels
     * further down, positions the whole vis+bar+labels group inside the
     * 48px scrubber panel -- shifted down from dead-center per feedback
     * that a mathematically-centered version still read as too high. */
    lv_obj_align(np->vis_container, LV_ALIGN_BOTTOM_LEFT, bar_inset, -25);
    lv_obj_clear_flag(np->vis_container, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < RPOD_VIS_BANDS; i++) {
        lv_obj_t *b = lv_obj_create(np->vis_container);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, VIS_BAR_W, 2);
        lv_obj_set_pos(b, i * (VIS_BAR_W + VIS_BAR_GAP), VIS_H - 2);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_color(b, RPOD_COLOR_TEXT, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        np->vis_bars[i] = b;
    }

    np->vis_timer = lv_timer_create(vis_timer_cb, VIS_PERIOD_MS, np);

    np->bar = lv_bar_create(screen);
    lv_obj_remove_style_all(np->bar);
    lv_obj_set_size(np->bar, bar_w, 4);
    lv_obj_set_style_radius(np->bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(np->bar, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(np->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(np->bar, RPOD_COLOR_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(np->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(np->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(np->bar, 0, 100);
    lv_obj_align(np->bar, LV_ALIGN_BOTTOM_LEFT, bar_x, -30);

    np->thumb = lv_obj_create(screen);
    lv_obj_remove_style_all(np->thumb);
    lv_obj_set_size(np->thumb, 10, 10);
    lv_obj_set_style_radius(np->thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(np->thumb, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(np->thumb, LV_OPA_COVER, 0);
    lv_obj_align_to(np->thumb, np->bar, LV_ALIGN_LEFT_MID, -5, 0);

    np->elapsed_label = lv_label_create(screen);
    lv_obj_set_style_text_color(np->elapsed_label, RPOD_COLOR_DIM_TEXT, 0);
    /* Lines up with bar_x -- the current-time reading starts exactly where
     * the seek bar itself starts, now that the visualizer bars sit to its
     * left. */
    lv_obj_align(np->elapsed_label, LV_ALIGN_BOTTOM_LEFT, bar_x, -10);

    np->remaining_label = lv_label_create(screen);
    lv_obj_set_style_text_color(np->remaining_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->remaining_label, LV_ALIGN_BOTTOM_RIGHT, -14, -10);

    np->timer = lv_timer_create(refresh_cb, 1000, np);
    refresh_cb(np->timer);

    lv_obj_add_event_cb(screen, screen_delete_cb, LV_EVENT_DELETE, np);
}
