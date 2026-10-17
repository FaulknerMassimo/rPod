#include "ha.h"

#include "json_read.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* The biggest reply read: a big installation's /api/states is a few MB. */
#define MAX_REPLY (8u * 1024 * 1024)

#define URL_MAX   200
#define TOKEN_MIN 16
#define TOKEN_MAX 500

/* Actions waiting for the worker; more than this are dropped (a person
 * can't turn the wheel that fast). */
#define JOBS_MAX 16

/* How long after an action the worker asks the server how it went: devices
 * report their new state a moment late. */
#define CONFIRM_DELAY_MS 350

typedef struct {
    char id[64];
    char service[24];
    char arg_key[24]; /* "" -> no argument */
    double arg;
    /* The state to show until the server confirms it ("" -> leave it) */
    char optimistic[32];
} job_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;

    char state_path[PATH_MAX];
    char url[URL_MAX + 1];
    char token[TOKEN_MAX + 1];
    unsigned gen; /* bumped when the server or token changes: older replies are stale */

    rpod_ha_state_t st;
    char err[96];
    char server_name[64];
    char server_version[24];
    bool fahrenheit;
    bool have_info;
    char failure[96];
    bool failed;

    rpod_ha_entity_t *ents;
    size_t nents;
    unsigned version;

    bool refresh_pending;
    job_t jobs[JOBS_MAX];
    size_t njobs; /* a queue from jobs[0] */
} g = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

/* --- Pure helpers ---------------------------------------------------------- */

bool rpod_ha_normalize_url(const char *in, char *out, size_t out_size)
{
    while (isspace((unsigned char)*in)) {
        in++;
    }
    size_t n = strlen(in);
    while (n > 0 && isspace((unsigned char)in[n - 1])) {
        n--;
    }
    char tmp[URL_MAX + 16];
    if (n == 0 || n > URL_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        /* No spaces or controls, no credentials in the address, and no
         * query or fragment to confuse the paths added to it. */
        if (c <= ' ' || c >= 0x7f || c == '@' || c == '?' || c == '#' || c == '\\' || c == '"') {
            return false;
        }
    }
    const char *scheme = "";
    if (strncasecmp(in, "http://", 7) == 0) {
        scheme = "http://";
        in += 7;
        n -= 7;
    } else if (strncasecmp(in, "https://", 8) == 0) {
        scheme = "https://";
        in += 8;
        n -= 8;
    } else if (memchr(in, ':', n) != NULL && n >= 3 && strncmp(memchr(in, ':', n), "://", 3) == 0) {
        return false; /* some other scheme */
    } else {
        scheme = "http://";
    }
    while (n > 0 && in[n - 1] == '/') {
        n--;
    }
    if (n == 0 || in[0] == '/' || in[0] == ':') {
        return false; /* no host */
    }
    snprintf(tmp, sizeof(tmp), "%s%.*s", scheme, (int)n, in);
    if (strlen(tmp) >= out_size) {
        return false;
    }
    strcpy(out, tmp);
    return true;
}

bool rpod_ha_valid_id(const char *id)
{
    size_t n = strlen(id);
    if (n < 3 || n > 63) {
        return false;
    }
    const char *dot = strchr(id, '.');
    if (dot == NULL || dot == id || dot[1] == '\0' || strchr(dot + 1, '.') != NULL) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            return false;
        }
    }
    return (size_t)(dot - id) < sizeof(((rpod_ha_entity_t *)0)->domain);
}

static void set_str(char *dst, size_t size, const char *src)
{
    memset(dst, 0, size);
    snprintf(dst, size, "%s", src != NULL ? src : "");
}

