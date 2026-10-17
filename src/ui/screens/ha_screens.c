#include "ha_screens.h"

#include "live_list.h"
#include "ha/ha.h"
#include "ui/hud.h"
#include "ui/metrics.h"
#include "ui/spinner.h"
#include "ui/theme.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How often the screens look for news, and how many looks make a refresh
 * from the server: every 5 s while one is up, never otherwise. */
#define TICK_MS       1000
#define TICKS_REFRESH 5

/* A category is a row on the first screen: what it's called and which of
 * Home Assistant's domains it gathers. */
typedef struct {
    const char *title;
    const char *domains[3]; /* NULL-terminated */
} category_t;

static const category_t CATEGORIES[] = {
    { "Lights",      { "light", NULL } },
    { "Switches",    { "switch", "input_boolean", NULL } },
    { "Fans",        { "fan", NULL } },
    { "Covers",      { "cover", NULL } },
    { "Locks",       { "lock", NULL } },
    { "Climate",     { "climate", NULL } },
    { "Scenes",      { "scene", NULL } },
    { "Scripts",     { "script", NULL } },
    { "Automations", { "automation", NULL } },
    { "Buttons",     { "button", "input_button", NULL } },
    { "Sensors",     { "sensor", NULL } },
};
#define NCATEGORIES (sizeof(CATEGORIES) / sizeof(CATEGORIES[0]))

/* --- Keeping a screen up to date -------------------------------------------- */

/* Every view starts with the signature of what it last drew, so the watcher
 * can tell whether the state has moved on in a way this screen shows. */
typedef struct {
    uint32_t shown;
} view_t;

typedef struct {
    lv_obj_t *screen;
    rpod_live_list_t *ll;
    view_t *view;
    uint32_t (*signature)(const view_t *view);
    lv_timer_t *timer;
    unsigned ticks;
    unsigned seen; /* rpod_ha_version() at the last look */
} watch_t;

static uint32_t fnv(uint32_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* What every screen shows regardless of its content: whether it's
 * configured, connected, and the error if not. */
static uint32_t sig_base(void)
{
    uint32_t h = 2166136261u;
    rpod_ha_state_t st = rpod_ha_state();
    char err[96];
    rpod_ha_error(err, sizeof(err));
    h = fnv(h, &st, sizeof(st));
    return fnv(h, err, strlen(err));
}

static void watch_tick(lv_timer_t *t)
{
    watch_t *w = lv_timer_get_user_data(t);
    /* Only the screen in view works: the ones under it catch up when shown
     * (their version differs then). */
    if (lv_screen_active() != w->screen) {
        return;
    }
    if (++w->ticks % TICKS_REFRESH == 0) {
        rpod_ha_refresh();
    }
    char why[96];
    if (rpod_ha_take_failure(why, sizeof(why))) {
        rpod_hud_message(RPOD_HUD_ICON_HOME, why);
    }
    unsigned v = rpod_ha_version();
    if (v == w->seen) {
        return;
    }
    w->seen = v;
    if (w->signature(w->view) != w->view->shown) {
        rpod_live_list_changed(w->ll);
    }
}

static void watch_deleted(lv_event_t *e)
{
    watch_t *w = lv_event_get_user_data(e);
    lv_timer_delete(w->timer);
    free(w);
}

/* `view` is owned by the live list (its ctx_free). */
static void watch_start(lv_obj_t *screen, rpod_live_list_t *ll, view_t *view,
                        uint32_t (*signature)(const view_t *view))
{
    watch_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return;
    }
    w->screen = screen;
    w->ll = ll;
    w->view = view;
    w->signature = signature;
    w->seen = rpod_ha_version();
    w->timer = lv_timer_create(watch_tick, TICK_MS, w);
    lv_obj_add_event_cb(screen, watch_deleted, LV_EVENT_DELETE, w);
}

/* Shows why there's nothing to list and returns true, unless there are
 * devices to show. `have` is how many the screen has. */
static bool show_unless_ready(rpod_live_list_t *ll, size_t have)
{
    if (!rpod_ha_configured()) {
        rpod_live_list_message(ll, "Home Assistant",
                               "Add your server in the web UI: Settings > Home Assistant.");
        return true;
    }
    rpod_ha_state_t st = rpod_ha_state();
    if (have == 0 && st == RPOD_HA_ERROR) {
        char err[96];
        rpod_ha_error(err, sizeof(err));
        rpod_live_list_message(ll, "Home Assistant", err);
        return true;
    }
    if (have == 0 && st != RPOD_HA_OK) {
        rpod_live_list_message(ll, "Home Assistant", "Connecting...");
        return true;
    }
    return false;
}

