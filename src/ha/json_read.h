/*
 * A small JSON reader for Home Assistant's REST replies (ha.h): the text
 * parsed into a tree of values the caller walks and frees. Strings are
 * unescaped to UTF-8 (\u escapes and surrogate pairs included); nesting is
 * capped so a hostile reply can't blow the stack.
 *
 * Pure logic (tests/test_ha.c).
 */

#ifndef RPOD_HA_JSON_READ_H
#define RPOD_HA_JSON_READ_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    RPOD_JV_NULL,
    RPOD_JV_BOOL,
    RPOD_JV_NUM,
    RPOD_JV_STR,
    RPOD_JV_ARR,
    RPOD_JV_OBJ,
} rpod_jv_type_t;

typedef struct rpod_jv rpod_jv_t;

/* The first value in text[0..len), or NULL if it isn't valid JSON (or has
 * trailing junk). Free with rpod_jv_free(). */
rpod_jv_t *rpod_jv_parse(const char *text, size_t len);
void rpod_jv_free(rpod_jv_t *v);

rpod_jv_type_t rpod_jv_type(const rpod_jv_t *v);

/* An object's member, or NULL (also for a NULL or non-object `v`). */
const rpod_jv_t *rpod_jv_get(const rpod_jv_t *v, const char *key);

/* An array's (or object's) element count, and the i-th element; NULL out of
 * range or for something else. */
size_t rpod_jv_len(const rpod_jv_t *v);
const rpod_jv_t *rpod_jv_at(const rpod_jv_t *v, size_t i);

/* The value as a string/number/bool, or `def` for NULL or the wrong type. */
const char *rpod_jv_str(const rpod_jv_t *v, const char *def);
double rpod_jv_num(const rpod_jv_t *v, double def);
bool rpod_jv_bool(const rpod_jv_t *v, bool def);

/* Whether `v` is a number (so a missing attribute differs from a 0). */
bool rpod_jv_is_num(const rpod_jv_t *v);

#endif /* RPOD_HA_JSON_READ_H */
