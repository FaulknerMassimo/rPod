#include "gestures.h"

#include <string.h>

#define CALL(fn, ...) do { if (g->actions.fn != NULL) g->actions.fn(__VA_ARGS__); } while (0)

/* Wrap-safe "has `at` come yet" on a 32-bit millisecond tick. */
static bool due(uint32_t now_ms, uint32_t at_ms)
{
    return (int32_t)(now_ms - at_ms) >= 0;
}

static int seek_dir(rpod_button_t btn)
{
    return btn == RPOD_BTN_NEXT ? 1 : -1;
}

void rpod_gestures_init(rpod_gestures_t *g, const rpod_input_actions_t *actions,
                        void (*center)(bool held, void *ctx), void *center_ctx)
{
    memset(g, 0, sizeof(*g));
    g->actions = *actions;
    g->center = center;
    g->center_ctx = center_ctx;
}

void rpod_gestures_feed(rpod_gestures_t *g, rpod_button_t btn, bool pressed, uint32_t now_ms)
{
    if (btn >= RPOD_BTN_COUNT || g->btn[btn].down == pressed) {
        return;
    }
    rpod_gesture_btn_t *b = &g->btn[btn];
    b->down = pressed;

    if (pressed) {
        b->held = false;
        b->down_ms = now_ms;
        if (g->asleep) {
            g->asleep = false;
            b->swallow = true;
            CALL(wake, g->actions.ctx);
            return;
        }
        b->swallow = false;
        switch (btn) {
        case RPOD_BTN_CENTER:
            if (g->center != NULL) {
                g->center(true, g->center_ctx);
            }
            break;
        case RPOD_BTN_MENU:
            CALL(menu, g->actions.ctx);
            break;
        default:
            break;
        }
        return;
    }

    if (b->swallow) {
        b->swallow = false;
        return;
    }
    switch (btn) {
    case RPOD_BTN_CENTER:
        if (g->center != NULL) {
            g->center(false, g->center_ctx);
        }
        break;
    case RPOD_BTN_PLAY_PAUSE:
        if (!b->held) {
            CALL(play_pause, g->actions.ctx);
        }
        break;
    case RPOD_BTN_PREV:
    case RPOD_BTN_NEXT:
        if (b->held) {
            CALL(seek, 0, g->actions.ctx);
        } else if (btn == RPOD_BTN_NEXT) {
            CALL(next, g->actions.ctx);
        } else {
            CALL(prev, g->actions.ctx);
        }
        break;
    default:
        break;
    }
}

void rpod_gestures_tick(rpod_gestures_t *g, uint32_t now_ms)
{
    for (int i = 0; i < RPOD_BTN_COUNT; i++) {
        rpod_gesture_btn_t *b = &g->btn[i];
        if (!b->down || b->swallow) {
            continue;
        }
        if (i == RPOD_BTN_PLAY_PAUSE && !b->held &&
            due(now_ms, b->down_ms + RPOD_GESTURE_SLEEP_HOLD_MS)) {
            b->held = true;
            CALL(sleep, g->actions.ctx);
        } else if ((i == RPOD_BTN_PREV || i == RPOD_BTN_NEXT) && !b->held &&
                   due(now_ms, b->down_ms + RPOD_GESTURE_SEEK_HOLD_MS)) {
            b->held = true;
            CALL(seek, seek_dir((rpod_button_t)i), g->actions.ctx);
        }
    }
}

bool rpod_gestures_timing(const rpod_gestures_t *g)
{
    static const rpod_button_t k_with_hold[] = { RPOD_BTN_PLAY_PAUSE, RPOD_BTN_PREV, RPOD_BTN_NEXT };
    for (size_t i = 0; i < sizeof(k_with_hold) / sizeof(k_with_hold[0]); i++) {
        const rpod_gesture_btn_t *b = &g->btn[k_with_hold[i]];
        if (b->down && !b->swallow && !b->held) {
            return true;
        }
    }
    return false;
}

/* Every button that's down stops counting: its release will be swallowed. */
static bool let_go(rpod_gestures_t *g)
{
    bool center_down = false;
    for (int i = 0; i < RPOD_BTN_COUNT; i++) {
        rpod_gesture_btn_t *b = &g->btn[i];
        if (!b->down || b->swallow) {
            continue;
        }
        if ((i == RPOD_BTN_PREV || i == RPOD_BTN_NEXT) && b->held) {
            CALL(seek, 0, g->actions.ctx);
        }
        if (i == RPOD_BTN_CENTER) {
            center_down = true;
        }
        b->swallow = true;
    }
    return center_down;
}

bool rpod_gestures_sleep(rpod_gestures_t *g)
{
    g->asleep = true;
    return let_go(g);
}

bool rpod_gestures_cancel(rpod_gestures_t *g)
{
    bool center_down = let_go(g);
    /* The source is gone, so no release is coming to clear them. */
    for (int i = 0; i < RPOD_BTN_COUNT; i++) {
        g->btn[i].down = false;
        g->btn[i].swallow = false;
    }
    return center_down;
}
