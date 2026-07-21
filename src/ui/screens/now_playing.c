#include "now_playing.h"

#include "audio/mpd_client.h"
#include "ui/cover_art.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Square cover art tile, center-cropped ("aspect fill") from whatever the
 * source image's aspect ratio actually is -- see cover_art.c. */
#define ART_SIZE 128

typedef struct {
    rpod_mpd_t *mpd;

    lv_obj_t *art_container;
    lv_obj_t *art_img;
    lv_obj_t *art_placeholder; /* LV_SYMBOL_AUDIO tile shown when there's no art */
    lv_image_dsc_t art_dsc;    /* backing store for art_img's LV_IMAGE_SRC_VARIABLE */

    lv_obj_t *title_label;
    lv_obj_t *artist_label;
    lv_obj_t *album_label;

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

    unsigned char *raw = NULL;
    size_t raw_size = 0;
    rpod_cover_art_t art = { 0 };
    bool decoded = rpod_mpd_get_cover_art(np->mpd, uri, &raw, &raw_size) &&
                   rpod_cover_art_decode(raw, raw_size, ART_SIZE, ART_SIZE, &art);
    rpod_mpd_free_cover_art(raw);

    if (decoded) {
        np->art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        np->art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        np->art_dsc.header.flags = 0;
        np->art_dsc.header.w = (uint16_t)art.w;
        np->art_dsc.header.h = (uint16_t)art.h;
        np->art_dsc.header.stride = (uint16_t)(art.w * 2);
        np->art_dsc.data_size = (uint32_t)(art.w * art.h * 2);
        np->art_dsc.data = (const uint8_t *)art.pixels;
        np->have_art = true;

        lv_image_set_src(np->art_img, &np->art_dsc);
        lv_obj_remove_flag(np->art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(np->art_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(np->art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(np->art_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    now_playing_state_t *np = lv_timer_get_user_data(timer);

    rpod_mpd_status_t status;
    if (!rpod_mpd_get_status(np->mpd, &status)) {
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

static void screen_delete_cb(lv_event_t *e)
{
    now_playing_state_t *np = lv_event_get_user_data(e);
    lv_timer_delete(np->timer);
    if (np->have_art) {
        free((void *)np->art_dsc.data);
    }
    free(np);
}

void rpod_now_playing_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;

    rpod_theme_create_header(screen, "Now Playing");

    now_playing_state_t *np = calloc(1, sizeof(*np));
    np->mpd = ctx;

    /* --- Cover art tile, top-left: rounded square, clipped so the image
     * (or the placeholder glyph) can't peek past the corners. --- */
    np->art_container = lv_obj_create(screen);
    lv_obj_remove_style_all(np->art_container);
    lv_obj_set_size(np->art_container, ART_SIZE, ART_SIZE);
    lv_obj_align(np->art_container, LV_ALIGN_TOP_LEFT, 14, RPOD_HEADER_HEIGHT + 10);
    lv_obj_set_style_radius(np->art_container, 14, 0);
    lv_obj_set_style_clip_corner(np->art_container, true, 0);
    lv_obj_set_style_bg_color(np->art_container, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_bg_opa(np->art_container, LV_OPA_COVER, 0);
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
     * on top, so this screen is display-only. --- */
    np->bar = lv_bar_create(screen);
    lv_obj_remove_style_all(np->bar);
    lv_obj_set_size(np->bar, RPOD_SCREEN_WIDTH - 28, 4);
    lv_obj_set_style_radius(np->bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(np->bar, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(np->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(np->bar, RPOD_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(np->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(np->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(np->bar, 0, 100);
    lv_obj_align(np->bar, LV_ALIGN_BOTTOM_MID, 0, -40);

    np->thumb = lv_obj_create(screen);
    lv_obj_remove_style_all(np->thumb);
    lv_obj_set_size(np->thumb, 10, 10);
    lv_obj_set_style_radius(np->thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(np->thumb, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(np->thumb, LV_OPA_COVER, 0);
    lv_obj_align_to(np->thumb, np->bar, LV_ALIGN_LEFT_MID, -5, 0);

    np->elapsed_label = lv_label_create(screen);
    lv_obj_set_style_text_color(np->elapsed_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->elapsed_label, LV_ALIGN_BOTTOM_LEFT, 14, -20);

    np->remaining_label = lv_label_create(screen);
    lv_obj_set_style_text_color(np->remaining_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->remaining_label, LV_ALIGN_BOTTOM_RIGHT, -14, -20);

    np->timer = lv_timer_create(refresh_cb, 1000, np);
    refresh_cb(np->timer);

    lv_obj_add_event_cb(screen, screen_delete_cb, LV_EVENT_DELETE, np);
}
