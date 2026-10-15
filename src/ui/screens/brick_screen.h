/*
 * Extras > Games > Brick: the game (games/brick.h) on screen. The wheel
 * slides the paddle -- in input.h's fine rotation, a wheel position at a
 * time -- and Center serves, pauses and plays on. It pauses itself whenever
 * it's out of sight: the screen dark, or a card over it.
 */

#ifndef RPOD_BRICK_SCREEN_H
#define RPOD_BRICK_SCREEN_H

#include "screen_stack.h"

/* Pushes a new game. */
void rpod_brick_screen_push(rpod_screen_stack_t *stack);

#endif /* RPOD_BRICK_SCREEN_H */
