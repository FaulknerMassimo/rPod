/*
 * The web daemon's side of rPod's settings socket (src/ui/control.h).
 */

#ifndef RPOD_WEB_CONTROL_CLIENT_H
#define RPOD_WEB_CONTROL_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* Sends one request line and returns the whole reply, malloc'd (free() it):
 * "ok\n" and maybe key=value lines, or "err <why>\n". NULL when rPod isn't
 * there to ask (not running, or busy past a couple of seconds). */
char *rpod_ctl_request(const char *sock_path, const char *line);

/* Whether a reply starts "ok". */
bool rpod_ctl_ok(const char *reply);

/* The value of `key` in a `get` reply's key=value lines. */
bool rpod_ctl_value(const char *reply, const char *key, char *out, size_t out_size);

#endif /* RPOD_WEB_CONTROL_CLIENT_H */
