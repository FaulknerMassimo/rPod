/*
 * The games' best scores (Extras > Games), one "game score" line each in a
 * small text file so they survive restarts. Pure C: no LVGL.
 */

#ifndef RPOD_SCORES_H
#define RPOD_SCORES_H

#include <stdbool.h>

/* Loads them from state_path (parent directory must exist); NULL keeps
 * them in memory only. Call once. */
void rpod_scores_init(const char *state_path);

/* `game`'s best so far: 0 before its first game. */
unsigned rpod_score_best(const char *game);

/* A finished game's score. True if it beats the best, which it then
 * becomes (and is saved). */
bool rpod_score_submit(const char *game, unsigned score);

#endif /* RPOD_SCORES_H */
