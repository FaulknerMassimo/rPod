/*
 * Shared wire protocol between rpod-wheel (the privileged decoder daemon)
 * and its clients (the UI's indev binding in Phase 4, tools/wheel-test-client
 * for manual verification). See docs/PLAN.md §4.5.
 */

#ifndef RPOD_WHEEL_PROTOCOL_H
#define RPOD_WHEEL_PROTOCOL_H

#include <stdint.h>

/* Unix domain socket the daemon publishes decoded events on. */
#define RPOD_WHEEL_SOCK_PATH "/run/rpod/wheel.sock"

/* GPIO pin map — docs/PLAN.md §1.2. */
#define RPOD_WHEEL_CLOCK_PIN  23
#define RPOD_WHEEL_DATA_PIN   25
#define RPOD_WHEEL_HAPTIC_PIN 26
#define RPOD_WHEEL_HOLD_PIN   16

enum rpod_wheel_event_type {
    RPOD_WHEEL_EVENT_BUTTON = 0,
    RPOD_WHEEL_EVENT_WHEEL  = 1,
    RPOD_WHEEL_EVENT_TOUCH  = 2,
};

enum rpod_wheel_button {
    RPOD_WHEEL_BTN_CENTER = 0,
    RPOD_WHEEL_BTN_LEFT   = 1,
    RPOD_WHEEL_BTN_RIGHT  = 2,
    RPOD_WHEEL_BTN_UP     = 3,
    RPOD_WHEEL_BTN_DOWN   = 4,
};

/* Wire event, docs/PLAN.md §4.5 — do not reorder or resize; the daemon and
 * every client must agree on the exact byte layout. */
struct rpod_wheel_event {
    uint8_t  type;      /* rpod_wheel_event_type */
    uint8_t  code;      /* rpod_wheel_button for BUTTON events, else unused */
    int8_t   value;     /* press=1/release=0, touch=1/0, or wheel delta */
    uint8_t  _pad;
    uint32_t position;  /* absolute wheel position, 0-255 */
    uint64_t timestamp_us;
};

#endif /* RPOD_WHEEL_PROTOCOL_H */
