/*
 * BlueZ client for Settings > Bluetooth (docs/PLAN.md §6.3, §8.1): adapter
 * power, discovery, pair + trust + connect, disconnect, forget.
 *
 * Talks to bluetoothd over the system D-Bus (sd-bus, from libsystemd) on the
 * LVGL thread only. Every BlueZ call is asynchronous -- Pair() alone can take
 * 10+ seconds -- and an lv_timer drains the bus connection, so nothing here
 * ever blocks the click wheel.
 *
 * The module keeps its own mirror of BlueZ's objects (one adapter, the first
 * one found, plus its devices), seeded by ObjectManager.GetManagedObjects and
 * kept current by InterfacesAdded/Removed and PropertiesChanged. It survives
 * bluetoothd starting late or restarting: the mirror empties while org.bluez
 * has no owner and is refetched when it gets one. Screens read the mirror
 * synchronously and register a watcher to rebuild when it changes.
 *
 * The sim uses the dev machine's own system bus -- i.e. the desktop's real
 * Bluetooth adapter. Point DBUS_SYSTEM_BUS_ADDRESS at another bus (e.g. one
 * running python-dbusmock's bluez5 template) to keep it away from that.
 */

#ifndef RPOD_BLUETOOTH_H
#define RPOD_BLUETOOTH_H

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    RPOD_BT_UNAVAILABLE, /* no system bus, or bluetoothd isn't running */
    RPOD_BT_NO_ADAPTER,  /* bluetoothd is up but sees no controller */
    RPOD_BT_OFF,
    RPOD_BT_ON,
} rpod_bt_state_t;

/* A call this process started on a device that BlueZ hasn't answered yet. */
typedef enum {
    RPOD_BT_OP_NONE = 0,
    RPOD_BT_OP_PAIRING,
    RPOD_BT_OP_CONNECTING,
    RPOD_BT_OP_DISCONNECTING,
} rpod_bt_op_t;

typedef struct {
    char path[64];  /* BlueZ object path -- the stable id to pass back in */
    char name[96];  /* Alias: the device's name, or BlueZ's address fallback */
    bool named;     /* BlueZ knows a real name (not just the address) */
    bool paired;
    bool connected;
    bool audio;     /* headphones/speaker by class, icon, or service UUID */
    rpod_bt_op_t op;
    char error[64]; /* why the last op failed, for display; "" if it didn't */
} rpod_bt_device_t;

/* Connects to the system bus and starts the lv_timer that drives it. Call
 * once, after lv_init() -- rpod_app_run() does. Never fails: with no bus the
 * state just stays RPOD_BT_UNAVAILABLE (and it retries every few seconds). */
void rpod_bt_init(void);

rpod_bt_state_t rpod_bt_state(void);

/* True while a power toggle is in flight, or while the adapter is
 * discovering (for any client, not just us). */
bool rpod_bt_power_pending(void);
bool rpod_bt_discovering(void);

/* Why the adapter can't be (or failed to be) powered, for display -- e.g.
 * "Blocked by rfkill". "" if nothing's wrong. */
const char *rpod_bt_adapter_error(void);

/* The adapter's devices, in no particular order. Pointers stay valid until
 * the bus is next processed (the next lv_timer tick) -- copy what you keep,
 * which for a device is its path. rpod_bt_find() returns NULL for a path
 * that isn't (or is no longer) there. */
size_t rpod_bt_device_count(void);
const rpod_bt_device_t *rpod_bt_device_at(size_t i);
const rpod_bt_device_t *rpod_bt_find(const char *path);

/* Calls cb(user) on the LVGL thread after anything visible changes (devices
 * added/removed, names, paired/connected, op progress, adapter power), at
 * most once per timer tick, until `owner` is deleted. Never called from
 * inside one of the functions below. */
void rpod_bt_watch(lv_obj_t *owner, void (*cb)(void *user), void *user);

void rpod_bt_set_powered(bool on);

/* Discovery of BR/EDR devices (A2DP is classic Bluetooth, and skipping LE
 * keeps phones' and AirPods' rotating LE addresses out of the list) for as
 * long as a scan screen is open. Paused while a pairing runs -- inquiry and
 * connection setup fight over the radio -- and resumed afterwards. */
void rpod_bt_scan_start(void);
void rpod_bt_scan_stop(void);

/* Pair, then mark trusted (so the device may reconnect on its own), then
 * connect. Pairs as NoInputNoOutput ("just works") with no agent, which is
 * what headphones and speakers do; a legacy PIN-only device will fail. */
void rpod_bt_pair(const char *path);
void rpod_bt_connect(const char *path);
void rpod_bt_disconnect(const char *path);

/* Removes the pairing and BlueZ's record of the device. */
void rpod_bt_forget(const char *path);

#endif /* RPOD_BLUETOOTH_H */
