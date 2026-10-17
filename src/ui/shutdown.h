/*
 * Turning rPod off (docs/PLAN.md §8.3): Settings > Shut Down, and Settings >
 * Auto-Off -- after a while with nothing happening, rPod powers itself off
 * rather than sit asleep running the battery down. Off is off:
 * logind powers the Pi down, the PiSugar cuts its power once it has halted
 * (system/pisugar/), and the PiSugar's button turns it back on. The battery
 * running out ends the same way (src/app.c).
 *
 * Nothing happening means no wheel or button, nothing playing (a video's
 * sound included), and the web UI not in use -- rpod-web says when it is,
 * over the settings socket (rpod_shutdown_activity()), while it's uploading,
 * converting or being clicked around. And only on battery: on USB power
 * there's none to save. Never while an alarm is set: off, it couldn't
 * ring.
 *
 * The timeout persists to a one-line file, like the backlight's.
 */

#ifndef RPOD_SHUTDOWN_H
#define RPOD_SHUTDOWN_H

#include "audio/mpd_client.h"
#include "input/input.h"

#include <stdbool.h>

/* The timeout that never shuts down. */
#define RPOD_SHUTDOWN_NEVER 0u

/* Watches `in`, `mpd` and the alarm for idle time. state_path is where the
 * timeout persists (parent directory must exist); NULL keeps it in memory
 * only. Unless `for_real`, shutting down only says it would and puts the
 * screen back -- the simulator, which would be powering off the desktop.
 * Call once, after lv_init() and rpod_alarm_init(). */
void rpod_shutdown_init(rpod_input_t *in, rpod_mpd_t *mpd, const char *state_path, bool for_real);

/* Minutes with nothing happening before rPod shuts itself down, or
 * RPOD_SHUTDOWN_NEVER. */
unsigned rpod_shutdown_idle_min(void);
void rpod_shutdown_set_idle_min(unsigned minutes);

/* Something's using rPod that the wheel doesn't see: the web UI. Counts as
 * activity, now. */
void rpod_shutdown_activity(void);

/* Shuts down now. With `msg`, it's put over everything first -- for when
 * someone's watching -- and the screen woken for it. */
void rpod_shutdown_now(const char *msg);

#endif /* RPOD_SHUTDOWN_H */
