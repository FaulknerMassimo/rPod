/*
 * "Play On": where the sound goes -- the Bluetooth headset or an AirPlay
 * speaker on the network (audio/sinks.h). A glass popup (sheet.h) opened by
 * holding Play/Pause on Now Playing.
 */

#ifndef RPOD_OUTPUT_SCREENS_H
#define RPOD_OUTPUT_SCREENS_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_open_overlay(); takes no ctx. */
void rpod_output_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_OUTPUT_SCREENS_H */
