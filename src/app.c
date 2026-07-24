#include "app.h"

#include "audio/listenbrainz.h"
#include "audio/mpd_client.h"
#include "audio/scrobbler.h"
#include "input/input.h"
#include "ui/metrics.h"
#include "ui/screens/main_menu.h"
#include "ui/screens/screen_stack.h"
#include "ui/status_bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* The screen stack the "Menu/back" button pops. One per process; kept static
 * for the button callbacks the same way tools/sim/sim_main.c used to. */
static rpod_screen_stack_t *g_stack;

/* The four app-level buttons (docs/PLAN.md §8.2). Menu pops the stack; the
 * transport three act on the MPD client passed as ctx. Which physical control
 * maps to each is the board's business (see the input backends). */
static void on_menu(void *ctx)
{
    (void)ctx;
    rpod_screen_stack_pop(g_stack);
}

static void on_play_pause(void *ctx)
{
    rpod_mpd_toggle_pause((rpod_mpd_t *)ctx);
}

static void on_next(void *ctx)
{
    rpod_mpd_next((rpod_mpd_t *)ctx);
}

static void on_prev(void *ctx)
{
    rpod_mpd_previous((rpod_mpd_t *)ctx);
}

int rpod_app_run(const rpod_board_t *board, const rpod_app_config_t *cfg)
{
    lv_init();
    rpod_metrics_init(board->form);

    lv_display_t *disp = board->create_display();
    if (disp == NULL) {
        fprintf(stderr, "rpod: board '%s' failed to create a display\n", board->name);
        return 1;
    }

    rpod_mpd_t *mpd = rpod_mpd_connect(cfg->mpd_socket);
    if (mpd == NULL || !rpod_mpd_is_connected(mpd)) {
        fprintf(stderr, "rpod: couldn't connect to MPD at %s\n", cfg->mpd_socket);
        return 1;
    }

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

    rpod_input_buttons_t buttons = {
        .on_menu = on_menu,
        .on_play_pause = on_play_pause,
        .on_next = on_next,
        .on_prev = on_prev,
        .ctx = mpd,
    };
    lv_indev_t *indev = board->create_input(&buttons);

    rpod_status_bar_create(disp, mpd);

    g_stack = rpod_screen_stack_create(indev);
    rpod_screen_stack_push(g_stack, rpod_main_menu_build, mpd, NULL);

    for (;;) {
        uint32_t idle_ms = lv_timer_handler();
        usleep(idle_ms * 1000);
    }

    return 0;
}
