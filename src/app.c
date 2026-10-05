#include "app.h"

#include "audio/airpods.h"
#include "audio/bluetooth.h"
#include "audio/listenbrainz.h"
#include "audio/mpd_client.h"
#include "audio/scrobbler.h"
#include "audio/volume_memory.h"
#include "input/input.h"
#include "ui/airpods_notify.h"
#include "ui/backlight.h"
#include "ui/cover_cache.h"
#include "ui/hud.h"
#include "ui/screens/main_menu.h"
#include "ui/screens/now_playing.h"
#include "ui/screens/screen_stack.h"
#include "ui/scrub.h"
#include "ui/seek_control.h"
#include "ui/sleep_timer.h"
#include "ui/status_bar.h"
#include "ui/volume_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Prev this far or more into a song restarts it; any earlier, it goes to the
 * previous song (docs/PLAN.md §8.2). */
#define PREV_RESTART_MS 2000

/* One per process, kept static for the input actions the same way
 * tools/sim/sim_main.c used to: the screen stack Menu pops, the input it
 * puts to sleep, and what sleep turns off. */
static rpod_screen_stack_t *g_stack;
static rpod_input_t *g_input;
static const rpod_board_t *g_board;
static lv_display_t *g_disp;
static lv_obj_t *g_curtain; /* black, over everything, while asleep */

/* The screen goes dark: black, rendering stopped, the panel switched off
 * where the board can -- and it comes back where it was. Sleep (docs/PLAN.md
 * §8.2: Play/Pause held, or the sleep timer) is the iPod's "off", which only
 * a press wakes. The backlight timer's `dim` (§8.3) is just the screen:
 * touching the wheel wakes it too, and the transport buttons work blind
 * (input/gestures.h). Phase 5's low-power sleep builds on this. */
static void go_dark(bool dim)
{
    if (dim && rpod_input_asleep(g_input)) {
        return; /* never lighten a full sleep */
    }
    rpod_input_sleep(g_input, dim);
    fprintf(stderr, dim ? "rpod: backlight off\n" : "rpod: asleep\n");
    if (g_curtain != NULL) {
        return; /* dark already: a dim sleep just deepened */
    }

    g_curtain = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(g_curtain);
    lv_obj_set_size(g_curtain, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_curtain, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_curtain, LV_OPA_COVER, 0);
    /* Drawn before rendering stops: it's also what the panel shows for the
     * moment it's back on, rather than the screen it went to sleep on. */
    lv_refr_now(g_disp);
    lv_display_enable_invalidation(g_disp, false);
    if (g_board->set_display_power != NULL) {
        g_board->set_display_power(false);
    }
}

static void on_wake(void *ctx)
{
    (void)ctx;
    if (g_curtain == NULL) {
        return;
    }
    if (g_board->set_display_power != NULL) {
        g_board->set_display_power(true);
    }
    lv_display_enable_invalidation(g_disp, true);
    lv_obj_delete(g_curtain); /* invalidates the whole screen */
    g_curtain = NULL;
    fprintf(stderr, "rpod: awake\n");
}

/* The app-level buttons' actions (docs/PLAN.md §8.2; taps and holds are
 * sorted out in src/input/gestures.c). Menu closes an open modal overlay
 * (e.g. the playlist picker) if there is one, else pops the stack -- and at
 * the root does nothing, so mashing it to back out of a deep menu can't
 * also put the device to sleep; only holding Play/Pause does. The transport
 * ones act on the MPD client passed as ctx. Which physical control maps to
 * each is the board's business (see the input backends). */
static void on_menu(void *ctx)
{
    (void)ctx;
    if (!rpod_screen_stack_close_overlay(g_stack)) {
        rpod_screen_stack_pop(g_stack); /* no-op at the root */
    }
}

static void on_play_pause(void *ctx)
{
    rpod_mpd_toggle_pause((rpod_mpd_t *)ctx);
}

/* Play/Pause held, or the sleep timer: pause and sleep -- the iPod's "off".
 * A scan still going (Next held too) ends first, or its resume would undo
 * the pause. Sleeping by hand cancels the sleep timer. */
static void on_sleep(void *ctx)
{
    rpod_sleep_timer_set(0);
    rpod_seek_control_stop();
    rpod_mpd_set_paused((rpod_mpd_t *)ctx, true);
    go_dark(false);
}

static void on_backlight_off(void *ctx)
{
    (void)ctx;
    go_dark(true);
}

static void on_next(void *ctx)
{
    rpod_mpd_next((rpod_mpd_t *)ctx);
    rpod_now_playing_refresh();
}

