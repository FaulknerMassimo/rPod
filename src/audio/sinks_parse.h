/*
 * Where the sound can go (docs/PLAN.md §6.3b): a connected Bluetooth headset,
 * read from `pw-dump`'s JSON (it's a PipeWire sink, and MPD plays to the
 * default one), and the AirPlay speakers OwnTone has found, read from its
 * /api/outputs.
 *
 * Pure logic (tests/test_sinks.c); audio/sinks.h runs it.
 */

#ifndef RPOD_SINKS_PARSE_H
#define RPOD_SINKS_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#define RPOD_SINK_MAX 16

typedef enum {
    RPOD_SINK_BLUETOOTH,
    RPOD_SINK_AIRPLAY,
} rpod_sink_kind_t;

typedef struct {
    unsigned id;      /* Bluetooth: PipeWire's node id, for `wpctl set-default` */
    char ref[24];     /* AirPlay: OwnTone's output id (digits) */
    char name[128];   /* node.name: stable across reconnects */
    char label[96];   /* what to call it on screen */
    rpod_sink_kind_t kind;
    bool current;     /* where the sound is going */
} rpod_sink_t;

/* The Bluetooth sinks in `json[0..len)` (pw-dump's whole array), at most
 * `max`; `current` is the PipeWire default. Returns how many, or 0 for
 * something that isn't pw-dump's output. */
size_t rpod_sinks_parse(const char *json, size_t len, rpod_sink_t *out, size_t max);

/* The AirPlay speakers in OwnTone's /api/outputs reply, at most `max`, by
 * label; `current` is "selected". Returns how many. */
size_t rpod_sinks_parse_owntone(const char *json, size_t len, rpod_sink_t *out, size_t max);

#endif /* RPOD_SINKS_PARSE_H */
