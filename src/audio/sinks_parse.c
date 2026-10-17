#include "sinks_parse.h"

#include "ha/json_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* The node.name of the default sink: pw-dump shows it in the "default"
 * metadata object, as {"key": "default.audio.sink", "value": {"name": ...}}. */
static const char *default_sink_name(const rpod_jv_t *objects)
{
    for (size_t i = 0; i < rpod_jv_len(objects); i++) {
        const rpod_jv_t *o = rpod_jv_at(objects, i);
        if (strcmp(rpod_jv_str(rpod_jv_get(o, "type"), ""), "PipeWire:Interface:Metadata") != 0) {
            continue;
        }
        const rpod_jv_t *entries = rpod_jv_get(o, "metadata");
        for (size_t k = 0; k < rpod_jv_len(entries); k++) {
            const rpod_jv_t *e = rpod_jv_at(entries, k);
            if (strcmp(rpod_jv_str(rpod_jv_get(e, "key"), ""), "default.audio.sink") == 0) {
                const rpod_jv_t *value = rpod_jv_get(e, "value");
                /* An object, or the same JSON as text on an older PipeWire. */
                if (rpod_jv_type(value) == RPOD_JV_OBJ) {
                    return rpod_jv_str(rpod_jv_get(value, "name"), NULL);
                }
                return NULL;
            }
        }
    }
    return NULL;
}

static int by_kind_label(const void *a, const void *b)
{
    const rpod_sink_t *x = a, *y = b;
    if (x->kind != y->kind) {
        return x->kind < y->kind ? -1 : 1;
    }
    return strcmp(x->label, y->label);
}

size_t rpod_sinks_parse(const char *json, size_t len, rpod_sink_t *out, size_t max)
{
    rpod_jv_t *root = rpod_jv_parse(json, len);
    if (root == NULL || rpod_jv_type(root) != RPOD_JV_ARR) {
        rpod_jv_free(root);
        return 0;
    }
    const char *def = default_sink_name(root);

    size_t n = 0;
    for (size_t i = 0; i < rpod_jv_len(root) && n < max; i++) {
        const rpod_jv_t *o = rpod_jv_at(root, i);
        if (strcmp(rpod_jv_str(rpod_jv_get(o, "type"), ""), "PipeWire:Interface:Node") != 0) {
            continue;
        }
        const rpod_jv_t *props = rpod_jv_get(rpod_jv_get(o, "info"), "props");
        if (strcmp(rpod_jv_str(rpod_jv_get(props, "media.class"), ""), "Audio/Sink") != 0) {
            continue;
        }
        const char *name = rpod_jv_str(rpod_jv_get(props, "node.name"), "");
        rpod_sink_kind_t kind;
        if (starts_with(name, "bluez_output.")) {
            kind = RPOD_SINK_BLUETOOTH;
        } else {
            continue;
        }
        const char *label = rpod_jv_str(rpod_jv_get(props, "node.description"), "");
        if (label[0] == '\0') {
            label = rpod_jv_str(rpod_jv_get(props, "node.nick"), name);
        }
        rpod_sink_t *s = &out[n++];
        memset(s, 0, sizeof(*s));
        s->id = (unsigned)rpod_jv_num(rpod_jv_get(o, "id"), 0);
        s->kind = kind;
        snprintf(s->name, sizeof(s->name), "%s", name);
        snprintf(s->label, sizeof(s->label), "%s", label);
        s->current = def != NULL && strcmp(def, name) == 0;
    }
    rpod_jv_free(root);
    qsort(out, n, sizeof(*out), by_kind_label);
    return n;
}

size_t rpod_sinks_parse_owntone(const char *json, size_t len, rpod_sink_t *out, size_t max)
{
    rpod_jv_t *root = rpod_jv_parse(json, len);
    const rpod_jv_t *outputs = rpod_jv_get(root, "outputs");
    size_t n = 0;
    for (size_t i = 0; i < rpod_jv_len(outputs) && n < max; i++) {
        const rpod_jv_t *o = rpod_jv_at(outputs, i);
        const char *id = rpod_jv_str(rpod_jv_get(o, "id"), "");
        const char *name = rpod_jv_str(rpod_jv_get(o, "name"), "");
        const char *type = rpod_jv_str(rpod_jv_get(o, "type"), "");
        bool digits = id[0] != '\0';
        for (const char *c = id; *c != '\0'; c++) {
            digits = digits && *c >= '0' && *c <= '9';
        }
        /* AirPlay and AirPlay 2 receivers; not OwnTone's own local audio. */
        if (!digits || name[0] == '\0' || strlen(id) >= sizeof(out->ref) || !starts_with(type, "AirPlay")) {
            continue;
        }
        rpod_sink_t *s = &out[n++];
        memset(s, 0, sizeof(*s));
        s->kind = RPOD_SINK_AIRPLAY;
        snprintf(s->ref, sizeof(s->ref), "%s", id);
        snprintf(s->name, sizeof(s->name), "%s", name);
        snprintf(s->label, sizeof(s->label), "%s", name);
        s->current = rpod_jv_bool(rpod_jv_get(o, "selected"), false);
    }
    rpod_jv_free(root);
    qsort(out, n, sizeof(*out), by_kind_label);
    return n;
}