static bool entity_from_json(const rpod_jv_t *o, rpod_ha_entity_t *e)
{
    const char *id = rpod_jv_str(rpod_jv_get(o, "entity_id"), NULL);
    if (id == NULL || !rpod_ha_valid_id(id)) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    set_str(e->id, sizeof(e->id), id);
    size_t dlen = (size_t)(strchr(id, '.') - id);
    memcpy(e->domain, id, dlen);
    set_str(e->state, sizeof(e->state), rpod_jv_str(rpod_jv_get(o, "state"), ""));
    e->brightness = -1;

    const rpod_jv_t *a = rpod_jv_get(o, "attributes");
    const char *fn = rpod_jv_str(rpod_jv_get(a, "friendly_name"), NULL);
    set_str(e->name, sizeof(e->name), fn != NULL && fn[0] != '\0' ? fn : id);
    set_str(e->unit, sizeof(e->unit), rpod_jv_str(rpod_jv_get(a, "unit_of_measurement"), ""));

    if (strcmp(e->domain, "light") == 0) {
        const rpod_jv_t *modes = rpod_jv_get(a, "supported_color_modes");
        for (size_t i = 0; i < rpod_jv_len(modes); i++) {
            const char *m = rpod_jv_str(rpod_jv_at(modes, i), "");
            if (strcmp(m, "onoff") != 0 && m[0] != '\0') {
                e->dimmable = true;
            }
        }
        const rpod_jv_t *b = rpod_jv_get(a, "brightness");
        if (rpod_jv_is_num(b)) {
            e->dimmable = e->dimmable || modes == NULL;
            e->brightness = (int)lround(rpod_jv_num(b, 0) * 100.0 / 255.0);
            if (e->brightness < 0) {
                e->brightness = 0;
            } else if (e->brightness > 100) {
                e->brightness = 100;
            }
        }
    } else if (strcmp(e->domain, "climate") == 0) {
        const rpod_jv_t *t = rpod_jv_get(a, "temperature");
        if (rpod_jv_is_num(t)) {
            e->has_target = true;
            e->target = rpod_jv_num(t, 0);
        }
        const rpod_jv_t *c = rpod_jv_get(a, "current_temperature");
        if (rpod_jv_is_num(c)) {
            e->has_current = true;
            e->current = rpod_jv_num(c, 0);
        }
    }
    return true;
}

static int entity_cmp(const void *x, const void *y)
{
    const rpod_ha_entity_t *a = x, *b = y;
    int c = strcasecmp(a->name, b->name);
    return c != 0 ? c : strcmp(a->id, b->id);
}

bool rpod_ha_parse_states(const char *json, size_t len, rpod_ha_entity_t **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    rpod_jv_t *root = rpod_jv_parse(json, len);
    if (root == NULL || rpod_jv_type(root) != RPOD_JV_ARR) {
        rpod_jv_free(root);
        return false;
    }
    size_t total = rpod_jv_len(root);
    rpod_ha_entity_t *ents = calloc(total ? total : 1, sizeof(*ents));
    if (ents == NULL) {
        rpod_jv_free(root);
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < total; i++) {
        if (entity_from_json(rpod_jv_at(root, i), &ents[n])) {
            n++;
        }
    }
    rpod_jv_free(root);
    qsort(ents, n, sizeof(*ents), entity_cmp);
    *out = ents;
    *count = n;
    return true;
}

bool rpod_ha_parse_state(const char *json, size_t len, rpod_ha_entity_t *out)
{
    rpod_jv_t *root = rpod_jv_parse(json, len);
    bool ok = root != NULL && entity_from_json(root, out);
    rpod_jv_free(root);
    return ok;
}

/* --- What Center does -------------------------------------------------------- */

typedef struct {
    const char *service;
    const char *label;
    const char *optimistic; /* "" -> state unchanged until confirmed */
} plan_t;

static bool in_state(const rpod_ha_entity_t *e, const char *s)
{
    return strcmp(e->state, s) == 0;
}

