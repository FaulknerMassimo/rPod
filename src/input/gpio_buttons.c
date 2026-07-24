#include "gpio_buttons.h"

#include "input/encoder.h"

#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>

/* Poll cadence. Fast enough that a press/scroll feels instant; the two-sample
 * debounce below then means a change must survive ~POLL_MS to count, which
 * settles the mechanical bounce on these small tactile switches. */
#define POLL_MS 15

typedef struct {
    struct gpiod_line *line; /* NULL when the pin is unused or its request failed */
    bool raw_prev;           /* last raw reading, for debounce */
    bool stable;             /* debounced pressed level */
    bool edge_prev;          /* debounced level at the previous poll, for edges */
} gline_t;

typedef struct {
    lv_indev_t *indev;
    rpod_input_buttons_t buttons;
    struct gpiod_chip *chip;
    /* Joystick + three keys. All active-low (pressed == 0). */
    gline_t up, down, left, right, center;
    gline_t key1, key2, key3;
} gpio_state_t;

/* Reads one line and returns its debounced pressed level. A reading only
 * updates `stable` once it has repeated (two consecutive equal samples), so a
 * single bouncing sample can't flip the state. */
static bool line_stable(gline_t *g)
{
    if (g->line == NULL) {
        return false;
    }
    int v = gpiod_line_get_value(g->line);
    bool raw = (v == 0); /* active-low: pressed pulls the line to ground */
    if (raw == g->raw_prev) {
        g->stable = raw;
    }
    g->raw_prev = raw;
    return g->stable;
}

/* True once per press: the debounced level rose since the previous poll. */
static bool line_edge(gline_t *g)
{
    bool p = line_stable(g);
    bool e = p && !g->edge_prev;
    g->edge_prev = p;
    return e;
}

/* The locked input map for the HAT (docs/PLAN.md multi-board notes):
 *   Joystick Up/Down -> scroll (encoder prev/next)
 *   Joystick Press   -> select (encoder ENTER, held so long-press gestures fire)
 *   Joystick Left    -> Previous       KEY1 -> Menu / back
 *   Joystick Right   -> Next           KEY2 -> Play / Pause
 *                                      KEY3 -> Next
 */
static void poll_cb(lv_timer_t *timer)
{
    gpio_state_t *s = lv_timer_get_user_data(timer);
    void *ctx = s->buttons.ctx;

    /* Center is a held *level* so a press-and-hold reaches LVGL as a sustained
     * press (LV_EVENT_LONG_PRESSED -> the like / add-to-playlist gestures). */
    bool center = line_stable(&s->center);

    /* Poll both rotation directions every tick so neither line's edge state
     * goes stale when the other fires. */
    bool up_e = line_edge(&s->up);
    bool down_e = line_edge(&s->down);
    int dir = up_e ? -1 : (down_e ? 1 : 0);
    rpod_encoder_feed(s->indev, dir, center);

    if (line_edge(&s->left) && s->buttons.on_prev) {
        s->buttons.on_prev(ctx);
    }
    if (line_edge(&s->right) && s->buttons.on_next) {
        s->buttons.on_next(ctx);
    }
    if (line_edge(&s->key1) && s->buttons.on_menu) {
        s->buttons.on_menu(ctx);
    }
    if (line_edge(&s->key2) && s->buttons.on_play_pause) {
        s->buttons.on_play_pause(ctx);
    }
    if (line_edge(&s->key3) && s->buttons.on_next) {
        s->buttons.on_next(ctx);
    }
}

/* Requests one line as an input with a pull-up bias. Returns NULL (a dead but
 * harmless line) for an unused pin or on any failure. */
static struct gpiod_line *request_line(struct gpiod_chip *chip, int offset)
{
    if (chip == NULL || offset < 0) {
        return NULL;
    }
    struct gpiod_line *line = gpiod_chip_get_line(chip, (unsigned int)offset);
    if (line == NULL) {
        return NULL;
    }
    if (gpiod_line_request_input_flags(line, "rpod", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        fprintf(stderr, "rpod: gpio line %d request failed\n", offset);
        return NULL;
    }
    return line;
}

lv_indev_t *rpod_gpio_input_init(const rpod_gpio_pinmap_t *pins,
                                 const rpod_input_buttons_t *buttons)
{
    lv_indev_t *indev = rpod_encoder_create();

    gpio_state_t *s = calloc(1, sizeof(*s));
    s->indev = indev;
    s->buttons = *buttons;

    const char *chip_path = (pins->chip != NULL) ? pins->chip : "/dev/gpiochip0";
    s->chip = gpiod_chip_open(chip_path);
    if (s->chip == NULL) {
        /* Display still comes up; input is dead until the chip is reachable
         * (permissions -- see rpod.service's SupplementaryGroups=gpio). */
        fprintf(stderr, "rpod: couldn't open gpio chip %s -- buttons disabled\n", chip_path);
        return indev;
    }

    s->up.line = request_line(s->chip, pins->up);
    s->down.line = request_line(s->chip, pins->down);
    s->left.line = request_line(s->chip, pins->left);
    s->right.line = request_line(s->chip, pins->right);
    s->center.line = request_line(s->chip, pins->center);
    s->key1.line = request_line(s->chip, pins->key1);
    s->key2.line = request_line(s->chip, pins->key2);
    s->key3.line = request_line(s->chip, pins->key3);

    lv_timer_create(poll_cb, POLL_MS, s);
    return indev;
}
