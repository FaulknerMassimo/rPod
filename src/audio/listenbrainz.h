/*
 * ListenBrainz scrobbling (https://listenbrainz.org/) -- submits "now
 * playing" and "listen" events for what MPD is playing. See
 * src/audio/scrobbler.h for the piece that decides *when* to call these
 * from polled MPD status; this file only owns getting a submission to the
 * server.
 *
 * No text-entry widget exists on the click wheel UI yet, so the user token
 * comes from the web UI (Settings > ListenBrainz, through ui/control.h,
 * which saves it with rpod_lb_save_token()) or else the environment
 * (RPOD_LISTENBRAINZ_TOKEN in the simulator; an EnvironmentFile on-device --
 * see system/systemd/rpod.service).
 *
 * rPod is portable -- "off Wi-Fi" is the normal state, not an occasional
 * blip -- so "listen" events are queued to disk (queue_path below) and
 * retried until they succeed, surviving process restarts and power loss.
 * "playing_now" pings are NOT persisted: a live status ping replayed hours
 * later, once Wi-Fi is back, would just show a stale "currently playing X"
 * on the user's profile, so those stay purely best-effort/in-memory.
 */

#ifndef RPOD_LISTENBRAINZ_H
#define RPOD_LISTENBRAINZ_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct rpod_lb rpod_lb_t;

/* Always returns a live handle (NULL only on outright allocation failure),
 * mirroring rpod_visualizer_start()'s "not configured" vs "configured but
 * quiet" split -- if token is NULL or empty, the handle is simply inert:
 * every rpod_lb_* call below becomes a no-op, so callers never need to
 * branch on whether scrobbling is enabled. Spawns a background submission
 * thread when a token is present; network calls never happen on the
 * caller's thread.
 *
 * queue_path is where pending "listen" events are persisted (a small JSONL
 * file, created/appended/rewritten as needed -- its parent directory must
 * already exist). NULL disables persistence: submit_listen() falls back to
 * the old attempt-once-then-drop behavior. */
rpod_lb_t *rpod_lb_init(const char *token, const char *queue_path);

/* Enqueues a "playing_now" ping -- best-effort, fire-and-forget: if the
 * in-memory queue is full (sustained network outage) or the request
 * itself fails, it's dropped and logged, never retried. */
void rpod_lb_now_playing(rpod_lb_t *lb, const char *artist, const char *title,
                          const char *album, unsigned duration_s);

/* Records a single "listen" (a completed/qualifying play) durably: appended
 * to the queue file before any network attempt, so it survives a crash or
 * power loss even before it's sent. Retried in the background roughly once
 * a minute until it succeeds. listened_at is the unix time the track
 * *started* playing, per the ListenBrainz API. */
void rpod_lb_submit_listen(rpod_lb_t *lb, const char *artist, const char *title,
                            const char *album, unsigned duration_s, time_t listened_at);

/* Swaps the token at runtime: a new one takes over from the next
 * submission (and starts the background thread if scrobbling was off);
 * NULL or "" turns scrobbling off -- listens already queued on disk stay
 * there for whenever a token comes back. False, changing nothing, for
 * something that isn't a token (a ListenBrainz token is a UUID). */
bool rpod_lb_set_token(rpod_lb_t *lb, const char *token);

/* Whether a token is set, i.e. listens are being submitted. */
bool rpod_lb_enabled(const rpod_lb_t *lb);

/* A token saved by the web UI, which wins over the environment's: true if
 * one was saved at `path` (out is "" when it was saved as "off"), false if
 * nothing was. */
bool rpod_lb_load_token(const char *path, char *out, size_t out_size);

/* Saves `token` ("" for off) at `path`, readable by its owner only. */
bool rpod_lb_save_token(const char *path, const char *token);

/* Signals the background thread, joins it, and frees lb. Safe to call with
 * lb == NULL. */
void rpod_lb_shutdown(rpod_lb_t *lb);

#endif /* RPOD_LISTENBRAINZ_H */
