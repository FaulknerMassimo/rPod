#include "visualizer.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 1024 real samples @ 44.1kHz = ~23ms/block (~43 updates/sec) and a ~43Hz
 * bin width -- coarse at the very bottom of the bass band, fine everywhere
 * else, and plenty responsive for a decorative meter. Must be a power of
 * two (fft_forward() below is a radix-2 Cooley-Tukey). */
#define FFT_SIZE  1024
#define FFT_BINS  (FFT_SIZE / 2)
#define FRAME_BYTES (RPOD_VIS_CHANNELS * (int)sizeof(int16_t))

/* Peak-meter ballistics: bars jump up fast, fall back slowly. Per-block
 * (~23ms) mix factors, not physical units -- tuned by eye. */
#define ATTACK  0.55f
#define RELEASE 0.15f

/* Per-band auto-gain: each band tracks its own recent peak and normalizes
 * against it, so quiet tracks/passages still produce visible movement and
 * loud ones don't peg every bar at max. AGC_DECAY is the peak's per-block
 * forgetting factor (~a few seconds to fully forget); AGC_MIN floors it so
 * silence doesn't divide-by-near-zero into noise-amplified noise. */
#define AGC_DECAY 0.995f
#define AGC_MIN   0.02f

struct rpod_visualizer {
    char fifo_path[512];
    pthread_t thread;
    volatile bool stop;

    pthread_mutex_t mutex;
    float levels[RPOD_VIS_BANDS];
};

/* --- FFT: iterative radix-2 Cooley-Tukey, in place, twiddles precomputed
 * once at thread start (this runs every ~23ms for the life of the Now
 * Playing screen, not a one-shot -- worth the small setup cost). --- */

