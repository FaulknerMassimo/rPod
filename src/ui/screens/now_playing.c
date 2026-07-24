#include "now_playing.h"

#include "audio/mpd_client.h"
#include "audio/visualizer.h"
#include "ui/cover_art.h"
#include "ui/heart_icon.h"
#include "ui/metrics.h"
#include "ui/playlist_membership.h"
#include "ui/status_bar.h"
#include "ui/theme.h"
#include "playlist_picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Square cover art tile, center-cropped ("aspect fill") from whatever the
 * source image's aspect ratio actually is -- see cover_art.c. Form dependent:
 * a big hero tile on the landscape panel, a small one on the 128x128 square. */
static int np_art_size(void)
{
    return rpod_metrics()->form == RPOD_FORM_SQUARE ? 40 : 136;
}

/* The blurred backdrop is decoded tiny (screen res / BG_SCALE) and then
 * upscaled to fill the screen -- the downsample itself does most of the
 * "melted" softening a real backdrop blur would, on top of which
 * rpod_cover_art_decode_background()'s own box blur smooths out the
 * remaining block edges. Blurring at full 320x240 res directly (tried
 * first) barely read as blurred at all: a small-radius box blur is too
 * subtle against fine detail at that resolution to look like more than a
 * slightly muddy version of the sharp art. */
#define BG_SCALE  8
#define BG_SRC_W  (rpod_metrics()->screen_w / BG_SCALE)
#define BG_SRC_H  (rpod_metrics()->screen_h / BG_SCALE)

/* Small equalizer-style visualizer living in the scrubber panel's own
 * headroom, above the progress bar -- Apple Music style, but actually
 * audio-reactive (src/audio/visualizer.c reads real PCM off an MPD fifo
 * output, not a canned animation). */
#define VIS_H         14
#define VIS_BAR_W     3
#define VIS_BAR_GAP   3
#define VIS_MIN_H     2 /* still show a sliver of each bar at zero level */
#define VIS_PERIOD_MS 33 /* ~30Hz -- plenty for a handful of small bars */

/* The top-right "loved" heart, and how close two centre presses must be to
 * count as a double-press (a like) rather than two singles. */
#define NP_HEART_SIZE      28
#define NP_DOUBLE_PRESS_MS 400

