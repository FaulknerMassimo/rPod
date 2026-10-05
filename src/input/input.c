#include "input.h"

#include "input/encoder.h"

#include <stdlib.h>

/* How often held buttons are checked for a hold (gestures.h's timings are
 * multiples of it). Paused while nothing that has a hold is down. */
#define HOLD_TICK_MS 50

struct rpod_input {
    lv_indev_t *indev;
    rpod_gestures_t gestures;
    lv_timer_t *hold_timer;
    bool center_held; /* the level the encoder was last given */
    uint32_t last_input_ms;
};

static void center_level(bool held, void *ctx)
{
    rpod_input_t *in = ctx;
    in->center_held = held;
    rpod_encoder_feed(in->indev, 0, held);
}

/* Center was down when input stopped counting it: let the encoder go
 * without the release reading as a click. */
static void drop_center(rpod_input_t *in)
{
    in->center_held = false;
    lv_indev_wait_release(in->indev);
    rpod_encoder_feed(in->indev, 0, false);
}

static void hold_tick_cb(lv_timer_t *timer)
{
    rpod_input_t *in = lv_timer_get_user_data(timer);
    rpod_gestures_tick(&in->gestures, lv_tick_get());
    if (!rpod_gestures_timing(&in->gestures)) {
        lv_timer_pause(timer);
    }
}

rpod_input_t *rpod_input_create(const rpod_input_actions_t *actions)
{
    rpod_input_t *in = calloc(1, sizeof(*in));
    in->indev = rpod_encoder_create();
    rpod_gestures_init(&in->gestures, actions, center_level, in);
    in->hold_timer = lv_timer_create(hold_tick_cb, HOLD_TICK_MS, in);
    lv_timer_pause(in->hold_timer);
    in->last_input_ms = lv_tick_get();
    return in;
}

lv_indev_t *rpod_input_indev(const rpod_input_t *in)
{
    return in->indev;
}

void rpod_input_button(rpod_input_t *in, rpod_button_t btn, bool pressed)
{
    in->last_input_ms = lv_tick_get();
    rpod_gestures_feed(&in->gestures, btn, pressed, lv_tick_get());
    if (rpod_gestures_timing(&in->gestures)) {
        lv_timer_resume(in->hold_timer);
    }
}

void rpod_input_rotate(rpod_input_t *in, int steps)
{
    if (steps == 0) {
        return;
    }
    in->last_input_ms = lv_tick_get();
    if (rpod_gestures_turn(&in->gestures)) {
        rpod_encoder_feed(in->indev, steps, in->center_held);
    }
}

void rpod_input_touch(rpod_input_t *in)
{
    in->last_input_ms = lv_tick_get();
    rpod_gestures_turn(&in->gestures);
}

bool rpod_input_scrub(rpod_input_t *in, int dir)
{
    if (in->gestures.asleep || in->gestures.actions.scrub == NULL) {
        return false;
    }
    in->last_input_ms = lv_tick_get();
    lv_indev_read(in->indev);
    return in->gestures.actions.scrub(dir, in->gestures.actions.ctx);
}

void rpod_input_release_all(rpod_input_t *in)
{
    if (rpod_gestures_cancel(&in->gestures)) {
        drop_center(in);
    }
}

void rpod_input_sleep(rpod_input_t *in, bool dim)
{
    if (rpod_gestures_sleep(&in->gestures, dim)) {
        drop_center(in);
    }
}

bool rpod_input_asleep(const rpod_input_t *in)
{
    return in->gestures.asleep;
}

uint32_t rpod_input_idle_ms(const rpod_input_t *in)
{
    for (int i = 0; i < RPOD_BTN_COUNT; i++) {
        if (in->gestures.btn[i].down) {
            return 0;
        }
    }
    return lv_tick_elaps(in->last_input_ms);
}
