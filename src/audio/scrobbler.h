/*
 * Glues polled MPD playback status to ListenBrainz submissions
 * (src/audio/listenbrainz.h) -- decides *when* a "now playing" or "listen"
 * event should fire, per the standard ListenBrainz/Last.fm submission
 * rule: a track must run longer than 30s, and is scrobbled once it's been
 * played for at least half its duration or 4 minutes, whichever is lower.
 *
 * Which play of a track has already been scrobbled is tracked in memory,
 * which by itself doesn't survive a crash or power loss mid-track -- the
 * process would come back up, see the same track still playing past the
 * threshold, and scrobble it a second time. state_path (below) closes that
 * gap: the (track, position within it) pair is persisted every time a
 * listen is submitted, and checked against on startup.
 */

#ifndef RPOD_SCROBBLER_H
#define RPOD_SCROBBLER_H

#include "audio/listenbrainz.h"
#include "audio/mpd_client.h"

typedef struct rpod_scrobbler rpod_scrobbler_t;

/* Creates an lv_timer that polls `mpd` (the same shared connection other
 * pollers such as ui/status_bar.c and ui/screens/now_playing.c already poll
 * independently) once a second for the rest of the process's life. Does not
 * take ownership of mpd or lb -- both must outlive the returned handle.
 *
 * state_path is where the last-scrobbled (track, position) pair is
 * persisted (a couple of lines of text; parent directory must already
 * exist). NULL disables this: a restart while a track is still playing past
 * the scrobble threshold will submit it again, as it always has. */
rpod_scrobbler_t *rpod_scrobbler_create(rpod_mpd_t *mpd, rpod_lb_t *lb, const char *state_path);

#endif /* RPOD_SCROBBLER_H */