typedef struct {
    rpod_mpd_t *mpd;
    rpod_screen_stack_t *stack;

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

    /* "Loved" control, top-right. Double-press centre toggles it (with a pop),
     * press-and-hold opens the Add-to-Playlist picker. `proxy` is an offscreen
     * focusable object that receives the centre-button events (Now Playing has
     * no other focusable widget). */
    lv_obj_t *heart;
    lv_obj_t *proxy;
    char cur_uri[512];    /* current track, for like/picker actions */
    char cur_title[256];
    char liked_uri[512];  /* track whose `liked` state the heart reflects */
    bool liked;
    bool longpress_pending;
    bool have_prev_click;      /* a first centre press is waiting for a second */
    uint32_t prev_click_ms;

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
    int as = np_art_size();
    bool decoded = fetched && rpod_cover_art_decode(raw, raw_size, as, as, &art);
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

/* Re-reads whether `uri` is a liked song and updates the heart (no pop -- this
 * follows track changes and screen returns, not a user tap). Cheap and skipped
 * while the track is unchanged. */
static void update_liked(now_playing_state_t *np, const char *uri)
{
    if (strcmp(np->liked_uri, uri) == 0) {
        return;
    }
    snprintf(np->liked_uri, sizeof(np->liked_uri), "%s", uri);
    bool in = false;
    rpod_mpd_playlist_contains(np->mpd, RPOD_LIKED_PLAYLIST_NAME, uri, &in);
    np->liked = in;
    rpod_heart_set_liked(np->heart, in, false);
}

/* Double-press centre: toggle the current track's liked state, with a pop. */
static void np_toggle_like(now_playing_state_t *np)
{
    if (np->cur_uri[0] == '\0') {
        return;
    }
    bool now_liked = false;
    if (rpod_playlist_liked_toggle(np->mpd, np->cur_uri, &now_liked)) {
        np->liked = now_liked;
        snprintf(np->liked_uri, sizeof(np->liked_uri), "%s", np->cur_uri);
        rpod_heart_set_liked(np->heart, now_liked, true);
    }
}

/* Centre-button gestures on the offscreen proxy. Select splits the same way as
 * the list rows: a double SHORT_CLICKED is a like, and a hold's picker is
 * dispatched on the release's CLICKED (deferred so the pushed picker doesn't
 * eat the release). A single press has no action here, so the double-press
 * costs no latency. */
static void np_proxy_event(lv_event_t *e)
{
    now_playing_state_t *np = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_SHORT_CLICKED) {
        uint32_t now = lv_tick_get();
        if (np->have_prev_click && lv_tick_diff(now, np->prev_click_ms) <= NP_DOUBLE_PRESS_MS) {
            np->have_prev_click = false;
            np_toggle_like(np);
        } else {
            np->have_prev_click = true;
            np->prev_click_ms = now;
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        np->longpress_pending = true;
    } else if (code == LV_EVENT_CLICKED) {
        if (np->longpress_pending) {
            np->longpress_pending = false;
            if (np->cur_uri[0] != '\0') {
                rpod_playlist_picker_push(np->stack, np->mpd, np->cur_uri, np->cur_title);
            }
        }
    }
}

/* On regaining focus (e.g. returning from the picker), the liked state may
 * have changed under us -- force a re-check on the next refresh. */
static void np_loaded_cb(lv_event_t *e)
{
    now_playing_state_t *np = lv_event_get_user_data(e);
    np->liked_uri[0] = '\0';
    if (np->cur_uri[0] != '\0') {
        update_liked(np, np->cur_uri);
    }
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
        snprintf(np->cur_uri, sizeof(np->cur_uri), "%s", status.uri);
        snprintf(np->cur_title, sizeof(np->cur_title), "%.*s", (int)sizeof(np->cur_title) - 1,
                 status.title[0] != '\0' ? status.title : status.uri);
        update_liked(np, status.uri);
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

    /* Hand the status bar back its live title + visualiser now that we're
     * leaving Now Playing (screen deletes only on pop -- see screen_stack.c). */
    rpod_status_bar_set_now_playing_visible(false);

    lv_timer_delete(np->timer);
    /* The square layout has no in-screen visualizer, so no vis timer. */
    if (np->vis_timer != NULL) {
        lv_timer_delete(np->vis_timer);
    }
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

/* Landscape (2" panel): hero cover tile at top-left, title/artist/album
 * stacked to its right, and a full-width scrubber + in-screen visualizer
 * along the bottom. This is the original Now Playing layout. */
static void build_np_landscape(now_playing_state_t *np, lv_obj_t *screen, const rpod_metrics_t *m)
{
    /* --- Cover art tile, top-left: rounded square, clipped so the image
     * (or the placeholder glyph) can't peek past the corners. --- */
    np->art_container = lv_obj_create(screen);
    lv_obj_remove_style_all(np->art_container);
    lv_obj_set_size(np->art_container, np_art_size(), np_art_size());
    lv_obj_align(np->art_container, LV_ALIGN_TOP_LEFT, 14, m->header_h + 10);
    rpod_theme_style_glass_panel(np->art_container, 14);
    lv_obj_set_style_clip_corner(np->art_container, true, 0);
    lv_obj_clear_flag(np->art_container, LV_OBJ_FLAG_SCROLLABLE);

    np->art_img = lv_image_create(np->art_container);
    lv_obj_set_size(np->art_img, np_art_size(), np_art_size());
    lv_obj_center(np->art_img);
    lv_obj_add_flag(np->art_img, LV_OBJ_FLAG_HIDDEN);

    np->art_placeholder = lv_label_create(np->art_container);
    lv_label_set_text(np->art_placeholder, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(np->art_placeholder, m->font_np_glyph, 0);
    lv_obj_set_style_text_color(np->art_placeholder, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_center(np->art_placeholder);

    /* --- Track info, to the right of the art: bold title over separate
     * dimmer artist / album lines, iOS Now-Playing style. --- */
    int info_x = 14 + np_art_size() + 14;
    int info_w = m->screen_w - info_x - 14;

    np->title_label = lv_label_create(screen);
    lv_obj_set_width(np->title_label, info_w);
    lv_label_set_long_mode(np->title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(np->title_label, m->font_title, 0);
    lv_obj_align(np->title_label, LV_ALIGN_TOP_LEFT, info_x, m->header_h + 12);

    np->artist_label = lv_label_create(screen);
    lv_obj_set_width(np->artist_label, info_w);
    lv_label_set_long_mode(np->artist_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(np->artist_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->artist_label, LV_ALIGN_TOP_LEFT, info_x, m->header_h + 44);

    np->album_label = lv_label_create(screen);
    lv_obj_set_width(np->album_label, info_w);
    lv_label_set_long_mode(np->album_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(np->album_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->album_label, LV_ALIGN_TOP_LEFT, info_x, m->header_h + 66);

    /* --- Loved heart, below the title/artist/album block (centred under the
     * info column, to the right of the art). Empty outline until the current
     * track is a liked song. Double-press the centre button to toggle it;
     * press-and-hold to add the track to other playlists. --- */
    np->heart = rpod_heart_create(screen, NP_HEART_SIZE);
    lv_obj_align_to(np->heart, np->album_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

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
    lv_obj_set_size(np->scrubber_panel, m->screen_w - 16, 48);
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
    int bar_w = (m->screen_w - bar_inset) - bar_x;

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
}

/* Square (1.44" panel): a compact vertical stack -- small centred cover, title
 * over a dim artist line, a thin full-width progress bar with elapsed /
 * -remaining beneath it, and a small heart in the top-right corner. No
 * in-screen visualiser (the status bar already shows one) and no scrubber
 * glass panel; there isn't room on 128x128. Creates every widget refresh_cb()
 * touches (album_label exists but stays hidden) so the shared code needs no
 * form guards. */
static void build_np_square(now_playing_state_t *np, lv_obj_t *screen, const rpod_metrics_t *m)
{
    int as = np_art_size();

    np->art_container = lv_obj_create(screen);
    lv_obj_remove_style_all(np->art_container);
    lv_obj_set_size(np->art_container, as, as);
    lv_obj_align(np->art_container, LV_ALIGN_TOP_MID, 0, m->header_h + 4);
    rpod_theme_style_glass_panel(np->art_container, 8);
    lv_obj_set_style_clip_corner(np->art_container, true, 0);
    lv_obj_clear_flag(np->art_container, LV_OBJ_FLAG_SCROLLABLE);

    np->art_img = lv_image_create(np->art_container);
    lv_obj_set_size(np->art_img, as, as);
    lv_obj_center(np->art_img);
    lv_obj_add_flag(np->art_img, LV_OBJ_FLAG_HIDDEN);

    np->art_placeholder = lv_label_create(np->art_container);
    lv_label_set_text(np->art_placeholder, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(np->art_placeholder, m->font_np_glyph, 0);
    lv_obj_set_style_text_color(np->art_placeholder, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_center(np->art_placeholder);

    /* Small "loved" heart, top-right corner over the backdrop. */
    np->heart = rpod_heart_create(screen, 16);
    lv_obj_align(np->heart, LV_ALIGN_TOP_RIGHT, -4, m->header_h + 4);

    int text_top = m->header_h + 4 + as + 4;
    np->title_label = lv_label_create(screen);
    lv_obj_set_width(np->title_label, m->screen_w - 8);
    lv_label_set_long_mode(np->title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(np->title_label, m->font_title, 0);
    lv_obj_set_style_text_align(np->title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(np->title_label, LV_ALIGN_TOP_MID, 0, text_top);

    np->artist_label = lv_label_create(screen);
    lv_obj_set_width(np->artist_label, m->screen_w - 8);
    lv_label_set_long_mode(np->artist_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(np->artist_label, m->font_small, 0);
    lv_obj_set_style_text_color(np->artist_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_style_text_align(np->artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(np->artist_label, LV_ALIGN_TOP_MID, 0,
                text_top + lv_font_get_line_height(m->font_title) + 1);

    /* Album line exists for refresh_cb() but isn't shown on the square panel. */
    np->album_label = lv_label_create(screen);
    lv_obj_add_flag(np->album_label, LV_OBJ_FLAG_HIDDEN);

    np->bar = lv_bar_create(screen);
    lv_obj_remove_style_all(np->bar);
    lv_obj_set_size(np->bar, m->screen_w - 24, 3);
    lv_obj_set_style_radius(np->bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(np->bar, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(np->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(np->bar, RPOD_COLOR_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(np->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(np->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(np->bar, 0, 100);
    lv_obj_align(np->bar, LV_ALIGN_BOTTOM_MID, 0, -20);

    np->thumb = lv_obj_create(screen);
    lv_obj_remove_style_all(np->thumb);
    lv_obj_set_size(np->thumb, 7, 7);
    lv_obj_set_style_radius(np->thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(np->thumb, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(np->thumb, LV_OPA_COVER, 0);
    lv_obj_align_to(np->thumb, np->bar, LV_ALIGN_LEFT_MID, -3, 0);

    /* Brighter than the landscape scrubber's dim times: here they sit directly
     * on the blurred backdrop (no glass panel behind them), where dim gray
     * would wash out on a light cover. */
    np->elapsed_label = lv_label_create(screen);
    lv_obj_set_style_text_color(np->elapsed_label, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(np->elapsed_label, m->font_small, 0);
    lv_obj_align(np->elapsed_label, LV_ALIGN_BOTTOM_LEFT, 12, -5);

    np->remaining_label = lv_label_create(screen);
    lv_obj_set_style_text_color(np->remaining_label, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(np->remaining_label, m->font_small, 0);
    lv_obj_align(np->remaining_label, LV_ALIGN_BOTTOM_RIGHT, -12, -5);
}

void rpod_now_playing_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    now_playing_state_t *np = calloc(1, sizeof(*np));
    np->mpd = ctx;
    np->stack = stack;

    const rpod_metrics_t *m = rpod_metrics();

    /* Reuse the status bar's single FIFO reader rather than starting a second
     * one -- see ui/status_bar.h's warning about two readers splitting the
     * same byte stream. */
    np->vis = rpod_status_bar_shared_visualizer();

    /* --- Backdrop (both forms): a blurred, darkened crop of the current cover
     * art, full-bleed behind the whole screen -- iOS lock-screen style. Created
     * first (bottom of the z-order) so every other widget floats on top of it
     * as glass. Hidden until update_art() has something to show. --- */
    np->bg_img = lv_image_create(screen);
    lv_obj_set_size(np->bg_img, BG_SRC_W, BG_SRC_H);
    lv_obj_center(np->bg_img);
    lv_image_set_scale(np->bg_img, BG_SCALE * 256);
    lv_obj_add_flag(np->bg_img, LV_OBJ_FLAG_HIDDEN);

    if (m->form == RPOD_FORM_SQUARE) {
        build_np_square(np, screen, m);
    } else {
        build_np_landscape(np, screen, m);
    }

    /* Offscreen focusable proxy: Now Playing has no other focusable widget,
     * so this is what the encoder's centre button drives (a plain lv_obj
     * isn't auto-added to the group, so add + focus it by hand). Not in edit
     * mode -- there's nothing to rotate through here. */
    np->proxy = lv_obj_create(screen);
    lv_obj_remove_style_all(np->proxy);
    lv_obj_set_size(np->proxy, 1, 1);
    lv_obj_set_pos(np->proxy, 0, 0);
    lv_obj_remove_flag(np->proxy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(np->proxy, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(np->proxy, np_proxy_event, LV_EVENT_SHORT_CLICKED, np);
    lv_obj_add_event_cb(np->proxy, np_proxy_event, LV_EVENT_LONG_PRESSED, np);
    lv_obj_add_event_cb(np->proxy, np_proxy_event, LV_EVENT_CLICKED, np);

    lv_group_t *g = lv_group_get_default();
    if (g != NULL) {
        lv_group_add_obj(g, np->proxy);
        lv_group_focus_obj(np->proxy);
    }

    np->timer = lv_timer_create(refresh_cb, 1000, np);
    refresh_cb(np->timer);

    lv_obj_add_event_cb(screen, np_loaded_cb, LV_EVENT_SCREEN_LOADED, np);
    lv_obj_add_event_cb(screen, screen_delete_cb, LV_EVENT_DELETE, np);

    /* While this screen is up the status bar drops its title+visualiser for a
     * plain "Now Playing" -- undone in screen_delete_cb on pop. */
    rpod_status_bar_set_now_playing_visible(true);
}
