#include "scrobbler.h"

#include "lvgl.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POLL_PERIOD_MS 1000

/* ListenBrainz/Last.fm submission rule: never scrobble anything 30s or
 * shorter, and otherwise scrobble once played for at least half its
 * duration or 4 minutes, whichever comes first. */
#define MIN_SCROBBLE_DURATION_S 30
#define MAX_SCROBBLE_THRESHOLD_S 240

/* A track detected right after startup counts as "the same play we already
 * scrobbled" (not a fresh one) if its uri matches the persisted record and
 * its current position (elapsed_s) hasn't gone backwards past this grace
 * relative to the position we'd already scrobbled at. Comparing track
 * *position* rather than wall-clock time is what makes this correct for a
 * short track replayed immediately after finishing: elapsed_s for that
 * fresh play starts back at ~0, nowhere near the (>= 30s, by construction
 * of MIN_SCROBBLE_DURATION_S/the elapsed threshold below) position the
 * previous play was scrobbled at -- so it's never mistaken for a resume.
 * The small grace absorbs MPD's own state_file save granularity: if MPD
 * itself also restarted (full power loss, not just this process crashing)
 * and resumed from a slightly stale saved position, elapsed_s could come
 * back a few seconds behind where it was at the moment we scrobbled. */
#define RESUME_ELAPSED_GRACE_S 5

struct rpod_scrobbler {
    rpod_mpd_t *mpd;
    rpod_lb_t *lb;
    lv_timer_t *timer;
    char state_path[512]; /* empty -- crash-recovery persistence disabled */

    char current_uri[512];
    time_t started_at;
    bool now_playing_sent;
    bool listen_sent;

    /* The (track, position) a listen was last actually submitted for --
     * loaded from state_path at startup, and rewritten there every time a
     * new listen is decided. */
    char last_scrobbled_uri[512];
    unsigned last_scrobbled_elapsed_s;
};

static void load_state(rpod_scrobbler_t *s)
{
    if (s->state_path[0] == '\0') {
        return;
    }
    FILE *f = fopen(s->state_path, "r");
    if (f == NULL) {
        return; /* no prior state -- normal before the first listen ever */
    }

    char uri[sizeof(s->last_scrobbled_uri)] = { 0 };
    unsigned long elapsed = 0;
    if (fgets(uri, sizeof(uri), f) != NULL) {
        uri[strcspn(uri, "\n")] = '\0';
        char line2[32];
        if (fgets(line2, sizeof(line2), f) != NULL) {
            elapsed = strtoul(line2, NULL, 10);
        }
    }
    fclose(f);

    snprintf(s->last_scrobbled_uri, sizeof(s->last_scrobbled_uri), "%s", uri);
    s->last_scrobbled_elapsed_s = (unsigned)elapsed;
}

static void save_state(rpod_scrobbler_t *s)
{
    if (s->state_path[0] == '\0') {
        return;
    }
    FILE *f = fopen(s->state_path, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: scrobbler couldn't write state file %s: %s\n",
                s->state_path, strerror(errno));
        return;
    }
    fprintf(f, "%s\n%u\n", s->last_scrobbled_uri, s->last_scrobbled_elapsed_s);
    fclose(f);
}

static void poll_cb(lv_timer_t *timer)
{
    rpod_scrobbler_t *s = lv_timer_get_user_data(timer);

    rpod_mpd_status_t status;
    if (!rpod_mpd_get_status_settled(s->mpd, &status) || status.uri[0] == '\0') {
        s->current_uri[0] = '\0';
        return;
    }

    if (strcmp(status.uri, s->current_uri) != 0) {
        /* New song -- reset per-song tracking. started_at is backdated by
         * the already-elapsed amount so a song caught mid-play (app start,
         * MPD reconnect) still scrobbles at roughly the right threshold and
         * with a roughly-correct listened_at. */
        snprintf(s->current_uri, sizeof(s->current_uri), "%s", status.uri);
        s->started_at = time(NULL) - (time_t)status.elapsed_s;

        if (strcmp(s->current_uri, s->last_scrobbled_uri) == 0 &&
            status.elapsed_s + RESUME_ELAPSED_GRACE_S >= s->last_scrobbled_elapsed_s) {
            /* Same play we already scrobbled before a crash/restart --
             * don't submit it again. */
            s->now_playing_sent = true;
            s->listen_sent = true;
        } else {
            s->now_playing_sent = false;
            s->listen_sent = false;
        }
    }

    if (status.state != RPOD_MPD_STATE_PLAY) {
        return;
    }

    if (!s->now_playing_sent) {
        rpod_lb_now_playing(s->lb, status.artist, status.title, status.album, status.duration_s);
        s->now_playing_sent = true;
    }

    if (!s->listen_sent && status.duration_s >= MIN_SCROBBLE_DURATION_S) {
        unsigned threshold = status.duration_s / 2;
        if (threshold > MAX_SCROBBLE_THRESHOLD_S) {
            threshold = MAX_SCROBBLE_THRESHOLD_S;
        }
        if (status.elapsed_s >= threshold) {
            rpod_lb_submit_listen(s->lb, status.artist, status.title, status.album,
                                   status.duration_s, s->started_at);
            s->listen_sent = true;

            snprintf(s->last_scrobbled_uri, sizeof(s->last_scrobbled_uri), "%s", s->current_uri);
            s->last_scrobbled_elapsed_s = status.elapsed_s;
            save_state(s);
        }
    }
}

rpod_scrobbler_t *rpod_scrobbler_create(rpod_mpd_t *mpd, rpod_lb_t *lb, const char *state_path)
{
    rpod_scrobbler_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->mpd = mpd;
    s->lb = lb;
    if (state_path != NULL) {
        snprintf(s->state_path, sizeof(s->state_path), "%s", state_path);
    }
    load_state(s);
    s->timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, s);
    return s;
}
