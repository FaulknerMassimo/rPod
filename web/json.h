/*
 * A JSON writer for the web daemon's API responses: values appended to a
 * growing buffer, commas and nesting tracked for the caller. Strings are
 * escaped, and bytes that aren't valid UTF-8 (a filename can be anything)
 * become U+FFFD, so the output always parses.
 *
 * Pure logic (tests/test_web.c).
 */

#ifndef RPOD_WEB_JSON_H
#define RPOD_WEB_JSON_H

#include <stdbool.h>
#include <stddef.h>

#define RPOD_JSON_MAX_DEPTH 16

typedef struct {
    char *s;
    size_t len, cap;
    bool oom; /* an allocation failed: the output is incomplete */
    int depth;
    bool first[RPOD_JSON_MAX_DEPTH]; /* nothing written at that level yet */
    bool after_key;
} rpod_json_t;

void rpod_json_init(rpod_json_t *j);
void rpod_json_free(rpod_json_t *j);

void rpod_json_obj(rpod_json_t *j);
void rpod_json_end_obj(rpod_json_t *j);
void rpod_json_arr(rpod_json_t *j);
void rpod_json_end_arr(rpod_json_t *j);

void rpod_json_key(rpod_json_t *j, const char *key);
void rpod_json_str(rpod_json_t *j, const char *v);
void rpod_json_int(rpod_json_t *j, long long v);
void rpod_json_num(rpod_json_t *j, double v);
void rpod_json_bool(rpod_json_t *j, bool v);
void rpod_json_null(rpod_json_t *j);

/* key + value. */
void rpod_json_kstr(rpod_json_t *j, const char *key, const char *v);
void rpod_json_kint(rpod_json_t *j, const char *key, long long v);
void rpod_json_knum(rpod_json_t *j, const char *key, double v);
void rpod_json_kbool(rpod_json_t *j, const char *key, bool v);

/* Whether `s` is entirely valid UTF-8 (no overlongs or surrogates). */
bool rpod_utf8_valid(const char *s);

#endif /* RPOD_WEB_JSON_H */
