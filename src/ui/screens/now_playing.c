#include "now_playing.h"

#include "audio/mpd_client.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    rpod_mpd_t *mpd;
    lv_obj_t *title_label;
    lv_obj_t *artist_album_label;
    lv_obj_t *state_label;
    lv_obj_t *bar;
    lv_obj_t *time_label;
    lv_timer_t *timer;
} now_playing_state_t;

static void refresh_cb(lv_timer_t *timer)
{
    now_playing_state_t *np = lv_timer_get_user_data(timer);

    rpod_mpd_status_t status;
    if (!rpod_mpd_get_status(np->mpd, &status)) {
        lv_label_set_text(np->state_label, "(disconnected)");
        return;
    }

    lv_label_set_text(np->title_label, status.title[0] != '\0' ? status.title : "(unknown title)");

    char sub[600];
    snprintf(sub, sizeof(sub), "%s - %s",
              status.artist[0] != '\0' ? status.artist : "Unknown artist",
              status.album[0] != '\0' ? status.album : "Unknown album");
    lv_label_set_text(np->artist_album_label, sub);

    const char *state_str = "Stopped";
    if (status.state == RPOD_MPD_STATE_PLAY) {
        state_str = "Playing";
    } else if (status.state == RPOD_MPD_STATE_PAUSE) {
        state_str = "Paused";
    }
    lv_label_set_text(np->state_label, state_str);

    int pct = status.duration_s > 0 ? (int)((status.elapsed_s * 100u) / status.duration_s) : 0;
    lv_bar_set_value(np->bar, pct, LV_ANIM_OFF);

    char t[64];
    snprintf(t, sizeof(t), "%u:%02u / %u:%02u",
              status.elapsed_s / 60u, status.elapsed_s % 60u,
              status.duration_s / 60u, status.duration_s % 60u);
    lv_label_set_text(np->time_label, t);
}

static void screen_delete_cb(lv_event_t *e)
{
    now_playing_state_t *np = lv_event_get_user_data(e);
    lv_timer_delete(np->timer);
    free(np);
}

void rpod_now_playing_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;

    rpod_theme_create_header(screen, "Now Playing");

    now_playing_state_t *np = calloc(1, sizeof(*np));
    np->mpd = ctx;

    np->title_label = lv_label_create(screen);
    lv_obj_set_width(np->title_label, RPOD_SCREEN_WIDTH - 20);
    lv_label_set_long_mode(np->title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_align(np->title_label, LV_ALIGN_TOP_MID, 0, RPOD_HEADER_HEIGHT + 20);

    np->artist_album_label = lv_label_create(screen);
    lv_obj_set_width(np->artist_album_label, RPOD_SCREEN_WIDTH - 20);
    lv_label_set_long_mode(np->artist_album_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(np->artist_album_label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(np->artist_album_label, LV_ALIGN_TOP_MID, 0, RPOD_HEADER_HEIGHT + 44);

    np->state_label = lv_label_create(screen);
    lv_obj_align(np->state_label, LV_ALIGN_TOP_MID, 0, RPOD_HEADER_HEIGHT + 68);

    np->bar = lv_bar_create(screen);
    lv_obj_set_size(np->bar, RPOD_SCREEN_WIDTH - 40, 8);
    lv_bar_set_range(np->bar, 0, 100);
    lv_obj_align(np->bar, LV_ALIGN_BOTTOM_MID, 0, -36);

    np->time_label = lv_label_create(screen);
    lv_obj_align(np->time_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    np->timer = lv_timer_create(refresh_cb, 1000, np);
    refresh_cb(np->timer);

    lv_obj_add_event_cb(screen, screen_delete_cb, LV_EVENT_DELETE, np);
}
