#include "json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rpod_json_init(rpod_json_t *j)
{
    memset(j, 0, sizeof(*j));
    j->first[0] = true;
}

void rpod_json_free(rpod_json_t *j)
{
    free(j->s);
    memset(j, 0, sizeof(*j));
}

static void put(rpod_json_t *j, const char *s, size_t n)
{
    if (j->oom) {
        return;
    }
    if (j->len + n + 1 > j->cap) {
        size_t cap = j->cap == 0 ? 1024 : j->cap;
        while (j->len + n + 1 > cap) {
            cap *= 2;
        }
        char *grown = realloc(j->s, cap);
        if (grown == NULL) {
            j->oom = true;
            return;
        }
        j->s = grown;
        j->cap = cap;
    }
    memcpy(j->s + j->len, s, n);
    j->len += n;
    j->s[j->len] = '\0';
}

static void puts_(rpod_json_t *j, const char *s)
{
    put(j, s, strlen(s));
}

/* Before any value: a comma if it isn't the first at its level, unless it
 * follows its key. */
static void sep(rpod_json_t *j)
{
    if (j->after_key) {
        j->after_key = false;
        return;
    }
    if (!j->first[j->depth]) {
        put(j, ",", 1);
    }
    j->first[j->depth] = false;
}

static void open_(rpod_json_t *j, char c)
{
    sep(j);
    put(j, &c, 1);
    if (j->depth + 1 < RPOD_JSON_MAX_DEPTH) {
        j->depth++;
        j->first[j->depth] = true;
    } else {
        j->oom = true; /* too deep to keep track: refuse to emit garbage */
    }
}

static void close_(rpod_json_t *j, char c)
{
    if (j->depth > 0) {
        j->depth--;
    }
    put(j, &c, 1);
}

void rpod_json_obj(rpod_json_t *j) { open_(j, '{'); }
void rpod_json_end_obj(rpod_json_t *j) { close_(j, '}'); }
void rpod_json_arr(rpod_json_t *j) { open_(j, '['); }
void rpod_json_end_arr(rpod_json_t *j) { close_(j, ']'); }

/* Length of the valid UTF-8 sequence at p, or 0 if there isn't one. */
static size_t utf8_seq(const unsigned char *p)
{
    unsigned c = p[0];
    if (c < 0x80) {
        return 1;
    }
    size_t n;
    uint32_t cp;
    if ((c & 0xe0) == 0xc0) {
        n = 2;
        cp = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
        n = 3;
        cp = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            return 0; /* includes hitting the NUL */
        }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    static const uint32_t min_cp[] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (cp < min_cp[n] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        return 0;
    }
    return n;
}

bool rpod_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p != '\0') {
        size_t n = utf8_seq(p);
        if (n == 0) {
            return false;
        }
        p += n;
    }
    return true;
}

static void put_escaped(rpod_json_t *j, const char *v)
{
    put(j, "\"", 1);
    const unsigned char *p = (const unsigned char *)(v != NULL ? v : "");
    while (*p != '\0') {
        unsigned c = *p;
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            put(j, e, 2);
            p++;
        } else if (c < 0x20) {
            char e[8];
            switch (c) {
            case '\n': puts_(j, "\\n"); break;
            case '\r': puts_(j, "\\r"); break;
            case '\t': puts_(j, "\\t"); break;
            default:
                snprintf(e, sizeof(e), "\\u%04x", c);
                puts_(j, e);
                break;
            }
            p++;
        } else if (c < 0x80) {
            put(j, (const char *)p, 1);
            p++;
        } else {
            size_t n = utf8_seq(p);
            if (n == 0) {
                puts_(j, "\xef\xbf\xbd"); /* U+FFFD for the stray byte */
                p++;
            } else {
                put(j, (const char *)p, n);
                p += n;
            }
        }
    }
    put(j, "\"", 1);
}

void rpod_json_key(rpod_json_t *j, const char *key)
{
    sep(j);
    put_escaped(j, key);
    put(j, ":", 1);
    j->after_key = true;
}

void rpod_json_str(rpod_json_t *j, const char *v)
{
    sep(j);
    put_escaped(j, v);
}

void rpod_json_int(rpod_json_t *j, long long v)
{
    char b[32];
    snprintf(b, sizeof(b), "%lld", v);
    sep(j);
    puts_(j, b);
}

void rpod_json_num(rpod_json_t *j, double v)
{
    char b[48];
    if (!isfinite(v)) {
        snprintf(b, sizeof(b), "null");
    } else {
        snprintf(b, sizeof(b), "%.10g", v);
    }
    sep(j);
    puts_(j, b);
}

void rpod_json_bool(rpod_json_t *j, bool v)
{
    sep(j);
    puts_(j, v ? "true" : "false");
}

void rpod_json_null(rpod_json_t *j)
{
    sep(j);
    puts_(j, "null");
}

void rpod_json_kstr(rpod_json_t *j, const char *key, const char *v)
{
    rpod_json_key(j, key);
    rpod_json_str(j, v);
}

void rpod_json_kint(rpod_json_t *j, const char *key, long long v)
{
    rpod_json_key(j, key);
    rpod_json_int(j, v);
}

void rpod_json_knum(rpod_json_t *j, const char *key, double v)
{
    rpod_json_key(j, key);
    rpod_json_num(j, v);
}

void rpod_json_kbool(rpod_json_t *j, const char *key, bool v)
{
    rpod_json_key(j, key);
    rpod_json_bool(j, v);
}
