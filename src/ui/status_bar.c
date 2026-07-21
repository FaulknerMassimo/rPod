#include "status_bar.h"

#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Same small equalizer-style bars as Now Playing's own visualizer (see
 * now_playing.c), just smaller -- this lives in a 28px-tall bar next to the
 * title instead of a dedicated panel. */
#define VIS_BAR_W       3
#define VIS_BAR_GAP     2
#define VIS_H           14
#define VIS_MIN_H       2
#define VIS_PERIOD_MS   33   /* ~30Hz */
#define CLOCK_PERIOD_MS 1000
#define PLAYER_PERIOD_MS 1000

/* Leaves enough room on both sides for the clock and battery labels at
 * RPOD_SCREEN_WIDTH (320) -- long titles truncate with "..." rather than
 * overlapping either. */
#define TITLE_MAX_W 160

struct rpod_status_bar {
    rpod_mpd_t *mpd;
    rpod_visualizer_t *vis;

    lv_obj_t *time_label;

    lv_obj_t *center;         /* row: vis bars (hidden when idle) + title/rPod label, kept centered as a unit */
    lv_obj_t *title_label;
    lv_obj_t *vis_container;
    lv_obj_t *vis_bars[RPOD_VIS_BANDS];
    bool vis_visible;

    lv_timer_t *clock_timer;
    lv_timer_t *player_timer;
    lv_timer_t *vis_timer;
};

/* Only one bar is ever created per process (see status_bar.h) -- exposed
 * globally so screens elsewhere in the stack (Now Playing) can reach its
 * shared visualizer handle without threading a pointer through every
 * build_fn's ctx. */
static rpod_status_bar_t *g_status_bar;

/* Same env-var-override-with-a-sim-default shape as sim_main.c's MPD socket
 * resolution -- RPOD_VIS_FIFO lets a future on-device systemd unit point at
 * wherever mpd.conf's fifo output actually lives (docs/PLAN.md #6.2)
 * without this needing to know sim vs device. */