static unsigned reverse_bits(unsigned v, int bits)
{
    unsigned r = 0;
    for (int i = 0; i < bits; i++) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

static void fft_forward(float *re, float *im, const float *tw_re, const float *tw_im)
{
    const int bits = 10; /* log2(FFT_SIZE), FFT_SIZE == 1024 */

    for (unsigned i = 0; i < FFT_SIZE; i++) {
        unsigned j = reverse_bits(i, bits);
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int size = 2; size <= FFT_SIZE; size <<= 1) {
        int half = size / 2;
        int step = FFT_SIZE / size;
        for (int start = 0; start < FFT_SIZE; start += size) {
            for (int k = 0; k < half; k++) {
                float tre = tw_re[k * step];
                float tim = tw_im[k * step];
                int a = start + k;
                int b = start + k + half;
                float xre = re[b] * tre - im[b] * tim;
                float xim = re[b] * tim + im[b] * tre;
                re[b] = re[a] - xre;
                im[b] = im[a] - xim;
                re[a] += xre;
                im[a] += xim;
            }
        }
    }
}

/* Band edges as FFT bin indices, low to high. Hand-picked roughly
 * log-spaced in Hz (~30/120/300/700/2000/6000/16000 Hz) and converted via
 * bin = hz * FFT_SIZE / RPOD_VIS_SAMPLE_RATE -- not a principled
 * perceptual scale, just enough spread that bass/mid/treble move
 * independently on a small 6-bar display. */
static const int band_bin_edges[RPOD_VIS_BANDS + 1] = { 1, 3, 7, 16, 46, 139, 371 };

static void process_block(rpod_visualizer_t *vis, const float *mono, const float *window,
                          const float *tw_re, const float *tw_im, float *agc_max, float *smoothed)
{
    float re[FFT_SIZE];
    float im[FFT_SIZE];
    for (int i = 0; i < FFT_SIZE; i++) {
        re[i] = mono[i] * window[i];
        im[i] = 0.0f;
    }

    fft_forward(re, im, tw_re, tw_im);

    float raw[RPOD_VIS_BANDS];
    for (int b = 0; b < RPOD_VIS_BANDS; b++) {
        float energy = 0.0f;
        int count = 0;
        int lo = band_bin_edges[b];
        int hi = band_bin_edges[b + 1];
        if (hi > FFT_BINS) {
            hi = FFT_BINS;
        }
        for (int k = lo; k < hi; k++) {
            energy += sqrtf(re[k] * re[k] + im[k] * im[k]);
            count++;
        }
        if (count > 0) {
            energy /= (float)count;
        }

        agc_max[b] = fmaxf(agc_max[b] * AGC_DECAY, fmaxf(energy, AGC_MIN));
        raw[b] = energy / agc_max[b];
        if (raw[b] > 1.0f) {
            raw[b] = 1.0f;
        }

        float rate = raw[b] > smoothed[b] ? ATTACK : RELEASE;
        smoothed[b] += (raw[b] - smoothed[b]) * rate;
        if (smoothed[b] < 0.0f) {
            smoothed[b] = 0.0f;
        }
    }

    pthread_mutex_lock(&vis->mutex);
    memcpy(vis->levels, smoothed, sizeof(vis->levels));
    pthread_mutex_unlock(&vis->mutex);
}

/* Called whenever a poll/read iteration comes back with no new audio (fifo
 * not open yet, no writer attached, or just an idle timeout -- MPD's fifo
 * output stops producing data while paused/stopped, it doesn't get closed,
 * so read() simply never has anything for us). Without this, `smoothed`
 * would just sit frozen at whatever it was when the music stopped instead
 * of settling back down to the idle floor. */
static void decay_idle(rpod_visualizer_t *vis, float *smoothed)
{
    for (int b = 0; b < RPOD_VIS_BANDS; b++) {
        smoothed[b] *= 0.6f;
        if (smoothed[b] < 0.005f) {
            smoothed[b] = 0.0f;
        }
    }
    pthread_mutex_lock(&vis->mutex);
    memcpy(vis->levels, smoothed, sizeof(vis->levels));
    pthread_mutex_unlock(&vis->mutex);
}

static void *thread_main(void *arg)
{
    rpod_visualizer_t *vis = arg;

    float *window = malloc(FFT_SIZE * sizeof(float));
    float *tw_re = malloc(FFT_BINS * sizeof(float));
    float *tw_im = malloc(FFT_BINS * sizeof(float));
    float *mono = malloc(FFT_SIZE * sizeof(float));
    if (window == NULL || tw_re == NULL || tw_im == NULL || mono == NULL) {
        free(window); free(tw_re); free(tw_im); free(mono);
        return NULL;
    }

    for (int i = 0; i < FFT_SIZE; i++) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(FFT_SIZE - 1)));
    }
    for (int k = 0; k < FFT_BINS; k++) {
        float angle = -2.0f * (float)M_PI * (float)k / (float)FFT_SIZE;
        tw_re[k] = cosf(angle);
        tw_im[k] = sinf(angle);
    }

    float agc_max[RPOD_VIS_BANDS];
    float smoothed[RPOD_VIS_BANDS];
    for (int b = 0; b < RPOD_VIS_BANDS; b++) {
        agc_max[b] = AGC_MIN;
        smoothed[b] = 0.0f;
    }

    int mono_count = 0;
    int fd = -1;
    uint8_t chunk[4096];
    uint8_t pending[FRAME_BYTES];
    int pending_len = 0;

    while (!vis->stop) {
        if (fd < 0) {
            fd = open(vis->fifo_path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                decay_idle(vis, smoothed);
                poll(NULL, 0, 200); /* nfds=0: just a sleep, no fd needed */
                continue;
            }
            pending_len = 0;
            mono_count = 0;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (vis->stop) {
            break;
        }
        if (pr <= 0) {
            /* Timeout (also our chance to notice `stop`) or interrupted.
             * This is the common "paused" path: MPD's fifo output just
             * stops writing rather than closing, so most idle time is
             * spent right here. */
            decay_idle(vis, smoothed);
            continue;
        }
        if (pfd.revents & POLLHUP) {
            /* No writer currently attached (MPD not running/output
             * disabled) -- poll() returns immediately with POLLHUP set
             * regardless of the timeout, so without an explicit close+wait
             * here this would busy-spin instead of idling. */
            close(fd);
            fd = -1;
            decay_idle(vis, smoothed);
            poll(NULL, 0, 200);
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }

        memcpy(chunk, pending, (size_t)pending_len);
        ssize_t n = read(fd, chunk + pending_len, sizeof(chunk) - (size_t)pending_len);
        if (n <= 0) {
            if (n < 0 && errno == EAGAIN) {
                continue;
            }
            close(fd); /* EOF or real error -- reopen and retry */
            fd = -1;
            decay_idle(vis, smoothed);
            continue;
        }

        size_t total = (size_t)pending_len + (size_t)n;
        size_t frames = total / (size_t)FRAME_BYTES;
        size_t used = frames * (size_t)FRAME_BYTES;
        const int16_t *samples = (const int16_t *)chunk;

        for (size_t f = 0; f < frames; f++) {
            int16_t l = samples[f * 2 + 0];
            int16_t r = samples[f * 2 + 1];
            mono[mono_count++] = ((float)l + (float)r) * 0.5f / 32768.0f;
            if (mono_count == FFT_SIZE) {
                process_block(vis, mono, window, tw_re, tw_im, agc_max, smoothed);
                mono_count = 0;
            }
        }

        pending_len = (int)(total - used);
        if (pending_len > 0) {
            memcpy(pending, chunk + used, (size_t)pending_len);
        }
    }

    if (fd >= 0) {
        close(fd);
    }
    free(window);
    free(tw_re);
    free(tw_im);
    free(mono);
    return NULL;
}

rpod_visualizer_t *rpod_visualizer_start(const char *fifo_path)
{
    rpod_visualizer_t *vis = calloc(1, sizeof(*vis));
    if (vis == NULL) {
        return NULL;
    }
    snprintf(vis->fifo_path, sizeof(vis->fifo_path), "%s", fifo_path);
    pthread_mutex_init(&vis->mutex, NULL);

    if (pthread_create(&vis->thread, NULL, thread_main, vis) != 0) {
        pthread_mutex_destroy(&vis->mutex);
        free(vis);
        return NULL;
    }
    return vis;
}

void rpod_visualizer_stop(rpod_visualizer_t *vis)
{
    if (vis == NULL) {
        return;
    }
    vis->stop = true;
    pthread_join(vis->thread, NULL);
    pthread_mutex_destroy(&vis->mutex);
    free(vis);
}

void rpod_visualizer_get_levels(rpod_visualizer_t *vis, float out[RPOD_VIS_BANDS])
{
    if (vis == NULL) {
        memset(out, 0, sizeof(float) * RPOD_VIS_BANDS);
        return;
    }
    pthread_mutex_lock(&vis->mutex);
    memcpy(out, vis->levels, sizeof(vis->levels));
    pthread_mutex_unlock(&vis->mutex);
}