/* --- Words for what's on ------------------------------------------------------- */

static void capitalized(char *out, size_t out_size, const char *s)
{
    snprintf(out, out_size, "%s", s);
    if (out[0] != '\0') {
        out[0] = (char)toupper((unsigned char)out[0]);
    }
    for (char *c = out; *c != '\0'; c++) {
        if (*c == '_') {
            *c = ' ';
        }
    }
}

/* "21.5" or "22": no more decimals than the number has. */
static void degrees(char *out, size_t out_size, double v)
{
    if (fabs(v - round(v)) < 0.05) {
        snprintf(out, out_size, "%.0f\xc2\xb0", v);
    } else {
        snprintf(out, out_size, "%.1f\xc2\xb0", v);
    }
}

/* The right-hand side of a device's row. */
static void state_text(const rpod_ha_entity_t *e, char *out, size_t out_size)
{
    const char *d = e->domain;
    if (strcmp(e->state, "unavailable") == 0 || strcmp(e->state, "unknown") == 0) {
        capitalized(out, out_size, e->state);
    } else if (strcmp(d, "sensor") == 0) {
        snprintf(out, out_size, "%s%s%s", e->state, e->unit[0] != '\0' ? " " : "", e->unit);
    } else if (strcmp(d, "light") == 0 && strcmp(e->state, "on") == 0 && e->brightness >= 0) {
        snprintf(out, out_size, "%d%%", e->brightness);
    } else if (strcmp(d, "climate") == 0) {
        if (e->has_target) {
            degrees(out, out_size, e->target);
        } else {
            capitalized(out, out_size, e->state);
        }
    } else if (strcmp(d, "scene") == 0 || strcmp(d, "button") == 0 || strcmp(d, "input_button") == 0 ||
               strcmp(d, "script") == 0) {
        out[0] = '\0'; /* a scene's state is when it was last used */
    } else {
        capitalized(out, out_size, e->state);
    }
}

static bool has_page(const rpod_ha_entity_t *e)
{
    return (strcmp(e->domain, "light") == 0 && e->dimmable && strcmp(e->state, "unavailable") != 0) ||
           strcmp(e->domain, "climate") == 0;
}

/* --- A device's page: power, and a number ----------------------------------- */

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_spinner_t sp;
    char id[64];
    bool brightness; /* else a temperature */
    int initial;
} number_t;

static void number_done(void *ctx)
{
    number_t *n = ctx;
    int v = n->sp.field[0].value;
    if (n->brightness) {
        if (v != n->initial) {
            rpod_ha_set_brightness(n->id, v);
        }
    } else if (v != n->initial) {
        rpod_ha_set_temperature(n->id, v);
    }
    rpod_screen_stack_pop(n->stack); /* frees n */
}

static void build_number(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    number_t *n = ctx;
    rpod_screen_stack_set_title(stack, n->brightness ? "Brightness" : "Temperature");
    lv_obj_t *sp = rpod_spinner_create(screen, &n->sp, number_done, n);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, rpod_metrics_mid_dy(rpod_metrics()));

    lv_obj_t *unit = lv_label_create(screen);
    lv_label_set_text(unit, n->brightness ? "Brightness %" : (rpod_ha_fahrenheit() ? "Degrees F" : "Degrees C"));
    lv_obj_set_style_text_font(unit, rpod_metrics()->font_small, 0);
    lv_obj_set_style_text_color(unit, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_align(unit, LV_ALIGN_BOTTOM_MID, 0, -12 - rpod_metrics()->footer_h);
}

