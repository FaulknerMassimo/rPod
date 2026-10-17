/*
 * Home Assistant (docs/PLAN.md §8.8): Extras > Home Assistant lists the
 * server's lights, switches, covers, locks, climate and the rest, and
 * controls them with the wheel. This is the client: Home Assistant's REST
 * API (https://developers.home-assistant.io/docs/api/rest/) with a
 * long-lived access token, over libcurl.
 *
 * Every request runs on a worker thread of its own -- a Home Assistant
 * that's off or out of Wi-Fi range takes seconds to time out, and the LVGL
 * thread never waits for it. Screens read a snapshot of the entities the
 * worker last fetched, and poll rpod_ha_version() to learn when it changed
 * (the same shape as the Bluetooth mirror, audio/bluetooth.h).
 *
 * The click wheel has no keyboard, so the server's address and token come
 * from the web UI (Settings > Home Assistant, through ui/control.h) or the
 * state file by hand. The token is a credential: the file is 0600, and no
 * reply ever carries it back out.
 *
 * Polling, not a WebSocket: the entities are refreshed (rpod_ha_refresh())
 * every few seconds while a Home Assistant screen is up, and not at all
 * otherwise, so an idle rPod costs the server nothing and the Zero 2 W no
 * wake-ups.
 */

#ifndef RPOD_HA_H
#define RPOD_HA_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char id[64];      /* "light.kitchen"; longer ids are left out (a list row's key is 63 bytes) */
    char domain[24];  /* "light" */
    char name[64];    /* friendly_name, else the id */
    char state[32];   /* "on", "off", "unavailable", "21.5"... */
    char unit[16];    /* sensors' unit_of_measurement */
    bool dimmable;    /* a light with a brightness */
    int brightness;   /* 0-100 (percent), -1 when unknown/off */
    bool has_target;  /* climate: a target temperature... */
    double target;
    bool has_current;
    double current;   /* climate: the room's temperature */
} rpod_ha_entity_t;

typedef enum {
    RPOD_HA_UNCONFIGURED, /* no address or no token */
    RPOD_HA_CONNECTING,   /* configured, nothing heard back yet */
    RPOD_HA_OK,
    RPOD_HA_ERROR,        /* the last request failed: rpod_ha_error() says why */
} rpod_ha_state_t;

/* Loads the saved address and token from `state_path` (NULL: nothing
 * persisted) and starts the worker thread. Call once. */
void rpod_ha_init(const char *state_path);

/* --- Settings (ui/control.h) ----------------------------------------------- */

/* The server's address, e.g. "http://homeassistant.local:8123". A bare
 * "host:port" gets http://; a trailing slash goes. "" forgets the server and
 * the token. False, changing nothing, for something that isn't a web
 * address, or if it couldn't be saved. */
bool rpod_ha_set_url(const char *url);

/* The long-lived access token (Profile > Security in Home Assistant). ""
 * forgets it. False for something that can't be one. */
bool rpod_ha_set_token(const char *token);

/* The address as saved; "" without one. Never the token. */
void rpod_ha_url(char *out, size_t out_size);

/* Both an address and a token are set. */
bool rpod_ha_configured(void);

/* --- State ------------------------------------------------------------------ */

rpod_ha_state_t rpod_ha_state(void);

/* Why the last request failed ("" unless the state is RPOD_HA_ERROR), as a
 * short line fit for the screen. */
void rpod_ha_error(char *out, size_t out_size);

/* The server's name and version, once it has said ("" until then). */
void rpod_ha_server(char *name, size_t name_size, char *version, size_t version_size);

/* Whether the server's temperatures are in Fahrenheit (else Celsius). */
bool rpod_ha_fahrenheit(void);

/* Bumped whenever what a screen would show changed. */
unsigned rpod_ha_version(void);

/* Asks the worker to fetch every entity. Cheap and coalescing: a refresh
 * already pending or running absorbs it. */
void rpod_ha_refresh(void);

/* A copy of the entities in any of `domains` (NULL-terminated), sorted by
 * name. Caller frees. *count is 0 and NULL is returned if there are none. */
rpod_ha_entity_t *rpod_ha_list(const char *const *domains, size_t *count);

/* How many entities are in any of `domains`. */
size_t rpod_ha_count(const char *const *domains);

/* The entity with this id, if it's known. */
bool rpod_ha_find(const char *id, rpod_ha_entity_t *out);

/* --- Actions ---------------------------------------------------------------- */

/* Whether pressing Center on this entity does something. */
bool rpod_ha_actionable(const rpod_ha_entity_t *e);

/* What Center does, in words for the screen: "Turn Off", "Lock", "Run"... */
const char *rpod_ha_action_label(const rpod_ha_entity_t *e);

/* Does it: a light, switch or fan toggles, a cover opens or closes, a lock
 * locks or unlocks, a scene is activated, a script run, a button pressed.
 * The cached state changes at once; the worker confirms it with the server
 * and corrects it if it didn't take. */
bool rpod_ha_activate(const rpod_ha_entity_t *e);

/* Sets a light's brightness (percent; 0 turns it off). */
bool rpod_ha_set_brightness(const char *id, int percent);

/* Sets a climate entity's target temperature. */
bool rpod_ha_set_temperature(const char *id, double degrees);

/* Whether the last action failed since this was last asked (it says why in
 * `out`), for the screen to tell the person. */
bool rpod_ha_take_failure(char *out, size_t out_size);

/* --- Pure helpers (tests/test_ha.c) ------------------------------------------ */

/* Normalises an address the way rpod_ha_set_url() saves it: true and `out`
 * set, or false for something that isn't a web address. */
bool rpod_ha_normalize_url(const char *in, char *out, size_t out_size);

/* Whether `id` is a plausible entity id ("domain.object_id", lower case,
 * at most 63 bytes). Anything that goes into a URL or a request body
 * passes this first. */
bool rpod_ha_valid_id(const char *id);

/* Parses the reply of GET /api/states into entities, sorted by name; ones
 * with an unusable id are left out. Caller frees *out. False if `json`
 * isn't a list of states. */
bool rpod_ha_parse_states(const char *json, size_t len, rpod_ha_entity_t **out, size_t *count);

/* Parses one state object (GET /api/states/<id>). */
bool rpod_ha_parse_state(const char *json, size_t len, rpod_ha_entity_t *out);

#endif /* RPOD_HA_H */