static void on_prev(void *ctx)
{
    rpod_mpd_t *mpd = ctx;
    rpod_mpd_state_t state;
    unsigned elapsed_ms, duration_ms;
    if (rpod_mpd_get_position(mpd, &state, &elapsed_ms, &duration_ms) &&
        (state == RPOD_MPD_STATE_PLAY || state == RPOD_MPD_STATE_PAUSE) &&
        elapsed_ms >= PREV_RESTART_MS) {
        rpod_mpd_seek(mpd, 0);
    } else {
        rpod_mpd_previous(mpd);
    }
    rpod_now_playing_refresh();
}

/* Next/Prev held: a silent scan through the song (ui/seek_control.h). */
static void on_seek(int dir, void *ctx)
{
    (void)ctx;
    if (dir != 0) {
        rpod_seek_control_start(dir);
    } else {
        rpod_seek_control_stop();
    }
}

/* A fast flick through a long list (ui/scrub.h) -- but not under a popup,
 * whose own list is what the wheel is turning. */
static bool on_scrub(int dir, void *ctx)
{
    (void)ctx;
    if (rpod_screen_stack_overlay(g_stack) != NULL) {
        return false;
    }
    return rpod_scrub(dir);
}

int rpod_app_run(const rpod_board_t *board, const rpod_app_config_t *cfg)
{
    lv_init();

    lv_display_t *disp = board->create_display();
    if (disp == NULL) {
        fprintf(stderr, "rpod: board '%s' failed to create a display\n", board->name);
        return 1;
    }
    g_board = board;
    g_disp = disp;

    rpod_mpd_t *mpd = rpod_mpd_connect(cfg->mpd_socket);
    if (mpd == NULL || !rpod_mpd_is_connected(mpd)) {
        fprintf(stderr, "rpod: couldn't connect to MPD at %s\n", cfg->mpd_socket);
        return 1;
    }

    /* Cover art fetches + decodes on its own thread and MPD connection
     * (ui/cover_cache.h). Needs lv_init() first -- it creates an lv_timer. */
    rpod_cover_cache_init(cfg->mpd_socket, cfg->cover_cache_dir);

    /* BlueZ over the system bus (audio/bluetooth.h). Never fails -- with no
     * bluetoothd, Settings > Bluetooth just says so. Needs lv_init() first
     * for its lv_timer. */
    rpod_bt_init(cfg->bt_default_agent);

    /* Per-device volume (audio/volume_memory.h): follows Bluetooth audio
     * devices connecting/disconnecting, so needs rpod_bt_init() first. */
    rpod_volume_memory_init(mpd, cfg->volume_state);

    /* AirPods' extras over AAP (audio/airpods.h): follows BlueZ for them,
     * and ducks through volume memory, so after both of the above. */
    rpod_airpods_init(mpd);

    /* NULL/unset token leaves scrobbling as an inert no-op (listenbrainz.h). */
    rpod_lb_t *lb = rpod_lb_init(cfg->listenbrainz_token, cfg->listenbrainz_queue);

    /* Needs lv_init() first -- rpod_scrobbler_create() creates an lv_timer. */
    rpod_scrobbler_create(mpd, lb, cfg->scrobbler_state);

    /* The status bar resolves its visualizer FIFO from RPOD_VIS_FIFO; seed it
     * from the board's default without clobbering a user-set value, so the
     * bar doesn't need a sim-vs-device parameter. */
    if (cfg->vis_fifo != NULL) {
        setenv("RPOD_VIS_FIFO", cfg->vis_fifo, 0);
    }

    rpod_input_actions_t actions = {
        .menu = on_menu,
        .play_pause = on_play_pause,
        .sleep = on_sleep,
        .next = on_next,
        .prev = on_prev,
        .seek = on_seek,
        .scrub = on_scrub,
        .wake = on_wake,
        .ctx = mpd,
    };
    g_input = rpod_input_create(&actions);
    board->create_input(g_input);
    rpod_backlight_init(g_input, on_backlight_off, NULL, cfg->backlight_state);
    rpod_sleep_timer_init(on_sleep, mpd);

    rpod_status_bar_create(disp, mpd);

    /* Above the bar, so after it; volume control shows on it. */
    rpod_hud_init();
    rpod_volume_control_init(mpd);
    rpod_seek_control_init(mpd);
    rpod_scrub_init();

    g_stack = rpod_screen_stack_create(rpod_input_indev(g_input));
    rpod_screen_stack_push(g_stack, rpod_main_menu_build, mpd, NULL);

    /* The AirPods' battery card opens as an overlay on the stack. */
    rpod_airpods_notify_init(g_stack);

    for (;;) {
        uint32_t idle_ms = lv_timer_handler();
        usleep(idle_ms * 1000);
    }

    return 0;
}
