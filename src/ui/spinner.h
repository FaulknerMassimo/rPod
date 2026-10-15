/*
 * Numbers set with the wheel a field at a time, the iPod's way (its alarm
 * time view, re/ipod4/SPEC.md §5.4: an arrow above and below the number
 * being set): turning steps the field with the arrows, wrapping around at
 * either end, and Center moves on to the next -- after the last one, the
 * spinner is done. Clock > Alarm Clock > Time sets hours and minutes with
 * one, Screen Lock a combination's four digits.
 *
 * The numbers live in the caller's rpod_spinner_t -- a screen's push ctx, so
 * a theme change rebuilds the spinner where it was (screen_stack.h).
 */

#ifndef RPOD_SPINNER_H
#define RPOD_SPINNER_H

#include "lvgl.h"

#include <stdbool.h>

#define RPOD_SPINNER_MAX_FIELDS 4

typedef struct {
    int min, max; /* inclusive */
    int value;
    int digits;   /* shown zero-padded to this many */
} rpod_spinner_field_t;

typedef struct {
    rpod_spinner_field_t field[RPOD_SPINNER_MAX_FIELDS];
    int count;
    int active; /* the field being set */
    bool time;  /* a ':' between the fields, rather than a gap */
    bool mask;  /* fields already set show as a dot (a combination) */
} rpod_spinner_t;

/* Builds the spinner for `sp` into `parent` (place it with the usual
 * lv_obj calls) and gives it the wheel: it joins the default group --
 * the screen's -- focused, in edit mode, so it should be the screen's only
 * focusable widget. done(ctx) runs on Center at the last field, and may
 * delete the spinner (popping its screen, say). */
lv_obj_t *rpod_spinner_create(lv_obj_t *parent, rpod_spinner_t *sp, void (*done)(void *ctx), void *ctx);

/* Shows what's in its rpod_spinner_t now, after the caller changed it. */
void rpod_spinner_refresh(lv_obj_t *spinner);

/* A quick shake, side to side: a wrong combination. */
void rpod_spinner_shake(lv_obj_t *spinner);

#endif /* RPOD_SPINNER_H */
