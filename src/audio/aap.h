/*
 * Apple Accessory Protocol (AAP, a.k.a. AACP): the control channel AirPods
 * (and Beats) speak to Apple devices on L2CAP PSM 0x1001, beside the A2DP
 * audio link. It carries what plain Bluetooth audio doesn't: per-bud battery,
 * in-ear detection, noise control, Conversation Awareness, stem presses, and
 * the AirPods' own settings (docs/PLAN.md §6.3).
 *
 * Encode/decode only, no I/O -- audio/airpods.c owns the socket. Written from
 * LibrePods' protocol notes (github.com/kavishdevar/librepods, docs/), not
 * from its GPL-3.0 code.
 *
 * Every packet after the handshake starts 04 00 04 00, then a 16-bit
 * little-endian opcode, then the opcode's payload.
 */

#ifndef RPOD_AAP_H
#define RPOD_AAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RPOD_AAP_PSM  0x1001
/* Service UUID in the device's SDP record -- how BlueZ tells us it speaks AAP. */
#define RPOD_AAP_UUID "74ec2172-0bad-4d01-8f77-997b2be0722a"

/* Room for the longest packet the builders below produce. */
#define RPOD_AAP_MAX_PACKET 16

/* Control command identifiers (opcode 0x0009) rPod reads or sets. Each
 * carries up to four value bytes; only the first matters for these. Two-state
 * settings use RPOD_AAP_ON / RPOD_AAP_OFF. */
enum {
    RPOD_AAP_CTL_EAR_DETECTION     = 0x0A, /* on/off */
    RPOD_AAP_CTL_LISTENING_MODE    = 0x0D, /* rpod_aap_mode_t */
    RPOD_AAP_CTL_PRESS_SPEED       = 0x17, /* 0 default, 1 slower, 2 slowest */
    RPOD_AAP_CTL_HOLD_DURATION     = 0x18, /* 0 default, 1 slower, 2 slowest */
    RPOD_AAP_CTL_HOLD_CYCLE        = 0x1A, /* RPOD_AAP_MODE_BIT()s press-and-hold cycles through */
    RPOD_AAP_CTL_ONE_BUD_ANC       = 0x1B, /* on/off: noise cancellation with one AirPod */
    RPOD_AAP_CTL_SWIPE_SPEED       = 0x23, /* 1 default, 2 longer, 3 longest */
    RPOD_AAP_CTL_VOLUME_SWIPE      = 0x25, /* on/off */
    RPOD_AAP_CTL_PERSONAL_VOLUME   = 0x26, /* on/off ("Adaptive Volume") */
    RPOD_AAP_CTL_CONVERSATION      = 0x28, /* on/off: Conversation Awareness */
    RPOD_AAP_CTL_ADAPTIVE_STRENGTH = 0x2E, /* 0 (most outside noise) - 100 (least) */
    RPOD_AAP_CTL_ALLOW_OFF         = 0x34, /* on/off: "Off" offered as a listening mode */
    RPOD_AAP_CTL_RAW_GESTURES      = 0x39, /* RPOD_AAP_GESTURE_*: presses reported as opcode 0x19 */
};

#define RPOD_AAP_ON  0x01
#define RPOD_AAP_OFF 0x02

typedef enum {
    RPOD_AAP_MODE_OFF          = 1,
    RPOD_AAP_MODE_ANC          = 2,
    RPOD_AAP_MODE_TRANSPARENCY = 3,
    RPOD_AAP_MODE_ADAPTIVE     = 4,
} rpod_aap_mode_t;

/* A listening mode's bit in RPOD_AAP_CTL_HOLD_CYCLE. */
#define RPOD_AAP_MODE_BIT(mode) (1u << ((mode) - 1))

/* RPOD_AAP_CTL_RAW_GESTURES bits. */
#define RPOD_AAP_GESTURE_SINGLE 0x01
#define RPOD_AAP_GESTURE_DOUBLE 0x02
#define RPOD_AAP_GESTURE_TRIPLE 0x04
#define RPOD_AAP_GESTURE_LONG   0x08