static bool plan_for(const rpod_ha_entity_t *e, plan_t *p)
{
    const char *d = e->domain;
    if (in_state(e, "unavailable") || in_state(e, "unknown")) {
        return false;
    }
    if (strcmp(d, "light") == 0 || strcmp(d, "switch") == 0 || strcmp(d, "fan") == 0 ||
        strcmp(d, "input_boolean") == 0) {
        if (in_state(e, "on")) {
            *p = (plan_t){ "turn_off", "Turn Off", "off" };
        } else if (in_state(e, "off")) {
            *p = (plan_t){ "turn_on", "Turn On", "on" };
        } else {
            return false;
        }
        return true;
    }
    if (strcmp(d, "automation") == 0) {
        if (in_state(e, "on")) {
            *p = (plan_t){ "turn_off", "Disable", "off" };
        } else if (in_state(e, "off")) {
            *p = (plan_t){ "turn_on", "Enable", "on" };
        } else {
            return false;
        }
        return true;
    }
    if (strcmp(d, "cover") == 0) {
        if (in_state(e, "open") || in_state(e, "opening")) {
            *p = (plan_t){ "close_cover", "Close", "closing" };
        } else if (in_state(e, "closed") || in_state(e, "closing")) {
            *p = (plan_t){ "open_cover", "Open", "opening" };
        } else {
            return false;
        }
        return true;
    }
    if (strcmp(d, "lock") == 0) {
        if (in_state(e, "locked")) {
            *p = (plan_t){ "unlock", "Unlock", "unlocked" };
        } else if (in_state(e, "unlocked")) {
            *p = (plan_t){ "lock", "Lock", "locked" };
        } else {
            return false;
        }
        return true;
    }
    if (strcmp(d, "scene") == 0) {
        *p = (plan_t){ "turn_on", "Activate", "" };
        return true;
    }
    if (strcmp(d, "script") == 0) {
        *p = (plan_t){ "turn_on", "Run", "" };
        return true;
    }
    if (strcmp(d, "button") == 0 || strcmp(d, "input_button") == 0) {
        *p = (plan_t){ "press", "Press", "" };
        return true;
    }
    return false;
}

bool rpod_ha_actionable(const rpod_ha_entity_t *e)
{
    plan_t p;
    return plan_for(e, &p);
}

const char *rpod_ha_action_label(const rpod_ha_entity_t *e)
{
    plan_t p;
    return plan_for(e, &p) ? p.label : "";
}

/* --- Persistence -------------------------------------------------------------- */

static void load_config(void)
{
    if (g.state_path[0] == '\0') {
        return;
    }
    FILE *f = fopen(g.state_path, "r");
    if (f == NULL) {
        return;
    }
    char url[URL_MAX + 16] = "", token[TOKEN_MAX + 16] = "";
    if (fgets(url, sizeof(url), f) != NULL) {
        url[strcspn(url, "\r\n")] = '\0';
        if (fgets(token, sizeof(token), f) != NULL) {
            token[strcspn(token, "\r\n")] = '\0';
        }
    }
    fclose(f);
    char norm[URL_MAX + 1];
    if (rpod_ha_normalize_url(url, norm, sizeof(norm))) {
        snprintf(g.url, sizeof(g.url), "%s", norm);
    }
    size_t tlen = strlen(token);
    if (tlen >= TOKEN_MIN && tlen <= TOKEN_MAX) {
        memcpy(g.token, token, tlen + 1);
    }
}

