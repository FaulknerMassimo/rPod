/*
 * The settings rPod itself owns, read and changed from outside the process:
 * the web UI's daemon (web/, docs/PLAN.md §8.6) talks to this over a Unix
 * socket. Everything happens on the LVGL thread -- the socket is polled
 * from an lv_timer, like the click wheel's and BlueZ's -- so a change goes
 * through the same setters a Settings screen uses, takes effect at once and
 * persists the usual way.
 *
 * One request per connection, one line each way and then key=value lines:
 *
 *   get                    ok, then every setting as key=value lines
 *   set <key> <value>      ok, or "err <why>"
 *   reset wheel            ok: the wheel's tuning back to its defaults
 *
 * Keys: theme, backlight (seconds, 0 = always on), sleep (minutes, 0 =
 * off), alarm (on/off), alarm_time (HH:MM), alarm_sound (a playlist, or
 * nothing for Shuffle Songs), wheel.<param> (input/wheel_tuning.h) and
 * listenbrainz_token (write-only; `get` says only whether it's on). The
 * value runs to the end of the line, so it can hold spaces.
 *
 * The socket is the only access control: it's created 0660, in a directory
 * only rPod's user can reach on device (rpod.service's RuntimeDirectory).
 */

#ifndef RPOD_CONTROL_H
#define RPOD_CONTROL_H

#include "audio/listenbrainz.h"

/* Starts listening at sock_path (NULL or "": not at all). `lb` is the
 * scrobbler's ListenBrainz handle, whose token a `set` swaps and saves at
 * lb_token_path. Call once, after the settings it serves are initialised. */
void rpod_control_init(const char *sock_path, rpod_lb_t *lb, const char *lb_token_path);

#endif /* RPOD_CONTROL_H */
