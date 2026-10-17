#include "json_read.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 24

struct rpod_jv {
    rpod_jv_type_t type;
    bool b;
    double n;
    char *s;           /* STR: the text; OBJ members: their key lives in `key` */
    char *key;         /* set on an object's members */
    struct rpod_jv *items; /* ARR/OBJ: array of children */
    size_t count, cap;
};

typedef struct {
    const char *p, *end;
    int depth;
} reader_t;

static void free_children(rpod_jv_t *v)
{
    for (size_t i = 0; i < v->count; i++) {
        rpod_jv_t *c = &v->items[i];
        free(c->s);
        free(c->key);
        free_children(c);
    }
    free(v->items);
    v->items = NULL;
    v->count = v->cap = 0;
}

void rpod_jv_free(rpod_jv_t *v)
{
    if (v == NULL) {
        return;
    }
    free_children(v);
    free(v->s);
    free(v->key);
    free(v);
}

static void skip_ws(reader_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

static size_t put_utf8(char *out, unsigned cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* After the opening quote: the unescaped string, malloc'd, or NULL. */
static char *read_string(reader_t *r)
{
    /* Unescaping never grows the text, so the raw length is enough. */
    size_t cap = (size_t)(r->end - r->p) + 1;
    char *out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    size_t n = 0;
    while (r->p < r->end) {
        unsigned char c = (unsigned char)*r->p++;
        if (c == '"') {
            out[n] = '\0';
            return out;
        }
        if (c < 0x20) {
            break; /* a raw control character */
        }
        if (c != '\\') {
            out[n++] = (char)c;
            continue;
        }
        if (r->p >= r->end) {
            break;
        }
        char e = *r->p++;
        switch (e) {
        case '"': case '\\': case '/': out[n++] = e; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'n': out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'u': {
            if (r->end - r->p < 4) {
                goto bad;
            }
            int hi = hex4(r->p);
            if (hi < 0) {
                goto bad;
            }
            r->p += 4;
            unsigned cp = (unsigned)hi;
            if (cp >= 0xD800 && cp < 0xDC00) {
                /* A high surrogate needs its low half. */
                if (r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                    int lo = hex4(r->p + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)lo - 0xDC00);
                        r->p += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                cp = 0xFFFD;
            }
            n += put_utf8(out + n, cp);
            break;
        }
        default:
            goto bad;
        }
    }
bad:
    free(out);
    return NULL;
}

static bool push_child(rpod_jv_t *parent, rpod_jv_t *child)
{
    if (parent->count == parent->cap) {
        size_t cap = parent->cap ? parent->cap * 2 : 4;
        rpod_jv_t *grown = realloc(parent->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        parent->items = grown;
        parent->cap = cap;
    }
    parent->items[parent->count++] = *child; /* the child's pointers move with it */
    return true;
}

static bool parse_value(reader_t *r, rpod_jv_t *out);

static bool parse_container(reader_t *r, rpod_jv_t *out, bool obj)
{
    if (++r->depth > MAX_DEPTH) {
        return false;
    }
    out->type = obj ? RPOD_JV_OBJ : RPOD_JV_ARR;
    char close = obj ? '}' : ']';
    skip_ws(r);
    if (r->p < r->end && *r->p == close) {
        r->p++;
        r->depth--;
        return true;
    }
    for (;;) {
        rpod_jv_t child = { 0 };
        skip_ws(r);
        if (obj) {
            if (r->p >= r->end || *r->p != '"') {
                return false;
            }
            r->p++;
            child.key = read_string(r);
            if (child.key == NULL) {
                return false;
            }
            skip_ws(r);
            if (r->p >= r->end || *r->p != ':') {
                free(child.key);
                return false;
            }
            r->p++;
        }
        if (!parse_value(r, &child)) {
            free(child.key);
            return false;
        }
        if (!push_child(out, &child)) {
            free(child.key);
            free(child.s);
            free_children(&child);
            return false;
        }
        skip_ws(r);
        if (r->p >= r->end) {
            return false;
        }
        if (*r->p == ',') {
            r->p++;
            continue;
        }
        if (*r->p == close) {
            r->p++;
            r->depth--;
            return true;
        }
        return false;
    }
}

static bool literal(reader_t *r, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, word, n) != 0) {
        return false;
    }
    r->p += n;
    return true;
}

/* Fills `out` (zeroed by the caller); on failure frees what it built. */
static bool parse_value(reader_t *r, rpod_jv_t *out)
{
    skip_ws(r);
    if (r->p >= r->end) {
        return false;
    }
    bool ok;
    switch (*r->p) {
    case '{':
        r->p++;
        ok = parse_container(r, out, true);
        break;
    case '[':
        r->p++;
        ok = parse_container(r, out, false);
        break;
    case '"':
        r->p++;
        out->type = RPOD_JV_STR;
        out->s = read_string(r);
        ok = out->s != NULL;
        break;
    case 't':
        out->type = RPOD_JV_BOOL;
        out->b = true;
        ok = literal(r, "true");
        break;
    case 'f':
        out->type = RPOD_JV_BOOL;
        ok = literal(r, "false");
        break;
    case 'n':
        out->type = RPOD_JV_NULL;
        ok = literal(r, "null");
        break;
    default: {
        /* strtod would also take "nan", "inf" and hex: only JSON's own
         * number characters get that far. */
        const char *q = r->p;
        if (*q == '-') {
            q++;
        }
        if (q >= r->end || *q < '0' || *q > '9') {
            ok = false;
            break;
        }
        char buf[40];
        size_t n = 0;
        const char *s = r->p;
        while (s < r->end && n < sizeof(buf) - 1 &&
               ((*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.' || *s == 'e' || *s == 'E')) {
            buf[n++] = *s++;
        }
        buf[n] = '\0';
        char *endp;
        double d = strtod(buf, &endp);
        if (*endp != '\0' || !isfinite(d)) {
            ok = false;
            break;
        }
        out->type = RPOD_JV_NUM;
        out->n = d;
        r->p = s;
        ok = true;
        break;
    }
    }
    if (!ok) {
        free_children(out);
        free(out->s);
        out->s = NULL;
    }
    return ok;
}

rpod_jv_t *rpod_jv_parse(const char *text, size_t len)
{
    rpod_jv_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        return NULL;
    }
    reader_t r = { .p = text, .end = text + len };
    if (!parse_value(&r, v)) {
        free(v);
        return NULL;
    }
    skip_ws(&r);
    if (r.p != r.end) {
        rpod_jv_free(v);
        return NULL;
    }
    return v;
}

rpod_jv_type_t rpod_jv_type(const rpod_jv_t *v)
{
    return v != NULL ? v->type : RPOD_JV_NULL;
}

const rpod_jv_t *rpod_jv_get(const rpod_jv_t *v, const char *key)
{
    if (v == NULL || v->type != RPOD_JV_OBJ) {
        return NULL;
    }
    for (size_t i = 0; i < v->count; i++) {
        if (strcmp(v->items[i].key, key) == 0) {
            return &v->items[i];
        }
    }
    return NULL;
}

size_t rpod_jv_len(const rpod_jv_t *v)
{
    return v != NULL && (v->type == RPOD_JV_ARR || v->type == RPOD_JV_OBJ) ? v->count : 0;
}

const rpod_jv_t *rpod_jv_at(const rpod_jv_t *v, size_t i)
{
    return i < rpod_jv_len(v) ? &v->items[i] : NULL;
}

const char *rpod_jv_str(const rpod_jv_t *v, const char *def)
{
    return v != NULL && v->type == RPOD_JV_STR ? v->s : def;
}

double rpod_jv_num(const rpod_jv_t *v, double def)
{
    return v != NULL && v->type == RPOD_JV_NUM ? v->n : def;
}

bool rpod_jv_bool(const rpod_jv_t *v, bool def)
{
    return v != NULL && v->type == RPOD_JV_BOOL ? v->b : def;
}

bool rpod_jv_is_num(const rpod_jv_t *v)
{
    return v != NULL && v->type == RPOD_JV_NUM;
}
