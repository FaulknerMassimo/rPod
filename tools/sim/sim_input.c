#include "sim_input.h"

#include "input/encoder.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    rpod_input_buttons_t buttons;
    bool prev_menu;
    bool prev_play_pause;
    bool prev_next;
    bool prev_prev;
} button_poll_state_t;

static void fire_on_edge(bool now, bool *prev, void (*handler)(void *), void *ctx)
{
    if (now && !*prev && handler != NULL) {
        handler(ctx);
    }
    *prev = now;
}

/* Menu/Play-Pause/Next/Prev: the four app-level buttons (docs/PLAN.md §8.2),
 * not part of the encoder pair. SDL_GetKeyboardState() returns a state
 * snapshot kept fresh by SDL_PumpEvents(), which the SDL window driver's own
 * event timer already calls every cycle when it drains the event queue with
 * SDL_PollEvent() -- reading the snapshot here doesn't steal events from that
 * queue. */
static void button_poll_cb(lv_timer_t *timer)
{
    button_poll_state_t *state = lv_timer_get_user_data(timer);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    fire_on_edge(keys[SDL_SCANCODE_M], &state->prev_menu, state->buttons.on_menu, state->buttons.ctx);
    fire_on_edge(keys[SDL_SCANCODE_SPACE], &state->prev_play_pause, state->buttons.on_play_pause,
                 state->buttons.ctx);
    fire_on_edge(keys[SDL_SCANCODE_N], &state->prev_next, state->buttons.on_next, state->buttons.ctx);
    fire_on_edge(keys[SDL_SCANCODE_P], &state->prev_prev, state->buttons.on_prev, state->buttons.ctx);
}

/* --- Wheel rotate/select (Left/Right/Enter) ------------------------------
 *
 * The tricky encoder read semantics live in src/input/encoder.c (shared with
 * the on-device backend). This just translates SDL key state into what that
 * expects: one rotation step per Left/Right key-down edge, and Enter reported
 * as a held level so a real press-and-hold reaches LVGL as a sustained press
 * (needed for the long-press gestures). Key auto-repeat isn't reproduced;
 * scroll acceleration is the real wheel's angular-velocity tracking
 * (docs/PLAN.md §8.2), not something this keyboard stand-in needs to fake. */
typedef struct {
    lv_indev_t *indev;
    bool prev_left;
    bool prev_right;
} sim_encoder_poll_t;

static void encoder_poll_cb(lv_timer_t *timer)
{
    sim_encoder_poll_t *p = lv_timer_get_user_data(timer);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    bool left = keys[SDL_SCANCODE_LEFT];
    bool right = keys[SDL_SCANCODE_RIGHT];
    bool enter_held = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];

    int dir = 0;
    if (left && !p->prev_left) {
        dir = -1;
    } else if (right && !p->prev_right) {
        dir = 1;
    }
    p->prev_left = left;
    p->prev_right = right;

    rpod_encoder_feed(p->indev, dir, enter_held);
}

lv_indev_t *rpod_sim_input_init(const rpod_input_buttons_t *buttons)
{
    lv_indev_t *indev = rpod_encoder_create();

    sim_encoder_poll_t *p = calloc(1, sizeof(*p));
    p->indev = indev;
    lv_timer_create(encoder_poll_cb, 30, p);

    button_poll_state_t *state = calloc(1, sizeof(*state));
    state->buttons = *buttons;
    lv_timer_create(button_poll_cb, 30, state);

    return indev;
}
