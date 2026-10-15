#include "wheel_tuning.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The 4th-gen wheel has 96 positions per turn (docs/clickwheel-protocol.md),
 * so 6 per step is 16 rows a turn at slow speed (4 felt too sensitive on
 * hardware). A slow, deliberate turn measured ~35 positions/s; full
 * acceleration arrives at ~3 turns/s. 16 positions a letter is six letters
 * a turn. */
static const rpod_wheel_tuning_t k_defaults = {
    .step_ticks = 6.0f,
    .accel_v0 = 100.0f,
    .accel_v1 = 300.0f,
    .accel_max = 6.0f,
    .scrub_v = 0.0f,
    .scrub_ticks = 16.0f,
};

static const rpod_wheel_param_t k_params[] = {
    { "step_ticks",  "RPOD_WHEEL_STEP_TICKS",  1.0f, 24.0f },
    { "accel_v0",    "RPOD_WHEEL_ACCEL_V0",    10.0f, 1000.0f },
    { "accel_v1",    "RPOD_WHEEL_ACCEL_V1",    20.0f, 2000.0f },
    { "accel_max",   "RPOD_WHEEL_ACCEL_MAX",   1.0f, 20.0f },
    { "scrub_v",     "RPOD_WHEEL_SCRUB_V",     0.0f, 2000.0f },
    { "scrub_ticks", "RPOD_WHEEL_SCRUB_TICKS", 4.0f, 48.0f },
};

#define PARAM_COUNT (sizeof(k_params) / sizeof(k_params[0]))

static struct {
    rpod_wheel_tuning_t base; /* defaults + environment: what a reset restores */
    rpod_wheel_tuning_t cur;
    bool tuned[PARAM_COUNT];  /* set from the state file or by hand */
    char path[PATH_MAX];
} g;

/* k_params[i]'s field in t. Same order as the struct. */
static float *field(rpod_wheel_tuning_t *t, size_t i)
{
    float *f[] = { &t->step_ticks, &t->accel_v0, &t->accel_v1, &t->accel_max, &t->scrub_v, &t->scrub_ticks };
    return f[i];
}

static int find(const char *key)
{
    for (size_t i = 0; i < PARAM_COUNT; i++) {
        if (strcmp(key, k_params[i].key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool in_range(size_t i, float v)
{
    return v >= k_params[i].min && v <= k_params[i].max;
}

static void load(void)
{
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* nothing tuned yet */
    }
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char key[32];
        float v;
        if (sscanf(line, "%31[a-z0-9_]=%f", key, &v) != 2) {
            continue;
        }
        int i = find(key);
        if (i >= 0 && in_range((size_t)i, v)) {
            *field(&g.cur, (size_t)i) = v;
            g.tuned[i] = true;
        }
    }
    fclose(f);
}

/* Only what was tuned, so a later change to a default or the environment
 * still reaches the rest. Written to a temp file and renamed over the real
 * one, like the other state files. */
static void save(void)
{
    if (g.path[0] == '\0') {
        return;
    }
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: wheel tuning couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    for (size_t i = 0; i < PARAM_COUNT; i++) {
        if (g.tuned[i]) {
            fprintf(f, "%s=%g\n", k_params[i].key, (double)*field(&g.cur, i));
        }
    }
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: wheel tuning couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

void rpod_wheel_tuning_init(const char *state_path)
{
    g.base = k_defaults;
    for (size_t i = 0; i < PARAM_COUNT; i++) {
        const char *v = getenv(k_params[i].env);
        if (v == NULL || v[0] == '\0') {
            continue;
        }
        char *end;
        float f = strtof(v, &end);
        if (end != v && f > 0.0f && in_range(i, f)) {
            *field(&g.base, i) = f;
        }
    }
    g.cur = g.base;
    memset(g.tuned, 0, sizeof(g.tuned));
    g.path[0] = '\0';
    if (state_path != NULL && state_path[0] != '\0') {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
        load();
    }
}

const rpod_wheel_tuning_t *rpod_wheel_tuning(void)
{
    return &g.cur;
}

float rpod_wheel_tuning_scrub_v(void)
{
    return g.cur.scrub_v > 0.0f ? g.cur.scrub_v : g.cur.accel_v1;
}

const rpod_wheel_param_t *rpod_wheel_tuning_params(size_t *count)
{
    *count = PARAM_COUNT;
    return k_params;
}

bool rpod_wheel_tuning_get(const char *key, float *value, float *reset_value)
{
    int i = find(key);
    if (i < 0) {
        return false;
    }
    *value = *field(&g.cur, (size_t)i);
    *reset_value = *field(&g.base, (size_t)i);
    return true;
}

bool rpod_wheel_tuning_set(const char *key, float value)
{
    int i = find(key);
    if (i < 0 || !in_range((size_t)i, value)) {
        return false;
    }
    *field(&g.cur, (size_t)i) = value;
    g.tuned[i] = true;
    save();
    return true;
}

void rpod_wheel_tuning_reset(void)
{
    g.cur = g.base;
    memset(g.tuned, 0, sizeof(g.tuned));
    if (g.path[0] != '\0' && remove(g.path) != 0 && errno != ENOENT) {
        fprintf(stderr, "rpod: wheel tuning couldn't remove %s: %s\n", g.path, strerror(errno));
    }
}
