#include "encoder.h"

#include <stdlib.h>

/* State shared between the read callback (drains it) and rpod_encoder_feed()
 * (fills it). Formerly encoder_poll_state_t in tools/sim/sim_input.c. */
typedef struct {
    uint32_t pending_key;  /* 0 = nothing queued; else LV_KEY_LEFT / LV_KEY_RIGHT */
    bool pending_release;  /* true once the rotation press half was reported */
    /* Enter (the centre/select button) is reported as a *level*, not a queued
     * edge, so a real press-and-hold reaches LVGL as a sustained press and
     * LV_EVENT_LONG_PRESSED can fire (needed for the press-and-hold gestures).
     * `enter_reported` remembers we owe a matching RELEASED once it lets go. */
    bool enter_held;
    bool enter_reported;
} encoder_state_t;

/* Not built on lv_sdl_keyboard_create() / a generic keypad driver: LVGL's
 * indev_encoder_proc() checks `data->key == LV_KEY_ENTER` on release to
 * decide whether to fire a click, and a driver that leaves data->key
 * uninitialised on the release half (as the vendored SDL keyboard driver
 * does -- see CLAUDE.md) can synthesise a spurious select on every plain
 * rotation. This callback reports both halves of each press with data->key
 * always set explicitly instead.
 *
 * Rotation stays edge-triggered (one press+release cycle per queued step, so
 * one step per detent); Enter is a sustained level. An in-flight rotation
 * cycle is always completed atomically before Enter is considered, so a
 * press's key never changes mid-hold. */
static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    encoder_state_t *enc = lv_indev_get_driver_data(indev);

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

lv_indev_t *rpod_encoder_create(void)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);

    encoder_state_t *enc = calloc(1, sizeof(*enc));
    lv_indev_set_driver_data(indev, enc);
    lv_indev_set_read_cb(indev, encoder_read_cb);
    return indev;
}

void rpod_encoder_feed(lv_indev_t *indev, int dir, bool enter_held)
{
    encoder_state_t *enc = lv_indev_get_driver_data(indev);

    enc->enter_held = enter_held;

    /* Only queue a new step once the previous one has been fully delivered,
     * so steps can't overlap (matches the old encoder_poll_cb gating). */
    if (enc->pending_key == 0) {
        if (dir < 0) {
            enc->pending_key = LV_KEY_LEFT;
        } else if (dir > 0) {
            enc->pending_key = LV_KEY_RIGHT;
        }
    }
}
