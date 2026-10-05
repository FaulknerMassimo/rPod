#include "sim_input.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdlib.h>

/* Every key below is read as a level from SDL_GetKeyboardState() -- a state
 * snapshot kept fresh by SDL_PumpEvents(), which the SDL window driver's own
 * event timer already calls every cycle when it drains the event queue with
 * SDL_PollEvent() -- so reading it here doesn't steal events from that queue.
 *
 * Not built on lv_sdl_keyboard_create() / a generic keypad driver: see the
 * CLAUDE.md note on lv_sdl_keyboard's uninitialised-key bug. The tricky
 * encoder read semantics live in src/input/encoder.c (shared with the
 * on-device backend), and taps vs holds in src/input/gestures.c; this only
 * turns key levels into button edges and rotation.
 *
 * Rotation is one step per Left/Right key-down edge. Key auto-repeat isn't
 * reproduced; scroll acceleration is the real wheel's angular-velocity
 * tracking (docs/PLAN.md §8.2), not something this keyboard stand-in needs
 * to fake -- Shift stands in for a flick fast enough to scrub letters. */

typedef struct {
    rpod_input_t *in;
    bool down[RPOD_BTN_COUNT];
    bool prev_left;
    bool prev_right;
} sim_input_t;

static void feed_button(sim_input_t *s, rpod_button_t btn, bool down)
{
    if (down != s->down[btn]) {
        s->down[btn] = down;
        rpod_input_button(s->in, btn, down);
    }
}

static void poll_cb(lv_timer_t *timer)
{
    sim_input_t *s = lv_timer_get_user_data(timer);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    feed_button(s, RPOD_BTN_MENU, keys[SDL_SCANCODE_M]);
    feed_button(s, RPOD_BTN_PLAY_PAUSE, keys[SDL_SCANCODE_SPACE]);
    feed_button(s, RPOD_BTN_NEXT, keys[SDL_SCANCODE_N]);
    feed_button(s, RPOD_BTN_PREV, keys[SDL_SCANCODE_P]);
    feed_button(s, RPOD_BTN_CENTER, keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]);

    bool left = keys[SDL_SCANCODE_LEFT];
    bool right = keys[SDL_SCANCODE_RIGHT];
    bool shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];

    int dir = 0;
    if (left && !s->prev_left) {
        dir = -1;
    } else if (right && !s->prev_right) {
        dir = 1;
    }
    s->prev_left = left;
    s->prev_right = right;

    if (dir != 0 && !(shift && rpod_input_scrub(s->in, dir))) {
        rpod_input_rotate(s->in, dir);
    }
}

void rpod_sim_input_init(rpod_input_t *in)
{
    sim_input_t *s = calloc(1, sizeof(*s));
    s->in = in;
    lv_timer_create(poll_cb, 30, s);
}
