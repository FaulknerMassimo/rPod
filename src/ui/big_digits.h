/*
 * Big numerals, for the clock face (ui/screens/clock_screens.h) and the
 * number spinners (ui/spinner.h): the iPod's own clock digits in the classic
 * themes when its bitmaps are built in (ui/classic.h), a 64 px Montserrat
 * otherwise. Every digit gets the same width, so a running clock doesn't
 * shuffle sideways as it ticks -- Montserrat's digits are proportional.
 */

#ifndef RPOD_BIG_DIGITS_H
#define RPOD_BIG_DIGITS_H

#include "lvgl.h"

#include <stdint.h>

lv_obj_t *rpod_big_digits_create(lv_obj_t *parent);

/* Shows `text`: digits and ':'. Anything else leaves a gap a digit wide. */
void rpod_big_digits_set_text(lv_obj_t *obj, const char *text);

void rpod_big_digits_set_color(lv_obj_t *obj, lv_color_t color);

/* How tall the numerals stand, and how wide `text` would be, in the active
 * theme. */
int32_t rpod_big_digits_height(void);
int32_t rpod_big_digits_width(const char *text);

#endif /* RPOD_BIG_DIGITS_H */
