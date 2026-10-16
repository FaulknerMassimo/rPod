#include "status_bar.h"

#include "airpods_art.h"
#include "classic.h"
#include "metrics.h"
#include "theme.h"
#include "audio/airpods.h"

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

/* Leaves enough room on both sides for the clock and battery labels. A title
 * wider than this scrolls within it (see set_title()) rather than overlapping
 * either; a shorter one hugs its text. */
#define TITLE_MAX_W 160

/* The classic title bar, at 2x of the iPod's (re/ipod4/SPEC.md §5.1): 19
 * lines with a rule along the last, the play state at (4, 4), the battery
 * at (134, 4), the title centred between x = 37 and 122. */
#define CL_PX           RPOD_CLASSIC_PX
#define CL_RULE_Y       (18 * CL_PX)
#define CL_ICON_Y       (4 * CL_PX)
#define CL_PLAY_X       (4 * CL_PX)
#define CL_LOCK_X       (24 * CL_PX) /* the hold icon, right after the play state's 20 */
#define CL_LOCK_Y       (3 * CL_PX)
#define CL_BATTERY_X    (134 * CL_PX)
#define CL_TITLE_X      (37 * CL_PX)
#define CL_TITLE_W      ((122 - 37) * CL_PX)
#define CL_TITLE_Y      (1 * CL_PX)
#define CL_ICON_H       (10 * CL_PX)
#define CL_AIRPODS_GAP  (4 * CL_PX)

struct rpod_status_bar {
    rpod_mpd_t *mpd;
    rpod_visualizer_t *vis;
    lv_display_t *disp;

    /* Everything below is rebuilt by rpod_status_bar_restyle(); widgets the
     * active theme doesn't have are NULL. */
    lv_obj_t *header;
    lv_obj_t *time_label;
    lv_obj_t *airpods_icon;

    /* Classic: the play state icon, the lock, the screen's title. */
    lv_obj_t *play_icon;
    lv_obj_t *lock_icon;
    bool locked;
    lv_obj_t *screen_title_label;
    char screen_title[64];

