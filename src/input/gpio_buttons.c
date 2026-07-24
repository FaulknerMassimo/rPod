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
    struct gpiod_line_request *req; /* NULL when the pin is unused or its request failed */
    unsigned int offset;            /* BCM offset this request covers */
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
    if (g->req == NULL) {
        return false;
    }
    enum gpiod_line_value v = gpiod_line_request_get_value(g->req, g->offset);
    bool raw = (v == GPIOD_LINE_VALUE_INACTIVE); /* active-low: pressed pulls the line to ground */
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

/* Requests one line as an input with a pull-up bias, recording the resulting
 * request handle + offset in `g`. Leaves g->req NULL (a dead but harmless line)
 * for an unused pin or on any failure. libgpiod v2 request-builder API: each
 * pin gets its own single-line request so one bad pin can't sink the rest. */
static void request_line(struct gpiod_chip *chip, gline_t *g, int offset)
{
    if (chip == NULL || offset < 0) {
        return;
    }

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (settings == NULL || line_cfg == NULL || req_cfg == NULL) {
        goto out;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

    unsigned int off = (unsigned int)offset;
    if (gpiod_line_config_add_line_settings(line_cfg, &off, 1, settings) < 0) {
        goto out;
    }
    gpiod_request_config_set_consumer(req_cfg, "rpod");

    struct gpiod_line_request *req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (req == NULL) {
        fprintf(stderr, "rpod: gpio line %d request failed\n", offset);
        goto out;
    }
    g->req = req;
    g->offset = off;

out:
    if (req_cfg != NULL) {
        gpiod_request_config_free(req_cfg);
    }
    if (line_cfg != NULL) {
        gpiod_line_config_free(line_cfg);
    }
    if (settings != NULL) {
        gpiod_line_settings_free(settings);
    }
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

    request_line(s->chip, &s->up, pins->up);
    request_line(s->chip, &s->down, pins->down);
    request_line(s->chip, &s->left, pins->left);
    request_line(s->chip, &s->right, pins->right);
    request_line(s->chip, &s->center, pins->center);
    request_line(s->chip, &s->key1, pins->key1);
    request_line(s->chip, &s->key2, pins->key2);
    request_line(s->chip, &s->key3, pins->key3);

    lv_timer_create(poll_cb, POLL_MS, s);
    return indev;
}
