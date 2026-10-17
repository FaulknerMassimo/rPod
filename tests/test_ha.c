/*
 * Home Assistant (src/ha/): the JSON reader, the server address's
 * normalisation, entity ids, the state parser and what Center does to each
 * kind of entity. Host-built: `make test`.
 */

#include "ha/ha.h"
#include "ha/json_read.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static rpod_jv_t *parse(const char *s)
{
    return rpod_jv_parse(s, strlen(s));
}

static void test_json(void)
{
    rpod_jv_t *v = parse(" {\"a\": 1.5, \"b\": [true, false, null, \"x\"], \"c\": {\"d\": \"e\"}, \"n\": -2e2} ");
    CHECK(v != NULL);
    CHECK(rpod_jv_type(v) == RPOD_JV_OBJ);
    CHECK(rpod_jv_num(rpod_jv_get(v, "a"), 0) == 1.5);
    CHECK(rpod_jv_num(rpod_jv_get(v, "n"), 0) == -200);
    const rpod_jv_t *b = rpod_jv_get(v, "b");
    CHECK(rpod_jv_len(b) == 4);
    CHECK(rpod_jv_bool(rpod_jv_at(b, 0), false));
    CHECK(!rpod_jv_bool(rpod_jv_at(b, 1), true));
    CHECK(rpod_jv_type(rpod_jv_at(b, 2)) == RPOD_JV_NULL);
    CHECK(strcmp(rpod_jv_str(rpod_jv_at(b, 3), "?"), "x") == 0);
    CHECK(rpod_jv_at(b, 4) == NULL);
    CHECK(strcmp(rpod_jv_str(rpod_jv_get(rpod_jv_get(v, "c"), "d"), "?"), "e") == 0);
    /* Wrong type or absent: the default. */
    CHECK(strcmp(rpod_jv_str(rpod_jv_get(v, "a"), "def"), "def") == 0);
    CHECK(rpod_jv_num(rpod_jv_get(v, "zz"), 7) == 7);
    CHECK(!rpod_jv_is_num(rpod_jv_get(v, "c")));
    rpod_jv_free(v);

    /* Escapes: \n, \u00e9, and a surrogate pair (U+1F4A1). */
    v = parse("\"a\\n\\u00e9\\ud83d\\udca1\\\"\"");
    CHECK(v != NULL && strcmp(rpod_jv_str(v, ""), "a\n\xc3\xa9\xf0\x9f\x92\xa1\"") == 0);
    rpod_jv_free(v);
    /* A lone surrogate becomes U+FFFD rather than invalid UTF-8. */
    v = parse("\"\\ud83d\"");
    CHECK(v != NULL && strcmp(rpod_jv_str(v, ""), "\xef\xbf\xbd") == 0);
    rpod_jv_free(v);

    CHECK(parse("") == NULL);
    CHECK(parse("{") == NULL);
    CHECK(parse("{\"a\":}") == NULL);
    CHECK(parse("[1,]") == NULL);
    CHECK(parse("[1] x") == NULL);
    CHECK(parse("nan") == NULL);
    CHECK(parse("0x10") == NULL);
    CHECK(parse("\"a\nb\"") == NULL); /* a raw control character */
    CHECK(parse("\"\\q\"") == NULL);
    CHECK(parse("{\"a\" 1}") == NULL);

    /* Nesting is capped. */
    char deep[200];
    memset(deep, '[', 100);
    memset(deep + 100, ']', 100);
    deep[200 - 1] = ']';
    CHECK(rpod_jv_parse(deep, 200) == NULL);

    /* A truncated reply leaks nothing (ASan checks) and fails. */
    const char *whole = "[{\"entity_id\":\"light.a\",\"attributes\":{\"x\":[1,2,{\"y\":\"zzz\"}]}}]";
    for (size_t n = 0; n < strlen(whole); n++) {
        CHECK(rpod_jv_parse(whole, n) == NULL);
    }
    v = parse(whole);
    CHECK(v != NULL);
    rpod_jv_free(v);
}