    lv_obj_t *center;         /* row: vis bars (hidden when idle) + title/rPod label, kept centered as a unit */
    lv_obj_t *title_label;
    lv_obj_t *vis_container;
    lv_obj_t *vis_bars[RPOD_VIS_BANDS];
    bool vis_visible;
    char fixed_title[32]; /* rpod_status_bar_set_fixed_title(); "" = the track's */
    bool hidden;          /* rpod_status_bar_set_hidden() */

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
    if (bar->time_label == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
    lv_label_set_text(bar->time_label, buf);
}

static void set_vis_visible(rpod_status_bar_t *bar, bool visible)
{
    if (visible == bar->vis_visible || bar->vis_container == NULL) {
        return;
    }
    bar->vis_visible = visible;
    if (visible) {
        lv_obj_remove_flag(bar->vis_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bar->vis_container, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Sets the centre title and sizes the label to match. A title that fits
 * within TITLE_MAX_W is sized to its content so it hugs its text -- the flex
 * row then centres the visualiser+title pair as a tight unit, with the bars
 * right next to the title. A longer one gets a fixed TITLE_MAX_W and scrolls
 * (LV_LABEL_LONG_MODE_SCROLL_CIRCULAR, same as Now Playing's own title) rather
 * than truncating or wrapping to a second line. No-ops when the text is
 * unchanged, so the 1Hz refresh never restarts the scroll animation. */
static void set_title(rpod_status_bar_t *bar, const char *text)
{
    if (strcmp(lv_label_get_text(bar->title_label), text) == 0) {
        return;
    }
    lv_label_set_text(bar->title_label, text);

    const lv_font_t *font = lv_obj_get_style_text_font(bar->title_label, LV_PART_MAIN);
    int32_t letter_space = lv_obj_get_style_text_letter_space(bar->title_label, LV_PART_MAIN);
    lv_point_t size;
    lv_text_get_size(&size, text, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);

    lv_obj_set_width(bar->title_label, size.x <= TITLE_MAX_W ? LV_SIZE_CONTENT : TITLE_MAX_W);
}

static void update_player(rpod_status_bar_t *bar)
{
    rpod_mpd_status_t status;
    /* _settled(): when an album/playlist plays out to its end, MPD (repeat off)
     * stops and forgets the current song. This global poll runs regardless of
     * which screen is on top, so using the settled fetch here re-cues to the
     * first track paused -- catching the queue ending even when Now Playing
     * isn't up, and never flashing the stopped "unknown" title. */
    bool connected = rpod_mpd_get_status_settled(bar->mpd, &status);

    /* Classic: the screen's own title (rpod_status_bar_set_screen_title()),
     * and whether something's playing or paused, as the iPod's title bar
     * showed it. */
    if (bar->play_icon != NULL) {
        rpod_mpd_state_t state = connected ? status.state : RPOD_MPD_STATE_UNKNOWN;
        if (state == RPOD_MPD_STATE_PLAY || state == RPOD_MPD_STATE_PAUSE) {
            rpod_classic_icon_set(bar->play_icon,
                                  state == RPOD_MPD_STATE_PLAY ? RPOD_ICON_PLAY : RPOD_ICON_PAUSE);
            lv_obj_remove_flag(bar->play_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bar->play_icon, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* "There's a current track" -- not "is actively playing right now" --
     * same condition main_menu.c uses to decide whether to show its own
     * "Now Playing" row, so a paused track still keeps its title up here
     * instead of falling back to "rPod" the moment you pause. */
    bool has_now_playing = connected && status.title[0] != '\0';

    /* Pinned by the screen on top -- see rpod_status_bar_set_fixed_title(). */
    if (bar->fixed_title[0] != '\0') {
        set_title(bar, bar->fixed_title);
        set_vis_visible(bar, false);
    } else {
        set_title(bar, has_now_playing ? status.title : "rPod");
        set_vis_visible(bar, has_now_playing);
    }
}

/* Shown while AirPods are connected: dim until their controls are up, so it
 * reads "connected" and then "ready" the way the battery card arrives. */
static void airpods_changed_cb(void *user)
{
    rpod_status_bar_t *bar = user;
    const rpod_airpods_t *ap = rpod_airpods();
    if (ap->link == RPOD_AIRPODS_ABSENT) {
        lv_obj_add_flag(bar->airpods_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(bar->airpods_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_image_recolor(bar->airpods_icon,
                                   ap->link == RPOD_AIRPODS_READY ? RPOD_COLOR_TEXT
                                                                  : RPOD_COLOR_DIM_TEXT, 0);
}

static void player_timer_cb(lv_timer_t *timer)
{
    update_player(lv_timer_get_user_data(timer));
}

static void vis_timer_cb(lv_timer_t *timer)
{
    rpod_status_bar_t *bar = lv_timer_get_user_data(timer);
    if (!bar->vis_visible || bar->vis_container == NULL) {
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

/* The glass bar's widgets: clock, title + visualiser, AirPods and battery. */
static void build_glass(rpod_status_bar_t *bar, lv_obj_t *layer)
{
    const rpod_metrics_t *m = rpod_metrics();

    lv_obj_t *header = lv_obj_create(layer);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), m->header_h);
    rpod_theme_style_glass_bar(header);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    bar->header = header;

    bar->time_label = lv_label_create(header);
    lv_label_set_text(bar->time_label, "--:--");
    lv_obj_set_style_text_color(bar->time_label, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(bar->time_label, m->font_small, 0);
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

    /* First in the row, so it sits left of the readout and the row grows
     * leftward (it's right-aligned) without moving the battery. */
    bar->airpods_icon = rpod_airpods_image_create(battery_row, rpod_airpods_glyph(false), RPOD_COLOR_TEXT);
    lv_obj_set_style_margin_right(bar->airpods_icon, 4, 0);
    lv_obj_add_flag(bar->airpods_icon, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_set_style_text_color(bar->title_label, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(bar->title_label, m->font_small, 0);
    /* SCROLL_CIRCULAR (same as Now Playing's title): titles wider than
     * TITLE_MAX_W scroll rather than truncating/wrapping; set_title() sizes
     * shorter ones to content and this centre align keeps them centred. */
    lv_label_set_long_mode(bar->title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(bar->title_label, LV_TEXT_ALIGN_CENTER, 0);
    set_title(bar, "rPod");
}

/* The classic title bar's widgets (see CL_* above). */
static void build_classic(rpod_status_bar_t *bar, lv_obj_t *layer)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_color_t fg = RPOD_COLOR_TEXT;

    lv_obj_t *header = lv_obj_create(layer);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), m->header_h);
    lv_obj_set_style_bg_color(header, RPOD_COLOR_HEADER_BG, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    bar->header = header;

    lv_obj_t *rule = lv_obj_create(header);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), CL_PX);
    lv_obj_set_pos(rule, 0, CL_RULE_Y);
    lv_obj_set_style_bg_color(rule, fg, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    bar->play_icon = rpod_classic_icon_create(header, RPOD_ICON_PLAY, fg);
    lv_obj_set_pos(bar->play_icon, CL_PLAY_X, CL_ICON_Y);
    lv_obj_add_flag(bar->play_icon, LV_OBJ_FLAG_HIDDEN);

    bar->lock_icon = rpod_classic_icon_create(header, RPOD_ICON_LOCK, fg);
    lv_obj_set_pos(bar->lock_icon, CL_LOCK_X, CL_LOCK_Y);
    if (!bar->locked) {
        lv_obj_add_flag(bar->lock_icon, LV_OBJ_FLAG_HIDDEN);
    }

    bar->screen_title_label = lv_label_create(header);
    lv_obj_set_style_text_font(bar->screen_title_label, m->font_body, 0);
    lv_obj_set_style_text_color(bar->screen_title_label, fg, 0);
    lv_obj_set_style_text_align(bar->screen_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(bar->screen_title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(bar->screen_title_label, CL_TITLE_W, lv_font_get_line_height(m->font_body));
    lv_obj_set_pos(bar->screen_title_label, CL_TITLE_X, CL_TITLE_Y);
    lv_label_set_text(bar->screen_title_label, bar->screen_title);

    /* No fuel gauge yet (see the glass bar's battery), so the battery is
     * drawn empty and dim rather than with a made-up level. */
    lv_obj_t *battery = rpod_classic_icon_create(header, RPOD_ICON_BATTERY, RPOD_COLOR_DIM_TEXT);
    lv_obj_set_pos(battery, CL_BATTERY_X, CL_ICON_Y);

    /* Its foot level with the battery's, on the iPod's pixel grid. */
    const lv_image_dsc_t *glyph = rpod_airpods_glyph(false);
    bar->airpods_icon = rpod_airpods_image_create(header, glyph, fg);
    if (glyph != NULL) {
        lv_obj_set_pos(bar->airpods_icon, CL_BATTERY_X - CL_AIRPODS_GAP - glyph->header.w,
                       CL_ICON_Y + CL_ICON_H - glyph->header.h);
    }
    lv_obj_add_flag(bar->airpods_icon, LV_OBJ_FLAG_HIDDEN);
}

/* (Re)creates the widgets in the active theme's look, below anything else
 * on the system layer (the HUD and the scrub letter draw over the bar). */
static void build_widgets(rpod_status_bar_t *bar)
{
    /* System layer: the same object on every screen, always drawn above
     * whatever rpod_screen_stack_t has currently loaded -- see
     * lv_display_get_layer_sys()'s doc comment in lv_display.h. */
    lv_obj_t *layer = lv_display_get_layer_sys(bar->disp);

    if (rpod_theme()->classic) {
        build_classic(bar, layer);
    } else {
        build_glass(bar, layer);
    }
    lv_obj_move_to_index(bar->header, 0);
    if (bar->hidden) {
        lv_obj_add_flag(bar->header, LV_OBJ_FLAG_HIDDEN);
    }
}

rpod_status_bar_t *rpod_status_bar_create(lv_display_t *disp, rpod_mpd_t *mpd)
{
    rpod_status_bar_t *bar = calloc(1, sizeof(*bar));
    bar->mpd = mpd;
    bar->disp = disp;
    snprintf(bar->screen_title, sizeof(bar->screen_title), "rPod");

    char fifo_path[512];
    resolve_vis_fifo_path(fifo_path, sizeof(fifo_path));
    bar->vis = rpod_visualizer_start(fifo_path);

    build_widgets(bar);

    bar->clock_timer = lv_timer_create(clock_timer_cb, CLOCK_PERIOD_MS, bar);
    clock_timer_cb(bar->clock_timer);

    bar->player_timer = lv_timer_create(player_timer_cb, PLAYER_PERIOD_MS, bar);
    player_timer_cb(bar->player_timer);

    bar->vis_timer = lv_timer_create(vis_timer_cb, VIS_PERIOD_MS, bar);

    rpod_airpods_watch(NULL, airpods_changed_cb, bar);
    airpods_changed_cb(bar);

    g_status_bar = bar;
    return bar;
}

rpod_visualizer_t *rpod_status_bar_shared_visualizer(void)
{
    return g_status_bar != NULL ? g_status_bar->vis : NULL;
}

void rpod_status_bar_set_fixed_title(const char *title)
{
    if (g_status_bar == NULL) {
        return;
    }
    snprintf(g_status_bar->fixed_title, sizeof(g_status_bar->fixed_title), "%s",
             title != NULL ? title : "");
    /* Reflect it now rather than waiting up to a second for the next poll, so
     * the title/vis swap lands with the screen transition. */
    update_player(g_status_bar);
}

void rpod_status_bar_set_locked(bool locked)
{
    rpod_status_bar_t *bar = g_status_bar;
    if (bar == NULL) {
        return;
    }
    bar->locked = locked;
    if (bar->lock_icon == NULL) {
        return;
    }
    if (locked) {
        lv_obj_remove_flag(bar->lock_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bar->lock_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

void rpod_status_bar_set_screen_title(const char *title)
{
    if (g_status_bar == NULL) {
        return;
    }
    snprintf(g_status_bar->screen_title, sizeof(g_status_bar->screen_title), "%s", title);
    if (g_status_bar->screen_title_label != NULL) {
        lv_label_set_text(g_status_bar->screen_title_label, g_status_bar->screen_title);
    }
}

void rpod_status_bar_refresh(void)
{
    if (g_status_bar != NULL) {
        lv_timer_ready(g_status_bar->player_timer);
    }
}

void rpod_status_bar_restyle(void)
{
    rpod_status_bar_t *bar = g_status_bar;
    if (bar == NULL) {
        return;
    }
    lv_obj_delete(bar->header);
    bar->header = NULL;
    bar->time_label = NULL;
    bar->airpods_icon = NULL;
    bar->play_icon = NULL;
    bar->lock_icon = NULL;
    bar->screen_title_label = NULL;
    bar->center = NULL;
    bar->title_label = NULL;
    bar->vis_container = NULL;
    for (int i = 0; i < RPOD_VIS_BANDS; i++) {
        bar->vis_bars[i] = NULL;
    }
    bar->vis_visible = false;

    build_widgets(bar);
    clock_timer_cb(bar->clock_timer);
    update_player(bar);
    airpods_changed_cb(bar);
}

void rpod_status_bar_set_hidden(bool hidden)
{
    rpod_status_bar_t *bar = g_status_bar;
    if (bar == NULL || bar->hidden == hidden) {
        return;
    }
    bar->hidden = hidden;
    if (hidden) {
        lv_obj_add_flag(bar->header, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(bar->header, LV_OBJ_FLAG_HIDDEN);
    }
}
