#include "encoder.h"

#include <stdlib.h>

/* State shared between the read callback (drains it) and rpod_encoder_feed()
 * (fills it). Formerly encoder_poll_state_t in tools/sim/sim_input.c. */
typedef struct {
    /* Rotation not yet delivered: >0 = next, <0 = previous. Accumulates, so a
     * fast click-wheel flick that covers several rows between two reads
     * arrives as one multi-step enc_diff rather than being throttled to one
     * step per read. */
    int32_t steps;
    /* Enter (the centre/select button) is reported as a *level*, not a queued
     * edge, so a real press-and-hold reaches LVGL as a sustained press and
     * LV_EVENT_LONG_PRESSED can fire (needed for the press-and-hold gestures).
     * `enter_latched` makes sure a tap that goes down and up again between
     * two reads still reaches LVGL as one press; `enter_reported` remembers
     * we owe a matching RELEASED once it lets go. */
    bool enter_held;
    bool enter_latched;
    bool enter_reported;
} encoder_state_t;

/* Not built on lv_sdl_keyboard_create() / a generic keypad driver: LVGL's
 * indev_encoder_proc() checks `data->key == LV_KEY_ENTER` on release to
 * decide whether to fire a click, and a driver that leaves data->key
 * uninitialised on the release half (as the vendored SDL keyboard driver
 * does -- see CLAUDE.md) can synthesise a spurious select on every plain
 * rotation. This callback sets data->key explicitly on every read instead.
 *
 * Rotation goes out as enc_diff on a RELEASED read -- LVGL only honours
 * enc_diff while released, and moves focus (or, in edit mode, sends
 * LV_KEY_LEFT/RIGHT) once per step. Enter is a sustained level; its release
 * always gets a read of its own before any rotation, so a press's key never
 * changes mid-hold. Rotation while Enter is held is dropped, as LVGL would
 * anyway. */
static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    encoder_state_t *enc = lv_indev_get_driver_data(indev);

    if (enc->enter_held || enc->enter_latched) {
        enc->enter_latched = false;
        enc->enter_reported = true;
        enc->steps = 0;
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

    int32_t diff = enc->steps;
    if (diff > INT16_MAX) {
        diff = INT16_MAX;
    } else if (diff < INT16_MIN) {
        diff = INT16_MIN;
    }
    enc->steps -= diff;

    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;
    data->enc_diff = (int16_t)diff;
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

void rpod_encoder_feed(lv_indev_t *indev, int steps, bool enter_held)
{
    encoder_state_t *enc = lv_indev_get_driver_data(indev);

    if (enter_held && !enc->enter_held) {
        enc->enter_latched = true;
    }
    enc->enter_held = enter_held;
    enc->steps += steps;
}