static void resolve_vis_fifo_path(char *out, size_t out_size)
{
    const char *override = getenv("RPOD_VIS_FIFO");
    if (override != NULL) {
        snprintf(out, out_size, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, out_size, "%s/.local/state/rpod-sim/mpd/visualizer.fifo", home != NULL ? home : "");
}

static void clock_timer_cb(lv_timer_t *timer)
{
    rpod_status_bar_t *bar = lv_timer_get_user_data(timer);

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
    lv_label_set_text(bar->time_label, buf);
}

static void set_vis_visible(rpod_status_bar_t *bar, bool visible)
{
    if (visible == bar->vis_visible) {
        return;
    }
    bar->vis_visible = visible;
    if (visible) {
        lv_obj_remove_flag(bar->vis_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bar->vis_container, LV_OBJ_FLAG_HIDDEN);
    }
}

static void player_timer_cb(lv_timer_t *timer)
{
    rpod_status_bar_t *bar = lv_timer_get_user_data(timer);

    rpod_mpd_status_t status;
    bool connected = rpod_mpd_get_status(bar->mpd, &status);
    /* "There's a current track" -- not "is actively playing right now" --
     * same condition main_menu.c uses to decide whether to show its own
     * "Now Playing" row, so a paused track still keeps its title up here
     * instead of falling back to "rPod" the moment you pause. */
    bool has_now_playing = connected && status.title[0] != '\0';

    lv_label_set_text(bar->title_label, has_now_playing ? status.title : "rPod");
    set_vis_visible(bar, has_now_playing);
}

static void vis_timer_cb(lv_timer_t *timer)
{
    rpod_status_bar_t *bar = lv_timer_get_user_data(timer);
    if (!bar->vis_visible) {
        return;
    }

    float levels[RPOD_VIS_BANDS];
    rpod_visualizer_get_levels(bar->vis, levels);
    for (int i = 0; i < RPOD_VIS_BANDS; i++) {
        int32_t h = VIS_MIN_H + (int32_t)(levels[i] * (VIS_H - VIS_MIN_H));
        /* Set height *and* y explicitly every tick, same as Now Playing's
         * visualizer -- pins the bottom edge at a fixed pixel regardless of
         * height rather than trusting flex to hold it there. */
        lv_obj_set_height(bar->vis_bars[i], h);
        lv_obj_set_y(bar->vis_bars[i], VIS_H - h);
    }
}

rpod_status_bar_t *rpod_status_bar_create(lv_display_t *disp, rpod_mpd_t *mpd)
{
    rpod_status_bar_t *bar = calloc(1, sizeof(*bar));
    bar->mpd = mpd;

    char fifo_path[512];
    resolve_vis_fifo_path(fifo_path, sizeof(fifo_path));
    bar->vis = rpod_visualizer_start(fifo_path);

    /* System layer: the same object on every screen, always drawn above
     * whatever rpod_screen_stack_t has currently loaded -- see
     * lv_display_get_layer_sys()'s doc comment in lv_display.h. */
    lv_obj_t *layer = lv_display_get_layer_sys(disp);

    lv_obj_t *header = lv_obj_create(layer);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), RPOD_HEADER_HEIGHT);
    rpod_theme_style_glass_bar(header);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    bar->time_label = lv_label_create(header);
    lv_label_set_text(bar->time_label, "--:--");
    lv_obj_set_style_text_color(bar->time_label, RPOD_COLOR_TEXT, 0);
    lv_obj_align(bar->time_label, LV_ALIGN_LEFT_MID, 10, 0);

    /* Battery: no fuel-gauge hardware wired up yet (docs/PLAN.md's
     * hardware notes / settings_screens.c's "About" screen), so this is
     * deliberately a fixed "unknown" reading -- a dim, unfilled icon next
     * to "--%" -- rather than a fabricated percentage. */
    lv_obj_t *battery_row = lv_obj_create(header);
    lv_obj_remove_style_all(battery_row);
    lv_obj_set_size(battery_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(battery_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_row, 4, 0);
    lv_obj_clear_flag(battery_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(battery_row, LV_ALIGN_RIGHT_MID, -10, 0);

    lv_obj_t *battery_label = lv_label_create(battery_row);
    lv_label_set_text(battery_label, "--%");
    lv_obj_set_style_text_color(battery_label, RPOD_COLOR_DIM_TEXT, 0);

    lv_obj_t *battery_icon = lv_label_create(battery_row);
    lv_label_set_text(battery_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(battery_icon, RPOD_COLOR_DIM_TEXT, 0);

    /* Centre group: visualizer bars (hidden until there's a current track)
     * + title, both flexed in one row so the pair re-centers as a unit when
     * the bars show/hide instead of the title jumping sideways on its own. */
    bar->center = lv_obj_create(header);
    lv_obj_remove_style_all(bar->center);
    lv_obj_set_size(bar->center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar->center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar->center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar->center, 6, 0);
    lv_obj_clear_flag(bar->center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar->center, LV_ALIGN_CENTER, 0, 0);

    int vis_total_w = RPOD_VIS_BANDS * VIS_BAR_W + (RPOD_VIS_BANDS - 1) * VIS_BAR_GAP;
    bar->vis_container = lv_obj_create(bar->center);
    lv_obj_remove_style_all(bar->vis_container);
    lv_obj_set_size(bar->vis_container, vis_total_w, VIS_H);
    lv_obj_clear_flag(bar->vis_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar->vis_container, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < RPOD_VIS_BANDS; i++) {
        lv_obj_t *b = lv_obj_create(bar->vis_container);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, VIS_BAR_W, 2);
        lv_obj_set_pos(b, i * (VIS_BAR_W + VIS_BAR_GAP), VIS_H - 2);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_color(b, RPOD_COLOR_TEXT, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        bar->vis_bars[i] = b;
    }

    bar->title_label = lv_label_create(bar->center);
    lv_label_set_text(bar->title_label, "rPod");
    lv_obj_set_style_text_color(bar->title_label, RPOD_COLOR_TEXT, 0);
    lv_label_set_long_mode(bar->title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(bar->title_label, TITLE_MAX_W);
    lv_obj_set_style_text_align(bar->title_label, LV_TEXT_ALIGN_CENTER, 0);

    bar->clock_timer = lv_timer_create(clock_timer_cb, CLOCK_PERIOD_MS, bar);
    clock_timer_cb(bar->clock_timer);

    bar->player_timer = lv_timer_create(player_timer_cb, PLAYER_PERIOD_MS, bar);
    player_timer_cb(bar->player_timer);

    bar->vis_timer = lv_timer_create(vis_timer_cb, VIS_PERIOD_MS, bar);

    g_status_bar = bar;
    return bar;
}

rpod_visualizer_t *rpod_status_bar_shared_visualizer(void)
{
    return g_status_bar != NULL ? g_status_bar->vis : NULL;
}
