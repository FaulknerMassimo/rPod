/*
 * Button gestures (docs/PLAN.md §8.2): turns raw presses and releases of the
 * five buttons into what they mean, the same for every input backend.
 *
 *   Center      a level, passed straight on to the encoder (LVGL times its
 *               own long press)
 *   Menu        acts on press -- it has no hold
 *   Play/Pause  tap toggles; held RPOD_GESTURE_SLEEP_HOLD_MS, sleeps
 *   Next/Prev   tap skips; held RPOD_GESTURE_SEEK_HOLD_MS, scans through
 *               the song until let go
 *
 * A button with a hold acts on release, so a tap can't also start the hold.
 * While asleep, a press of any button only wakes the device; that press and
 * its release go no further. A dim sleep -- the backlight timer's, only the
 * screen off -- is lighter: touching or turning the wheel wakes it too, and
 * Play/Pause, Next and Prev wake it *and* act, since they don't need the
 * screen (an iPod's work in a pocket). Centre and Menu act on what's on
 * screen, so they still only wake.
 *
 * Pure logic, clocked by the caller's millisecond tick, so it runs without
 * LVGL (tests/test_gestures.c); input.c drives it from an lv_timer.
 */

#ifndef RPOD_GESTURES_H
#define RPOD_GESTURES_H

#include <stdbool.h>
#include <stdint.h>

#define RPOD_GESTURE_SEEK_HOLD_MS   500
#define RPOD_GESTURE_SLEEP_HOLD_MS  1500

typedef enum {
    RPOD_BTN_CENTER,
    RPOD_BTN_MENU,
    RPOD_BTN_PLAY_PAUSE,
    RPOD_BTN_PREV,
    RPOD_BTN_NEXT,
    RPOD_BTN_COUNT,
} rpod_button_t;

/* What the buttons do -- the app's side (src/app.c). Any may be NULL. */
typedef struct {
    void (*menu)(void *ctx);
    void (*play_pause)(void *ctx);
    void (*sleep)(void *ctx);         /* Play/Pause held */
    void (*next)(void *ctx);
    void (*prev)(void *ctx);
    /* Next (+1) or Prev (-1) held long enough to scan, then 0 when it's let
     * go. The scan itself is the app's (ui/seek_control.h). */
    void (*seek)(int dir, void *ctx);
    /* Fast-scroll alphabet jump (ui/scrub.h): to the next (+1) or previous
     * (-1) letter of the current list, or 0 to just show the letter it's on.
     * False if the current screen has no letter index -- the backend then
     * scrolls rows as usual. Not a gesture: backends reach it through
     * rpod_input_scrub(). */
    bool (*scrub)(int dir, void *ctx);
    void (*wake)(void *ctx);          /* a press while asleep */
    void *ctx;
} rpod_input_actions_t;

typedef struct {
    bool down;
    bool swallow;  /* woke the device, or was down when it slept: ignore until let go */
    bool held;     /* its hold action has fired */
    uint32_t down_ms;
} rpod_gesture_btn_t;

typedef struct {
    rpod_input_actions_t actions;
    void (*center)(bool held, void *ctx); /* the select level, for the encoder */
    void *center_ctx;
    bool asleep;
    bool dim;      /* asleep is a dim sleep (see above) */
    rpod_gesture_btn_t btn[RPOD_BTN_COUNT];
} rpod_gestures_t;

void rpod_gestures_init(rpod_gestures_t *g, const rpod_input_actions_t *actions,
                        void (*center)(bool held, void *ctx), void *center_ctx);

/* A button went down or up. Repeats of its current state are ignored. */
void rpod_gestures_feed(rpod_gestures_t *g, rpod_button_t btn, bool pressed, uint32_t now_ms);

/* Fires holds that are due. Call every few tens of ms while
 * rpod_gestures_timing() says so. */
void rpod_gestures_tick(rpod_gestures_t *g, uint32_t now_ms);

/* True while a held button could still fire a hold. */
bool rpod_gestures_timing(const rpod_gestures_t *g);

/* Puts input to sleep -- a dim sleep if `dim` (see above), which a later
 * full sleep deepens: buttons already down are let go of without acting (a
 * seek gets its closing 0). Returns whether Center was down -- the caller
 * releases the encoder without a click. */
bool rpod_gestures_sleep(rpod_gestures_t *g, bool dim);

/* The wheel was touched or turned. True if a turn should move the
 * selection; false while asleep, when a dim sleep wakes on it (the actions'
 * `wake`) -- that turn still doesn't move anything. */
bool rpod_gestures_turn(rpod_gestures_t *g);

/* The backend lost its source (the wheel daemon went away): forget every
 * button without acting, as for sleep, but stay awake. Returns whether
 * Center was down. */
bool rpod_gestures_cancel(rpod_gestures_t *g);

#endif /* RPOD_GESTURES_H */