/* Battery report components and statuses. */
enum {
    RPOD_AAP_BATT_SINGLE = 0x01, /* one-piece headphones (AirPods Max) */
    RPOD_AAP_BATT_RIGHT  = 0x02,
    RPOD_AAP_BATT_LEFT   = 0x04,
    RPOD_AAP_BATT_CASE   = 0x08,
};
enum {
    RPOD_AAP_BATT_CHARGING     = 0x01,
    RPOD_AAP_BATT_DISCHARGING  = 0x02,
    RPOD_AAP_BATT_DISCONNECTED = 0x04, /* not reporting (out of range, lid shut) */
};

/* In-ear report states, per bud. */
enum {
    RPOD_AAP_EAR_IN      = 0x00,
    RPOD_AAP_EAR_OUT     = 0x01,
    RPOD_AAP_EAR_IN_CASE = 0x02,
};

/* Stem press report types (not the same numbers as the gesture bits). */
enum {
    RPOD_AAP_PRESS_SINGLE = 0x05,
    RPOD_AAP_PRESS_DOUBLE = 0x06,
    RPOD_AAP_PRESS_TRIPLE = 0x07,
    RPOD_AAP_PRESS_LONG   = 0x08,
};

typedef enum {
    RPOD_AAP_EV_NONE = 0,     /* not one rPod uses (or malformed) */
    RPOD_AAP_EV_BATTERY,
    RPOD_AAP_EV_EAR,
    RPOD_AAP_EV_CONTROL,      /* a setting's current value -- also the echo of one we set */
    RPOD_AAP_EV_CONVERSATION, /* Conversation Awareness wants the volume at `level` */
    RPOD_AAP_EV_INFO,
    RPOD_AAP_EV_PRESS,
} rpod_aap_event_type_t;

#define RPOD_AAP_MAX_BATTERIES 4

typedef struct {
    rpod_aap_event_type_t type;
    union {
        struct {
            size_t count;
            struct {
                uint8_t component; /* RPOD_AAP_BATT_LEFT... */
                uint8_t level;     /* percent */
                uint8_t status;    /* RPOD_AAP_BATT_CHARGING... */
            } items[RPOD_AAP_MAX_BATTERIES];
        } battery;
        struct {
            /* The primary bud carries the microphone; which one that is can
             * swap when one comes out. */
            uint8_t primary, secondary; /* RPOD_AAP_EAR_* */
        } ear;
        struct {
            uint8_t id;
            uint8_t value[4];
        } control;
        /* 1-2: wearer started talking, duck hard; 3-7: easing back up;
         * 8-9: back to normal. */
        uint8_t conversation_level;
        struct {
            char name[64];
            char model[16];    /* e.g. "A3048" */
            char serial[32];
            char firmware[48];
        } info;
        struct {
            uint8_t type; /* RPOD_AAP_PRESS_* */
            uint8_t bud;  /* 1 left, 2 right */
        } press;
    };
} rpod_aap_event_t;

/* Packet builders. Each writes into `out` (RPOD_AAP_MAX_PACKET bytes) and
 * returns the length to send. Connection setup is handshake, then feature
 * flags (unlocks Adaptive and Conversation Awareness during playback), then
 * the notification request, after which the AirPods report their battery,
 * in-ear state and every setting's current value. */
size_t rpod_aap_build_handshake(uint8_t *out);
size_t rpod_aap_build_features(uint8_t *out);
size_t rpod_aap_build_notifications(uint8_t *out);
size_t rpod_aap_build_control(uint8_t *out, uint8_t id, uint8_t value);

/* Decodes one received packet (L2CAP SEQPACKET keeps their boundaries).
 * Returns false -- out->type RPOD_AAP_EV_NONE -- for anything else. */
bool rpod_aap_parse(const uint8_t *pkt, size_t len, rpod_aap_event_t *out);

#endif /* RPOD_AAP_H */