static void push_number(rpod_screen_stack_t *stack, const rpod_ha_entity_t *e, bool brightness)
{
    number_t *n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return;
    }
    n->stack = stack;
    n->brightness = brightness;
    snprintf(n->id, sizeof(n->id), "%s", e->id);
    if (brightness) {
        n->initial = e->brightness >= 0 ? e->brightness : 50;
        n->sp = (rpod_spinner_t){ .field = { { .min = 0, .max = 100, .value = n->initial, .digits = 3 } }, .count = 1 };
    } else {
        int lo = rpod_ha_fahrenheit() ? 41 : 5, hi = rpod_ha_fahrenheit() ? 95 : 35;
        n->initial = (int)lround(e->target);
        if (n->initial < lo) {
            lo = n->initial;
        }
        if (n->initial > hi) {
            hi = n->initial;
        }
        n->sp = (rpod_spinner_t){ .field = { { .min = lo, .max = hi, .value = n->initial, .digits = 2 } }, .count = 1 };
    }
    rpod_screen_stack_push(stack, build_number, n, free);
}

typedef struct {
    view_t v;
    char id[64];
} page_view_t;

static uint32_t page_signature(const view_t *view)
{
    const page_view_t *pv = (const page_view_t *)view;
    uint32_t h = sig_base();
    rpod_ha_entity_t e;
    bool found = rpod_ha_find(pv->id, &e);
    h = fnv(h, &found, sizeof(found));
    return found ? fnv(h, &e, sizeof(e)) : h;
}

static void on_page_power(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    rpod_live_list_t *ll = ref->ll; /* the rebuild below frees ref */
    page_view_t *pv = rpod_live_list_ctx(ll);
    rpod_ha_entity_t e;
    if (rpod_ha_find(pv->id, &e) && rpod_ha_actionable(&e) && rpod_ha_activate(&e)) {
        rpod_live_list_changed(ll);
    }
}

static void on_page_number(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    page_view_t *pv = rpod_live_list_ctx(ref->ll);
    rpod_ha_entity_t e;
    if (rpod_ha_find(pv->id, &e)) {
        bool light = strcmp(e.domain, "light") == 0;
        if (light || e.has_target) {
            push_number(stack, &e, light);
        }
    }
}

static void fill_page(rpod_live_list_t *ll)
{
    page_view_t *pv = rpod_live_list_ctx(ll);
    pv->v.shown = page_signature(&pv->v);

    rpod_ha_entity_t e;
    if (!rpod_ha_find(pv->id, &e)) {
        rpod_live_list_message(ll, "Home Assistant", "That device isn't there any more.");
        return;
    }
    char text[32];
    rpod_list_item_t *it;

    if (strcmp(e.domain, "light") == 0) {
        if (rpod_ha_actionable(&e)) {
            it = rpod_live_list_add(ll, "power", on_page_power);
            if (it != NULL) {
                snprintf(it->text, sizeof(it->text), "%s", rpod_ha_action_label(&e));
            }
        }
        it = rpod_live_list_add(ll, "brightness", on_page_number);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "Brightness");
            if (e.brightness >= 0) {
                snprintf(it->accessory, sizeof(it->accessory), "%d%%", e.brightness);
            }
            it->chevron = true;
        }
        return;
    }

    /* Climate. */
    if (e.has_target) {
        it = rpod_live_list_add(ll, "target", on_page_number);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "Target");
            degrees(it->accessory, sizeof(it->accessory), e.target);
            it->chevron = true;
        }
    }
    if (e.has_current) {
        it = rpod_live_list_add(ll, "room", NULL);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "Room");
            degrees(it->accessory, sizeof(it->accessory), e.current);
        }
    }
    it = rpod_live_list_add(ll, "mode", NULL);
    if (it != NULL) {
        capitalized(text, sizeof(text), e.state);
        snprintf(it->text, sizeof(it->text), "Mode");
        snprintf(it->accessory, sizeof(it->accessory), "%s", text);
    }
}

/* ctx: the entity's id, owned by the stack. */
static void build_page(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    page_view_t *pv = calloc(1, sizeof(*pv));
    if (pv == NULL) {
        return;
    }
    snprintf(pv->id, sizeof(pv->id), "%s", (const char *)ctx);
    rpod_ha_entity_t e;
    if (rpod_ha_find(pv->id, &e)) {
        rpod_screen_stack_set_title(stack, e.name);
    }
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill_page, pv, free);
    if (ll != NULL) {
        watch_start(screen, ll, &pv->v, page_signature);
    }
    rpod_ha_refresh();
}

/* --- A category's devices ------------------------------------------------------ */

typedef struct {
    view_t v;
    const category_t *cat;
} category_view_t;

