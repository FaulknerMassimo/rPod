#include "av_sync.h"

#include "audio/sinks.h"

#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Estimates where nothing can be read, by MPD output plugin. ALSA's is
 * MPD's default buffer_time (500 ms) a little short of full -- what the
 * kernel says, when MPD's playing through it, wins. PipeWire's is its graph
 * plus a Bluetooth headset's own buffer (A2DP is 150-250 ms); pulse is the
 * simulator's desktop. Still to measure on the dev board: these are where
 * the trim starts from. */
static const struct {
    const char *plugin;
    int ms;
} k_defaults[] = {
    { "alsa", 450 },
    { "pipewire", 250 },
    { "pulse", 120 },
};


static struct {
    int trim_ms;
    char path[PATH_MAX];
} g;

void rpod_av_sync_init(const char *state_path)
{
    if (state_path == NULL || state_path[0] == '\0') {
        return;
    }
    snprintf(g.path, sizeof(g.path), "%s", state_path);
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* first run */
    }
    int ms;
    if (fscanf(f, "%d", &ms) == 1 && ms >= -RPOD_AV_SYNC_TRIM_MAX && ms <= RPOD_AV_SYNC_TRIM_MAX) {
        g.trim_ms = ms;
    }
    fclose(f);
}

int rpod_av_sync_trim_ms(void)
{
    return g.trim_ms;
}

/* Written to a temp file and renamed over the real one, as the backlight's
 * is (ui/backlight.c). */
void rpod_av_sync_set_trim_ms(int ms)
{
    if (ms < -RPOD_AV_SYNC_TRIM_MAX || ms > RPOD_AV_SYNC_TRIM_MAX || ms == g.trim_ms) {
        return;
    }
    g.trim_ms = ms;
    if (g.path[0] == '\0') {
        return;
    }
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: video sync couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%d\n", ms);
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: video sync couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

/* The first number after "key" and a colon in a /proc/asound file. */
static bool proc_field(const char *text, const char *key, long *out)
{
    for (const char *p = text; (p = strstr(p, key)) != NULL; p++) {
        if (p != text && p[-1] != '\n') {
            continue;
        }
        const char *colon = strchr(p, ':');
        if (colon == NULL) {
            return false;
        }
        char *end;
        long v = strtol(colon + 1, &end, 10);
        if (end == colon + 1) {
            return false;
        }
        *out = v;
        return true;
    }
    return false;
}

static bool read_small(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0;
}

/* What the sound card has queued, in ms, from whichever playback stream is
 * running -- only MPD plays to the card on the device (PipeWire never opens
 * it, docs/PLAN.md §6.3). -1 if none is. */
static int alsa_delay_ms(void)
{
    glob_t gl;
    if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &gl) != 0) {
        return -1;
    }
    int ms = -1;
    for (size_t i = 0; i < gl.gl_pathc && ms < 0; i++) {
        char text[1024], params[1024], path[PATH_MAX];
        long delay, rate;
        if (!read_small(gl.gl_pathv[i], text, sizeof(text)) || strstr(text, "state: RUNNING") == NULL ||
            !proc_field(text, "delay", &delay)) {
            continue;
        }
        snprintf(path, sizeof(path), "%.*s/hw_params", (int)(strrchr(gl.gl_pathv[i], '/') - gl.gl_pathv[i]),
                 gl.gl_pathv[i]);
        if (read_small(path, params, sizeof(params)) && proc_field(params, "rate", &rate) && rate > 0 && delay >= 0) {
            ms = (int)(delay * 1000 / rate);
        }
    }
    globfree(&gl);
    return ms;
}

int rpod_av_sync_delay_ms(rpod_mpd_t *mpd)
{
    rpod_mpd_output_t *outputs = NULL;
    size_t n = 0;
    int delay = 0;
    if (rpod_mpd_list_outputs(mpd, &outputs, &n)) {
        for (size_t i = 0; i < n; i++) {
            if (!outputs[i].enabled) {
                continue;
            }
            int ms = 0;
            for (size_t k = 0; k < sizeof(k_defaults) / sizeof(k_defaults[0]); k++) {
                if (strcmp(outputs[i].plugin, k_defaults[k].plugin) == 0) {
                    ms = k_defaults[k].ms;
                }
            }
            if (strcmp(outputs[i].plugin, "alsa") == 0) {
                int measured = alsa_delay_ms();
                if (measured >= 0) {
                    ms = measured;
                }
            }
            /* MPD moves on once every output has taken the sound: the
             * slowest one is the one that's heard last. */
            if (ms > delay) {
                delay = ms;
            }
        }
        rpod_mpd_free_outputs(outputs);
    }
    if (rpod_sinks_airplay_active()) {
        delay = RPOD_SINKS_AIRPLAY_DELAY_MS; /* the speaker's buffer, not the headset's */
    }
    return delay + g.trim_ms;
}