static void test_url(void)
{
    char out[200];
    CHECK(rpod_ha_normalize_url("http://homeassistant.local:8123", out, sizeof(out)));
    CHECK(strcmp(out, "http://homeassistant.local:8123") == 0);
    CHECK(rpod_ha_normalize_url("  192.168.1.5:8123/  ", out, sizeof(out)));
    CHECK(strcmp(out, "http://192.168.1.5:8123") == 0);
    CHECK(rpod_ha_normalize_url("HTTPS://ha.example.com//", out, sizeof(out)));
    CHECK(strcmp(out, "https://ha.example.com") == 0);
    CHECK(rpod_ha_normalize_url("https://example.com/ha", out, sizeof(out)));
    CHECK(strcmp(out, "https://example.com/ha") == 0);

    CHECK(!rpod_ha_normalize_url("", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("http://", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("ftp://host", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("file:///etc/passwd", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("http://user:pw@host", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("http://host/?x=1", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("http://ho st", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("http://host\r\nX: y", out, sizeof(out)));
    CHECK(!rpod_ha_normalize_url("/just/a/path", out, sizeof(out)));
    char tiny[8];
    CHECK(!rpod_ha_normalize_url("http://homeassistant.local", tiny, sizeof(tiny)));
}

static void test_ids(void)
{
    CHECK(rpod_ha_valid_id("light.kitchen"));
    CHECK(rpod_ha_valid_id("input_boolean.guest_mode_2"));
    CHECK(!rpod_ha_valid_id("light"));
    CHECK(!rpod_ha_valid_id(".kitchen"));
    CHECK(!rpod_ha_valid_id("light."));
    CHECK(!rpod_ha_valid_id("light.a.b"));
    CHECK(!rpod_ha_valid_id("Light.Kitchen"));
    CHECK(!rpod_ha_valid_id("light.kit\"chen"));
    CHECK(!rpod_ha_valid_id("light.kit/chen"));
    CHECK(!rpod_ha_valid_id("light.kit chen"));
    char longid[100];
    memset(longid, 'a', sizeof(longid));
    memcpy(longid, "light.", 6);
    longid[64] = '\0';
    CHECK(!rpod_ha_valid_id(longid)); /* 64 bytes */
    longid[63] = '\0';
    CHECK(rpod_ha_valid_id(longid));
}

static const char *STATES =
    "[{\"entity_id\":\"light.living\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"Living Room\","
    "\"brightness\":128,\"supported_color_modes\":[\"color_temp\",\"xy\"]}},"
    "{\"entity_id\":\"light.hall\",\"state\":\"off\",\"attributes\":{\"friendly_name\":\"hall\","
    "\"brightness\":null,\"supported_color_modes\":[\"onoff\"]}},"
    "{\"entity_id\":\"climate.house\",\"state\":\"heat\",\"attributes\":{\"friendly_name\":\"House\","
    "\"temperature\":21.5,\"current_temperature\":19}},"
    "{\"entity_id\":\"sensor.temp\",\"state\":\"18.2\",\"attributes\":{\"unit_of_measurement\":\"\\u00b0C\"}},"
    "{\"entity_id\":\"lock.front\",\"state\":\"locked\",\"attributes\":{}},"
    "{\"entity_id\":\"cover.garage\",\"state\":\"closed\",\"attributes\":{}},"
    "{\"entity_id\":\"scene.movie\",\"state\":\"2026-01-01T00:00:00+00:00\",\"attributes\":{}},"
    "{\"entity_id\":\"switch.bad\\\"id\",\"state\":\"on\",\"attributes\":{}},"
    "{\"state\":\"on\"}]";

static const rpod_ha_entity_t *by_id(const rpod_ha_entity_t *e, size_t n, const char *id)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(e[i].id, id) == 0) {
            return &e[i];
        }
    }
    return NULL;
}

static void test_states(void)
{
    rpod_ha_entity_t *e;
    size_t n;
    CHECK(rpod_ha_parse_states(STATES, strlen(STATES), &e, &n));
    CHECK(n == 7); /* the bad id and the one without an id are left out */

    /* Sorted by name, case-insensitively: hall, House, Living Room... */
    CHECK(strcmp(e[0].name, "cover.garage") == 0); /* no friendly_name: the id */
    CHECK(strcmp(e[1].name, "hall") == 0);
    CHECK(strcmp(e[2].name, "House") == 0);

    const rpod_ha_entity_t *l = by_id(e, n, "light.living");
    CHECK(l != NULL && strcmp(l->domain, "light") == 0);
    CHECK(l->dimmable && l->brightness == 50); /* 128/255 */
    const rpod_ha_entity_t *h = by_id(e, n, "light.hall");
    CHECK(h != NULL && !h->dimmable && h->brightness == -1);

    const rpod_ha_entity_t *c = by_id(e, n, "climate.house");
    CHECK(c != NULL && c->has_target && c->target == 21.5 && c->has_current && c->current == 19);

    const rpod_ha_entity_t *s = by_id(e, n, "sensor.temp");
    CHECK(s != NULL && strcmp(s->unit, "\xc2\xb0" "C") == 0 && strcmp(s->state, "18.2") == 0);

    /* What Center does. */
    CHECK(strcmp(rpod_ha_action_label(l), "Turn Off") == 0);
    CHECK(strcmp(rpod_ha_action_label(h), "Turn On") == 0);
    CHECK(strcmp(rpod_ha_action_label(by_id(e, n, "lock.front")), "Unlock") == 0);
    CHECK(strcmp(rpod_ha_action_label(by_id(e, n, "cover.garage")), "Open") == 0);
    CHECK(strcmp(rpod_ha_action_label(by_id(e, n, "scene.movie")), "Activate") == 0);
    CHECK(!rpod_ha_actionable(s));
    CHECK(!rpod_ha_actionable(c));

    rpod_ha_entity_t gone = *l;
    snprintf(gone.state, sizeof(gone.state), "unavailable");
    CHECK(!rpod_ha_actionable(&gone));
    free(e);

    CHECK(!rpod_ha_parse_states("{}", 2, &e, &n));
    CHECK(!rpod_ha_parse_states("[{", 2, &e, &n));
    CHECK(rpod_ha_parse_states("[]", 2, &e, &n) && n == 0);
    free(e);

    rpod_ha_entity_t one;
    const char *st = "{\"entity_id\":\"switch.fan\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"Fan\"}}";
    CHECK(rpod_ha_parse_state(st, strlen(st), &one) && strcmp(one.id, "switch.fan") == 0);
    CHECK(!rpod_ha_parse_state("[]", 2, &one));
}

int main(void)
{
    test_json();
    test_url();
    test_ids();
    test_states();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_ha: ok\n");
    return 0;
}
