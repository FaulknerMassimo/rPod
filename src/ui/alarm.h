/*
 * Extras > Clock > Alarm Clock (docs/PLAN.md §8.5): music at a set time every
 * day it's on -- the whole library shuffled, or a playlist -- faded in from
 * quiet. When it goes off it starts the music, then calls ring(), which the
 * app uses to wake the screen and show the alarm (ui/screens/clock_screens.h)
 * for snoozing or stopping. The settings persist to a small file.
 *
 * The time is the system's, as the status bar's clock is: rPod has no
 * real-time clock, so it's right once NTP has set it over Wi-Fi.
 */

#ifndef RPOD_ALARM_H
#define RPOD_ALARM_H

#include "audio/mpd_client.h"

#include <stdbool.h>
#include <time.h>

#define RPOD_ALARM_SNOOZE_MIN 9

/* `mpd` must outlive it; state_path is where the settings persist (parent
 * directory must exist; NULL keeps them in memory only). Call once, after
 * lv_init() and rpod_volume_memory_init(). */
void rpod_alarm_init(rpod_mpd_t *mpd, const char *state_path, void (*ring)(void *ctx), void *ctx);

bool rpod_alarm_enabled(void);
void rpod_alarm_set_enabled(bool on);

/* 24-hour. */
void rpod_alarm_time(int *hour, int *minute);
void rpod_alarm_set_time(int hour, int minute);

/* What it plays: a stored playlist's name, or "" for Shuffle Songs. */
const char *rpod_alarm_sound(void);
void rpod_alarm_set_sound(const char *playlist);

/* Going off: from ring() until it's snoozed, stopped or dismissed. */
bool rpod_alarm_ringing(void);

/* Pauses the music, to go off again RPOD_ALARM_SNOOZE_MIN minutes from now. */
void rpod_alarm_snooze(void);

/* Pauses the music: done until its time tomorrow. Cancels a snooze. */
void rpod_alarm_stop(void);

/* Done until tomorrow, but the music plays on. */
void rpod_alarm_dismiss(void);

/* When a snooze will go off, or 0 with none waiting. */
time_t rpod_alarm_snoozed_until(void);

#endif /* RPOD_ALARM_H */
