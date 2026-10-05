/*
 * iPod-style alphabetical order for the browse lists, and the letter groups
 * the alphabet scrub jumps between (docs/PLAN.md §8.2, ui/scrub.h).
 *
 * A name files under the first letter of its first word that counts: leading
 * punctuation and a leading "The " are skipped ("The Beatles" is a B,
 * "(What's the Story)" a W), case and Latin accents fold away ("Édith" is an
 * E), and anything that doesn't start with a letter goes under '#', after Z.
 * rpod_alpha_compare() sorts by those same rules, so in a list sorted with it
 * every letter's entries sit together -- which is what rpod_alpha_jump()
 * relies on.
 *
 * Pure logic, no LVGL (tests/test_alpha_sort.c).
 */

#ifndef RPOD_ALPHA_SORT_H
#define RPOD_ALPHA_SORT_H

#include <stddef.h>

/* 'A'-'Z', or '#'. */
char rpod_alpha_letter(const char *name);

/* strcmp()-style, in the order above. Names that differ only in case or
 * accents still get a stable order (by their raw bytes). */
int rpod_alpha_compare(const char *a, const char *b);

/* The name of entry `i` of a list sorted with rpod_alpha_compare(). */
typedef const char *(*rpod_alpha_name_fn)(const void *ctx, size_t i);

/* From entry `cur` of a `count`-entry sorted list: the first entry of the
 * next letter (dir > 0), or of the previous one (dir < 0). Stays at `cur`
 * past the last letter; before the first, goes to the top of the list. */
size_t rpod_alpha_jump(rpod_alpha_name_fn name_at, const void *ctx, size_t count, size_t cur,
                       int dir);

#endif /* RPOD_ALPHA_SORT_H */
