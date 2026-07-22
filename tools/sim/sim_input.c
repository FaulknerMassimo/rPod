#include "sim_input.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    rpod_sim_buttons_t buttons;
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

/* Menu/Play-Pause/Next/Prev: the click wheel's four physical buttons,
 * app-level shortcuts per docs/PLAN.md §8.2, not part of the encoder pair.
 * SDL_GetKeyboardState() returns a state snapshot kept fresh by
 * SDL_PumpEvents(), which the SDL window driver's own event timer already
 * calls every cycle when it drains the event queue with SDL_PollEvent() --
 * reading the snapshot here doesn't steal events from that queue. */
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
 * Deliberately NOT built on lv_sdl_keyboard_create(): its read callback
 * (sdl_keyboard_read() in third_party/lvgl/src/drivers/sdl/lv_sdl_keyboard.c)
 * sets data->state = LV_INDEV_STATE_RELEASED for the release half of a
 * keypress but never sets data->key, leaving it uninitialized.
 * indev_encoder_proc() checks `data->key == LV_KEY_ENTER` on release to
 * decide whether to fire a click -- with garbage in that field, it can
 * read as LV_KEY_ENTER, so *every* key release (including plain rotation)
 * can spuriously select whatever was just rotated onto. Confirmed by
 * instrumenting the actual LVGL source: rotating right correctly moved
 * focus one step, then the very next (buggy) release synthesized a click
 * into the new selection -- exactly the "left/right drills into the menu"
 * behavior seen in testing. This driver reports both halves of each press
 * explicitly instead, so data->key is always well-defined. */

typedef struct {
    bool prev_left;
    bool prev_right;
    uint32_t pending_key;  /* 0 = nothing queued (LEFT/RIGHT rotation step) */
    bool pending_release;  /* true once the rotation press half was reported */
    /* Enter (the centre/select button) is reported as a *level*, not a queued
     * edge, so a real press-and-hold reaches LVGL as a sustained press and
     * LV_EVENT_LONG_PRESSED can fire (needed for the press-and-hold gestures).
     * `enter_reported` remembers we owe a matching RELEASED once it lets go. */
    bool enter_held;
    bool enter_reported;
} encoder_poll_state_t;

/* Rotation stays edge-triggered (one press+release cycle per key-down, so one
 * step per tap); Enter is a sustained level. An in-flight rotation cycle is
 * always completed atomically before Enter is considered, so a press's key
 * never changes mid-hold. */
static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    encoder_poll_state_t *enc = lv_indev_get_driver_data(indev);

    if (enc->pending_release) {
        enc->pending_release = false;
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = enc->pending_key;
        enc->pending_key = 0;
        return;
    }

    if (enc->enter_held) {
        enc->enter_reported = true;
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = LV_KEY_ENTER;
        return;
    }
    if (enc->enter_reported) {
        enc->enter_reported = false;
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = LV_KEY_ENTER;
        return;
    }

    if (enc->pending_key != 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = enc->pending_key;
        enc->pending_release = true;
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;
}

/* Tracks Enter as a held level and queues one rotation step per Left/Right
 * key-down transition. Key auto-repeat isn't reproduced; scroll acceleration
 * is the real wheel's angular-velocity tracking (docs/PLAN.md §8.2), not
 * something this keyboard stand-in needs to fake. Only queues a new rotation
 * step once the previous one has been fully delivered, so steps can't overlap. */
static void encoder_poll_cb(lv_timer_t *timer)
{
    lv_indev_t *indev = lv_timer_get_user_data(timer);
    encoder_poll_state_t *enc = lv_indev_get_driver_data(indev);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    bool left = keys[SDL_SCANCODE_LEFT];
    bool right = keys[SDL_SCANCODE_RIGHT];

    enc->enter_held = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];

    if (enc->pending_key == 0) {
        if (left && !enc->prev_left) {
            enc->pending_key = LV_KEY_LEFT;
        } else if (right && !enc->prev_right) {
            enc->pending_key = LV_KEY_RIGHT;
        }
    }
    enc->prev_left = left;
    enc->prev_right = right;
}

lv_indev_t *rpod_sim_input_init(const rpod_sim_buttons_t *buttons)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);

    encoder_poll_state_t *enc = calloc(1, sizeof(*enc));
    lv_indev_set_driver_data(indev, enc);
    lv_indev_set_read_cb(indev, encoder_read_cb);
    /* indev's own built-in read timer (created by lv_indev_create()) drains
     * enc->pending_key via encoder_read_cb; this timer only fills it. */
    lv_timer_create(encoder_poll_cb, 30, indev);

    button_poll_state_t *state = calloc(1, sizeof(*state));
    state->buttons = *buttons;
    lv_timer_create(button_poll_cb, 30, state);

    return indev;
}
