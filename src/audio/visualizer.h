/*
 * Real audio-reactive spectrum for the Now Playing screen's visualizer.
 * rPod otherwise never touches audio itself -- MPD owns decode/mixing
 * entirely (docs/PLAN.md §6.1) -- but MPD supports a "fifo" audio_output
 * specifically so a client can tap the PCM stream for exactly this kind of
 * side use. This reads that FIFO on a background thread, runs a small FFT,
 * and exposes a handful of smoothed frequency-band levels for the UI to
 * poll. See tools/sim/mpd-dev.conf.in and docs/PLAN.md §6.2 for the
 * matching MPD-side "fifo" output config (format "44100:16:2" -- must
 * match RPOD_VIS_SAMPLE_RATE below).
 */

#ifndef RPOD_VISUALIZER_H
#define RPOD_VISUALIZER_H

#define RPOD_VIS_BANDS 6

/* Must match the `format` string on MPD's fifo audio_output. */
#define RPOD_VIS_SAMPLE_RATE 44100
#define RPOD_VIS_CHANNELS    2

typedef struct rpod_visualizer rpod_visualizer_t;

/* Spawns a background thread reading raw S16LE PCM from the FIFO at
 * `fifo_path` and computing a live spectrum from it. Always returns a live
 * handle (NULL only on outright allocation/thread-creation failure) even
 * if the fifo doesn't exist yet or has no writer -- the thread just polls
 * and reports all-zero levels until MPD starts feeding it, so callers
 * don't need a separate fallback path for "not configured" versus
 * "configured but silent" (e.g. paused). Free with rpod_visualizer_stop(). */
rpod_visualizer_t *rpod_visualizer_start(const char *fifo_path);
void rpod_visualizer_stop(rpod_visualizer_t *vis);

/* Fills out[RPOD_VIS_BANDS] with the current smoothed band levels, each in
 * [0, 1], low frequency first. Cheap (a mutex-protected copy) -- meant to
 * be polled from a UI timer. Safe to call with vis == NULL (e.g. if start
 * failed) -- fills with zeros. */
void rpod_visualizer_get_levels(rpod_visualizer_t *vis, float out[RPOD_VIS_BANDS]);

#endif /* RPOD_VISUALIZER_H */