static uint32_t category_signature(const view_t *view)
{
    const category_view_t *cv = (const category_view_t *)view;
    uint32_t h = sig_base();
    size_t n;
    rpod_ha_entity_t *list = rpod_ha_list(cv->cat->domains, &n);
    h = fnv(h, &n, sizeof(n));
    if (list != NULL) {
        h = fnv(h, list, n * sizeof(*list));
    }
    free(list);
    return h;
}

static void on_device(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    rpod_ha_entity_t e;
    if (!rpod_ha_find(ref->key, &e)) {
        return;
    }
    if (has_page(&e)) {
        /* The page's ctx is the id, which the stack frees with the screen. */
        char *id = strdup(e.id);
        if (id != NULL) {
            rpod_screen_stack_push(stack, build_page, id, free);
        }
    } else if (rpod_ha_actionable(&e) && rpod_ha_activate(&e)) {
        rpod_live_list_changed(ref->ll); /* shows the new state at once */
    }
}

static void fill_category(rpod_live_list_t *ll)
{
    category_view_t *cv = rpod_live_list_ctx(ll);
    cv->v.shown = category_signature(&cv->v);

    size_t n;
    rpod_ha_entity_t *list = rpod_ha_list(cv->cat->domains, &n);
    if (show_unless_ready(ll, n)) {
        free(list);
        return;
    }
    if (n == 0) {
        rpod_live_list_message(ll, cv->cat->title, "Nothing here.");
        free(list);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const rpod_ha_entity_t *e = &list[i];
        rpod_list_item_t *it = rpod_live_list_add(ll, e->id, on_device);
        if (it == NULL) {
            break;
        }
        snprintf(it->text, sizeof(it->text), "%s", e->name);
        state_text(e, it->accessory, sizeof(it->accessory));
        it->chevron = has_page(e);
        if (strcmp(e->domain, "climate") == 0 && e->has_current) {
            char cur[16];
            degrees(cur, sizeof(cur), e->current);
            char mode[32];
            capitalized(mode, sizeof(mode), e->state);
            snprintf(it->subtitle, sizeof(it->subtitle), "%s, now %s", mode, cur);
        }
    }
    free(list);
}

/* ctx: the category (static). */
static void build_category(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    category_view_t *cv = calloc(1, sizeof(*cv));
    if (cv == NULL) {
        return;
    }
    cv->cat = ctx;
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill_category, cv, free);
    if (ll != NULL) {
        watch_start(screen, ll, &cv->v, category_signature);
    }
    rpod_ha_refresh();
}

/* --- First screen: the categories -------------------------------------------- */

static uint32_t root_signature(const view_t *view)
{
    (void)view;
    uint32_t h = sig_base();
    for (size_t i = 0; i < NCATEGORIES; i++) {
        size_t n = rpod_ha_count(CATEGORIES[i].domains);
        h = fnv(h, &n, sizeof(n));
    }
    return h;
}

static void on_category(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    for (size_t i = 0; i < NCATEGORIES; i++) {
        if (strcmp(ref->key, CATEGORIES[i].title) == 0) {
            rpod_screen_stack_push(stack, build_category, (void *)&CATEGORIES[i], NULL);
            return;
        }
    }
}

static void fill_root(rpod_live_list_t *ll)
{
    view_t *v = rpod_live_list_ctx(ll);
    v->shown = root_signature(v);

    size_t counts[NCATEGORIES], total = 0;
    for (size_t i = 0; i < NCATEGORIES; i++) {
        counts[i] = rpod_ha_count(CATEGORIES[i].domains);
        total += counts[i];
    }
    if (show_unless_ready(ll, total)) {
        return;
    }
    if (total == 0) {
        rpod_live_list_message(ll, "Home Assistant", "No devices found.");
        return;
    }
    for (size_t i = 0; i < NCATEGORIES; i++) {
        if (counts[i] == 0) {
            continue;
        }
        rpod_list_item_t *it = rpod_live_list_add(ll, CATEGORIES[i].title, on_category);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "%s", CATEGORIES[i].title);
            snprintf(it->accessory, sizeof(it->accessory), "%zu", counts[i]);
            it->chevron = true;
        }
    }
}

void rpod_ha_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    view_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        return;
    }
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill_root, v, free);
    if (ll != NULL) {
        watch_start(screen, ll, v, root_signature);
    }
    rpod_ha_refresh();
}