/* Under the lock. */
static bool save_config(void)
{
    if (g.state_path[0] == '\0') {
        return true;
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.state_path);
    /* 0600: the token is a credential. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (f == NULL) {
        fprintf(stderr, "rpod: home assistant couldn't write %s: %s\n", tmp, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    fprintf(f, "%s\n%s\n", g.url, g.token);
    if (fclose(f) != 0 || rename(tmp, g.state_path) != 0) {
        fprintf(stderr, "rpod: home assistant couldn't save %s: %s\n", g.state_path, strerror(errno));
        remove(tmp);
        return false;
    }
    return true;
}

/* --- Shared state ------------------------------------------------------------- */

static bool configured_locked(void)
{
    return g.url[0] != '\0' && g.token[0] != '\0';
}

/* Under the lock: the config changed (or went), so what was fetched is no
 * longer about this server. */
static void config_changed_locked(void)
{
    g.gen++;
    free(g.ents);
    g.ents = NULL;
    g.nents = 0;
    g.err[0] = '\0';
    g.server_name[0] = g.server_version[0] = '\0';
    g.fahrenheit = false;
    g.have_info = false;
    g.njobs = 0;
    g.st = configured_locked() ? RPOD_HA_CONNECTING : RPOD_HA_UNCONFIGURED;
    g.refresh_pending = configured_locked();
    g.version++;
    pthread_cond_broadcast(&g.cv);
}

static void set_status_locked(rpod_ha_state_t st, const char *err)
{
    if (g.st != st || strcmp(g.err, err) != 0) {
        g.st = st;
        snprintf(g.err, sizeof(g.err), "%s", err);
        g.version++;
    }
}

bool rpod_ha_set_url(const char *url)
{
    char norm[URL_MAX + 1] = "";
    if (url[0] != '\0' && !rpod_ha_normalize_url(url, norm, sizeof(norm))) {
        return false;
    }
    pthread_mutex_lock(&g.mu);
    char old_url[sizeof(g.url)], old_token[sizeof(g.token)];
    memcpy(old_url, g.url, sizeof(old_url));
    memcpy(old_token, g.token, sizeof(old_token));
    snprintf(g.url, sizeof(g.url), "%s", norm);
    if (norm[0] == '\0') {
        g.token[0] = '\0'; /* forgetting the server forgets its token */
    }
    bool ok = save_config();
    if (!ok) {
        memcpy(g.url, old_url, sizeof(g.url));
        memcpy(g.token, old_token, sizeof(g.token));
    } else if (strcmp(old_url, g.url) != 0 || strcmp(old_token, g.token) != 0) {
        config_changed_locked();
    }
    pthread_mutex_unlock(&g.mu);
    return ok;
}

bool rpod_ha_set_token(const char *token)
{
    size_t n = strlen(token);
    if (n != 0 && (n < TOKEN_MIN || n > TOKEN_MAX)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (token[i] <= ' ' || token[i] >= 0x7f || token[i] == '"' || token[i] == '\\') {
            return false;
        }
    }
    pthread_mutex_lock(&g.mu);
    char old_token[sizeof(g.token)];
    memcpy(old_token, g.token, sizeof(old_token));
    snprintf(g.token, sizeof(g.token), "%s", token);
    bool ok = save_config();
    if (!ok) {
        memcpy(g.token, old_token, sizeof(g.token));
    } else if (strcmp(old_token, g.token) != 0) {
        config_changed_locked();
    }
    pthread_mutex_unlock(&g.mu);
    return ok;
}

void rpod_ha_url(char *out, size_t out_size)
{
    pthread_mutex_lock(&g.mu);
    snprintf(out, out_size, "%s", g.url);
    pthread_mutex_unlock(&g.mu);
}

bool rpod_ha_configured(void)
{
    pthread_mutex_lock(&g.mu);
    bool c = configured_locked();
    pthread_mutex_unlock(&g.mu);
    return c;
}

rpod_ha_state_t rpod_ha_state(void)
{
    pthread_mutex_lock(&g.mu);
    rpod_ha_state_t st = g.st;
    pthread_mutex_unlock(&g.mu);
    return st;
}

void rpod_ha_error(char *out, size_t out_size)
{
    pthread_mutex_lock(&g.mu);
    snprintf(out, out_size, "%s", g.err);
    pthread_mutex_unlock(&g.mu);
}

void rpod_ha_server(char *name, size_t name_size, char *version, size_t version_size)
{
    pthread_mutex_lock(&g.mu);
    snprintf(name, name_size, "%s", g.server_name);
    snprintf(version, version_size, "%s", g.server_version);
    pthread_mutex_unlock(&g.mu);
}

bool rpod_ha_fahrenheit(void)
{
    pthread_mutex_lock(&g.mu);
    bool f = g.fahrenheit;
    pthread_mutex_unlock(&g.mu);
    return f;
}

unsigned rpod_ha_version(void)
{
    pthread_mutex_lock(&g.mu);
    unsigned v = g.version;
    pthread_mutex_unlock(&g.mu);
    return v;
}

void rpod_ha_refresh(void)
{
    pthread_mutex_lock(&g.mu);
    if (configured_locked() && !g.refresh_pending) {
        g.refresh_pending = true;
        pthread_cond_signal(&g.cv);
    }
    pthread_mutex_unlock(&g.mu);
}

static bool in_domains(const char *domain, const char *const *domains)
{
    for (; *domains != NULL; domains++) {
        if (strcmp(domain, *domains) == 0) {
            return true;
        }
    }
    return false;
}

rpod_ha_entity_t *rpod_ha_list(const char *const *domains, size_t *count)
{
    pthread_mutex_lock(&g.mu);
    size_t n = 0;
    for (size_t i = 0; i < g.nents; i++) {
        n += in_domains(g.ents[i].domain, domains);
    }
    rpod_ha_entity_t *out = n ? malloc(n * sizeof(*out)) : NULL;
    if (out == NULL) {
        n = 0;
    }
    size_t k = 0;
    for (size_t i = 0; k < n && i < g.nents; i++) {
        if (in_domains(g.ents[i].domain, domains)) {
            out[k++] = g.ents[i];
        }
    }
    pthread_mutex_unlock(&g.mu);
    *count = k;
    return out;
}

size_t rpod_ha_count(const char *const *domains)
{
    pthread_mutex_lock(&g.mu);
    size_t n = 0;
    for (size_t i = 0; i < g.nents; i++) {
        n += in_domains(g.ents[i].domain, domains);
    }
    pthread_mutex_unlock(&g.mu);
    return n;
}

bool rpod_ha_find(const char *id, rpod_ha_entity_t *out)
{
    bool found = false;
    pthread_mutex_lock(&g.mu);
    for (size_t i = 0; i < g.nents; i++) {
        if (strcmp(g.ents[i].id, id) == 0) {
            *out = g.ents[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g.mu);
    return found;
}

bool rpod_ha_take_failure(char *out, size_t out_size)
{
    pthread_mutex_lock(&g.mu);
    bool failed = g.failed;
    if (failed) {
        snprintf(out, out_size, "%s", g.failure);
        g.failed = false;
    }
    pthread_mutex_unlock(&g.mu);
    return failed;
}

/* --- Actions ------------------------------------------------------------------- */

/* Under the lock: shows `state` on the entity until the server says better. */
static void patch_locked(const char *id, const char *state, int brightness, bool has_target, double target)
{
    for (size_t i = 0; i < g.nents; i++) {
        rpod_ha_entity_t *e = &g.ents[i];
        if (strcmp(e->id, id) != 0) {
            continue;
        }
        if (state != NULL && state[0] != '\0') {
            set_str(e->state, sizeof(e->state), state);
        }
        if (brightness >= 0) {
            e->brightness = brightness;
        }
        if (has_target) {
            e->has_target = true;
            e->target = target;
        }
        g.version++;
        return;
    }
}

static bool enqueue(const job_t *job)
{
    bool ok = false;
    pthread_mutex_lock(&g.mu);
    if (configured_locked() && g.njobs < JOBS_MAX) {
        g.jobs[g.njobs++] = *job;
        ok = true;
        pthread_cond_signal(&g.cv);
    }
    pthread_mutex_unlock(&g.mu);
    return ok;
}

bool rpod_ha_activate(const rpod_ha_entity_t *e)
{
    plan_t p;
    if (!plan_for(e, &p) || !rpod_ha_valid_id(e->id)) {
        return false;
    }
    job_t job = { 0 };
    snprintf(job.id, sizeof(job.id), "%s", e->id);
    snprintf(job.service, sizeof(job.service), "%s", p.service);
    snprintf(job.optimistic, sizeof(job.optimistic), "%s", p.optimistic);
    if (!enqueue(&job)) {
        return false;
    }
    pthread_mutex_lock(&g.mu);
    patch_locked(e->id, p.optimistic, -1, false, 0);
    pthread_mutex_unlock(&g.mu);
    return true;
}

bool rpod_ha_set_brightness(const char *id, int percent)
{
    if (!rpod_ha_valid_id(id) || strncmp(id, "light.", 6) != 0) {
        return false;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    job_t job = { 0 };
    snprintf(job.id, sizeof(job.id), "%s", id);
    if (percent == 0) {
        snprintf(job.service, sizeof(job.service), "turn_off");
        snprintf(job.optimistic, sizeof(job.optimistic), "off");
    } else {
        snprintf(job.service, sizeof(job.service), "turn_on");
        snprintf(job.arg_key, sizeof(job.arg_key), "brightness_pct");
        job.arg = percent;
        snprintf(job.optimistic, sizeof(job.optimistic), "on");
    }
    if (!enqueue(&job)) {
        return false;
    }
    pthread_mutex_lock(&g.mu);
    patch_locked(id, job.optimistic, percent, false, 0);
    pthread_mutex_unlock(&g.mu);
    return true;
}

bool rpod_ha_set_temperature(const char *id, double degrees)
{
    if (!rpod_ha_valid_id(id) || strncmp(id, "climate.", 8) != 0 || !isfinite(degrees) ||
        degrees < -50 || degrees > 150) {
        return false;
    }
    job_t job = { 0 };
    snprintf(job.id, sizeof(job.id), "%s", id);
    snprintf(job.service, sizeof(job.service), "set_temperature");
    snprintf(job.arg_key, sizeof(job.arg_key), "temperature");
    job.arg = degrees;
    if (!enqueue(&job)) {
        return false;
    }
    pthread_mutex_lock(&g.mu);
    patch_locked(id, NULL, -1, true, degrees);
    pthread_mutex_unlock(&g.mu);
    return true;
}

/* --- The worker ---------------------------------------------------------------- */

typedef struct {
    char *p;
    size_t n, cap;
    bool overflow;
} reply_t;

static size_t collect(char *data, size_t size, size_t nmemb, void *user)
{
    reply_t *r = user;
    size_t len = size * nmemb;
    if (r->n + len > MAX_REPLY) {
        r->overflow = true;
        return 0; /* aborts the transfer */
    }
    if (r->n + len + 1 > r->cap) {
        size_t cap = r->cap ? r->cap : 16384;
        while (cap < r->n + len + 1) {
            cap *= 2;
        }
        char *grown = realloc(r->p, cap);
        if (grown == NULL) {
            return 0;
        }
        r->p = grown;
        r->cap = cap;
    }
    memcpy(r->p + r->n, data, len);
    r->n += len;
    r->p[r->n] = '\0';
    return len;
}

/* A short line fit for a 320-pixel screen for why a request failed. */
static void describe_failure(CURLcode rc, long status, const char *url, char *out, size_t out_size)
{
    const char *host = strstr(url, "://");
    host = host != NULL ? host + 3 : url;
    int hlen = (int)strcspn(host, "/");
    if (rc == CURLE_OK) {
        if (status == 401 || status == 403) {
            snprintf(out, out_size, "The access token was rejected.");
        } else if (status == 404) {
            snprintf(out, out_size, "Not found - is that Home Assistant's address?");
        } else {
            snprintf(out, out_size, "Home Assistant answered %ld.", status);
        }
        return;
    }
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
        snprintf(out, out_size, "Can't find %.*s on the network.", hlen, host);
        break;
    case CURLE_COULDNT_CONNECT:
        snprintf(out, out_size, "Can't connect to %.*s.", hlen, host);
        break;
    case CURLE_OPERATION_TIMEDOUT:
        snprintf(out, out_size, "%.*s didn't answer in time.", hlen, host);
        break;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE:
        snprintf(out, out_size, "The server's certificate isn't trusted.");
        break;
    case CURLE_WRITE_ERROR:
        snprintf(out, out_size, "The reply was too big.");
        break;
    default:
        snprintf(out, out_size, "%s", curl_easy_strerror(rc));
        break;
    }
}

/* One request to the server. `body` NULL is a GET, else a JSON POST. True
 * with the reply in `reply` on a 200; false with `why` set otherwise. */
static bool request(CURL *curl, const char *url, const char *token, const char *path, const char *body,
                    long timeout_s, reply_t *reply, char *why, size_t why_size)
{
    char full[URL_MAX + 160];
    snprintf(full, sizeof(full), "%s%s", url, path);
    char auth[TOKEN_MAX + 32];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);

    struct curl_slist *headers = curl_slist_append(NULL, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, full);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    /* The token goes to the address it was given for, and nowhere else. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, reply);
    if (body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);

    if (rc == CURLE_OK && status >= 200 && status < 300) {
        return true;
    }
    describe_failure(rc, status, url, why, why_size);
    return false;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* GET /api/config once per server: its name, version and units. */
static void fetch_info(CURL *curl, const char *url, const char *token, unsigned gen)
{
    reply_t reply = { 0 };
    char why[96];
    if (request(curl, url, token, "/api/config", NULL, 10, &reply, why, sizeof(why))) {
        rpod_jv_t *root = rpod_jv_parse(reply.p, reply.n);
        if (root != NULL) {
            pthread_mutex_lock(&g.mu);
            if (g.gen == gen) {
                snprintf(g.server_name, sizeof(g.server_name), "%s", rpod_jv_str(rpod_jv_get(root, "location_name"), ""));
                snprintf(g.server_version, sizeof(g.server_version), "%s", rpod_jv_str(rpod_jv_get(root, "version"), ""));
                const char *t = rpod_jv_str(rpod_jv_get(rpod_jv_get(root, "unit_system"), "temperature"), "");
                g.fahrenheit = strstr(t, "F") != NULL;
                g.have_info = true;
                g.version++;
            }
            pthread_mutex_unlock(&g.mu);
            rpod_jv_free(root);
        }
    }
    free(reply.p);
}

static void do_refresh(CURL *curl, const char *url, const char *token, unsigned gen)
{
    reply_t reply = { 0 };
    char why[96];
    if (!request(curl, url, token, "/api/states", NULL, 20, &reply, why, sizeof(why))) {
        pthread_mutex_lock(&g.mu);
        if (g.gen == gen) {
            set_status_locked(RPOD_HA_ERROR, why);
        }
        pthread_mutex_unlock(&g.mu);
        free(reply.p);
        return;
    }
    rpod_ha_entity_t *ents = NULL;
    size_t n = 0;
    bool ok = reply.p != NULL && rpod_ha_parse_states(reply.p, reply.n, &ents, &n);
    free(reply.p);

    pthread_mutex_lock(&g.mu);
    if (g.gen == gen) {
        if (!ok) {
            set_status_locked(RPOD_HA_ERROR, "That isn't Home Assistant's reply.");
            free(ents);
        } else {
            if (n != g.nents || (n > 0 && memcmp(ents, g.ents, n * sizeof(*ents)) != 0)) {
                free(g.ents);
                g.ents = ents;
                g.nents = n;
                g.version++;
            } else {
                free(ents);
            }
            set_status_locked(RPOD_HA_OK, "");
        }
    } else {
        free(ents);
    }
    bool need_info = g.gen == gen && ok && !g.have_info;
    pthread_mutex_unlock(&g.mu);

    if (need_info) {
        fetch_info(curl, url, token, gen);
    }
}

static void do_job(CURL *curl, const char *url, const char *token, unsigned gen, const job_t *job)
{
    char path[160], body[200];
    size_t dlen = (size_t)(strchr(job->id, '.') - job->id);
    snprintf(path, sizeof(path), "/api/services/%.*s/%s", (int)dlen, job->id, job->service);
    if (job->arg_key[0] != '\0') {
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"%s\":%.4g}", job->id, job->arg_key, job->arg);
    } else {
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", job->id);
    }

    reply_t reply = { 0 };
    char why[96];
    bool ok = request(curl, url, token, path, body, 10, &reply, why, sizeof(why));
    free(reply.p);
    if (!ok) {
        pthread_mutex_lock(&g.mu);
        if (g.gen == gen) {
            snprintf(g.failure, sizeof(g.failure), "%s", why);
            g.failed = true;
            g.version++;
            g.refresh_pending = true; /* undo the optimistic state */
        }
        pthread_mutex_unlock(&g.mu);
        return;
    }

    /* Ask how it went. */
    sleep_ms(CONFIRM_DELAY_MS);
    char spath[96];
    snprintf(spath, sizeof(spath), "/api/states/%s", job->id);
    reply_t state = { 0 };
    rpod_ha_entity_t fresh;
    if (request(curl, url, token, spath, NULL, 10, &state, why, sizeof(why)) && state.p != NULL &&
        rpod_ha_parse_state(state.p, state.n, &fresh)) {
        pthread_mutex_lock(&g.mu);
        if (g.gen == gen) {
            for (size_t i = 0; i < g.nents; i++) {
                if (strcmp(g.ents[i].id, fresh.id) == 0) {
                    if (memcmp(&g.ents[i], &fresh, sizeof(fresh)) != 0) {
                        g.ents[i] = fresh;
                        g.version++;
                    }
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g.mu);
    }
    free(state.p);
}

static void *worker(void *arg)
{
    (void)arg;
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "rpod: home assistant curl_easy_init failed\n");
        return NULL;
    }
    pthread_mutex_lock(&g.mu);
    for (;;) {
        bool is_job = g.njobs > 0;
        bool is_refresh = !is_job && g.refresh_pending && configured_locked();
        if (!is_job && !is_refresh) {
            pthread_cond_wait(&g.cv, &g.mu);
            continue;
        }
        job_t job = { 0 };
        if (is_job) {
            job = g.jobs[0];
            memmove(&g.jobs[0], &g.jobs[1], (g.njobs - 1) * sizeof(g.jobs[0]));
            g.njobs--;
        } else {
            g.refresh_pending = false;
        }
        char url[URL_MAX + 1], token[TOKEN_MAX + 1];
        memcpy(url, g.url, sizeof(url));
        memcpy(token, g.token, sizeof(token));
        unsigned gen = g.gen;
        pthread_mutex_unlock(&g.mu);

        if (is_job) {
            do_job(curl, url, token, gen, &job);
        } else {
            do_refresh(curl, url, token, gen);
        }
        memset(token, 0, sizeof(token));

        pthread_mutex_lock(&g.mu);
    }
    return NULL;
}

void rpod_ha_init(const char *state_path)
{
    snprintf(g.state_path, sizeof(g.state_path), "%s", state_path != NULL ? state_path : "");
    load_config();
    g.st = configured_locked() ? RPOD_HA_CONNECTING : RPOD_HA_UNCONFIGURED;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (pthread_create(&g.thread, NULL, worker, NULL) != 0) {
        fprintf(stderr, "rpod: home assistant worker failed to start\n");
        return;
    }
    pthread_detach(g.thread);
}
