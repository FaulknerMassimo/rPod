#include "video_decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frames decoded ahead: ~130 ms at 30 fps, and 600 KB at 320x240. */
#define SLOTS 4

/* A frame decoded this far behind the clock is dropped before it's even
 * converted: the decoder is catching up. */
#define LATE_MS 60

typedef enum { SLOT_FREE, SLOT_READY, SLOT_SHOWN } slot_state_t;

typedef struct {
    slot_state_t state;
    rpod_video_frame_t frame;
    uint8_t *buf;
} slot_t;

struct rpod_video {
    char *path;
    int max_w, max_h;

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    /* Changed under `lock`; these two are also read without it, by the
     * thread checking whether to stop what it's doing. */
    atomic_bool quit;
    atomic_uint seek_gen; /* bumped by every rpod_video_seek() */

    /* Under `lock`. */
    rpod_video_state_t state;
    int w, h, stride;
    unsigned duration_ms;
    slot_t slots[SLOTS];
    bool eof;
    int64_t seek_ms;
    bool seek_preview;
    int64_t clock_ms; /* the last frame_at()'s media_ms, to spot late frames */
    slot_t *shown;
};

/* --- The thread ---------------------------------------------------------------- */

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    struct SwsContext *sws;
    AVFrame *frame;
    AVPacket *pkt;
    int stream;
    AVRational tb;
    int64_t origin; /* where the sound starts, in the video's time base: MPD's 0 */
    unsigned gen;   /* the seek the frames being decoded belong to */
    int64_t skip_until_ms; /* an exact seek: frames before this aren't shown */
    bool preview;   /* a preview seek: the next frame shows at once */
} dec_t;

static int64_t frame_ms(const dec_t *d, const AVFrame *f)
{
    int64_t pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = f->pts;
    }
    if (pts == AV_NOPTS_VALUE) {
        return INT64_MIN;
    }
    return av_rescale_q(pts - d->origin, d->tb, (AVRational){ 1, 1000 });
}

/* Where the picture fits in max_w x max_h, its aspect kept, sides even. */
static void fit(int w, int h, AVRational sar, int max_w, int max_h, int *ow, int *oh)
{
    double aspect = (double)w / h;
    if (sar.num > 0 && sar.den > 0) {
        aspect *= av_q2d(sar);
    }
    if (aspect >= (double)max_w / max_h) {
        *ow = max_w;
        *oh = (int)(max_w / aspect + 1) & ~1;
    } else {
        *oh = max_h;
        *ow = (int)(max_h * aspect + 1) & ~1;
    }
    *ow = *ow < 2 ? 2 : *ow > max_w ? max_w : *ow;
    *oh = *oh < 2 ? 2 : *oh > max_h ? max_h : *oh;
}

static bool open_input(rpod_video_t *v, dec_t *d)
{
    if (avformat_open_input(&d->fmt, v->path, NULL, NULL) < 0 || avformat_find_stream_info(d->fmt, NULL) < 0) {
        fprintf(stderr, "rpod: can't open the video %s\n", v->path);
        return false;
    }
    d->stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->stream < 0) {
        fprintf(stderr, "rpod: no picture in %s\n", v->path);
        return false;
    }
    AVStream *st = d->fmt->streams[d->stream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    d->dec = codec != NULL ? avcodec_alloc_context3(codec) : NULL;
    if (d->dec == NULL || avcodec_parameters_to_context(d->dec, st->codecpar) < 0) {
        fprintf(stderr, "rpod: can't decode %s's picture\n", v->path);
        return false;
    }
    d->dec->pkt_timebase = st->time_base;
    /* One thread does rpod-convert's 320x180; anything bigger needs help. */
    d->dec->thread_count = st->codecpar->width * st->codecpar->height > 320 * 240 ? 3 : 1;
    if (avcodec_open2(d->dec, codec, NULL) < 0) {
        fprintf(stderr, "rpod: can't decode %s's picture\n", v->path);
        return false;
    }
    d->tb = st->time_base;

    /* MPD counts from the start of the sound (its ffmpeg decoder plugin's
     * start_time), so frames do too. */
    int audio = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, d->stream, NULL, 0);
    d->origin = 0;
    if (audio >= 0 && d->fmt->streams[audio]->start_time != AV_NOPTS_VALUE) {
        d->origin = av_rescale_q(d->fmt->streams[audio]->start_time, d->fmt->streams[audio]->time_base, d->tb);
    }

    int w, h;
    fit(d->dec->width, d->dec->height, av_guess_sample_aspect_ratio(d->fmt, st, NULL), v->max_w, v->max_h, &w, &h);
    /* Rows padded and aligned: swscale's SIMD writes past the end of a row
     * that isn't a multiple of its blocks. */
    int stride = FFALIGN(w * 2, 64);
    pthread_mutex_lock(&v->lock);
    for (int i = 0; i < SLOTS; i++) {
        v->slots[i].buf = av_malloc((size_t)stride * (size_t)(h + 1));
        if (v->slots[i].buf == NULL) {
            pthread_mutex_unlock(&v->lock);
            return false;
        }
        memset(v->slots[i].buf, 0, (size_t)stride * (size_t)h);
    }
    v->w = w;
    v->h = h;
    v->stride = stride;
    v->duration_ms = d->fmt->duration != AV_NOPTS_VALUE ? (unsigned)(d->fmt->duration / 1000) : 0;
    pthread_mutex_unlock(&v->lock);

    d->frame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    return d->frame != NULL && d->pkt != NULL;
}

/* A free slot, waiting for one if need be. NULL to stop: quitting, or a
 * seek came in meanwhile. Called and returns with the lock held. */
static slot_t *free_slot(rpod_video_t *v, const dec_t *d)
{
    for (;;) {
        if (v->quit || v->seek_gen != d->gen) {
            return NULL;
        }
        for (int i = 0; i < SLOTS; i++) {
            if (v->slots[i].state == SLOT_FREE) {
                return &v->slots[i];
            }
        }
        pthread_cond_wait(&v->cond, &v->lock);
    }
}

/* Puts a decoded frame up for showing, converting it into a free slot --
 * unless it's before an exact seek's target or already late. */
static void publish(rpod_video_t *v, dec_t *d)
{
    AVFrame *f = d->frame;
    int64_t ms = frame_ms(d, f);
    if (ms == INT64_MIN || (!d->preview && ms < d->skip_until_ms)) {
        return;
    }
    pthread_mutex_lock(&v->lock);
    if (!d->preview && v->shown != NULL && v->clock_ms != INT64_MIN && v->clock_ms - ms > LATE_MS) {
        pthread_mutex_unlock(&v->lock);
        return;
    }
    slot_t *s = free_slot(v, d);
    pthread_mutex_unlock(&v->lock);
    if (s == NULL) {
        return;
    }

    d->sws = sws_getCachedContext(d->sws, f->width, f->height, f->format, v->w, v->h, AV_PIX_FMT_RGB565,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (d->sws == NULL) {
        return;
    }
    uint8_t *dst[4] = { s->buf, NULL, NULL, NULL };
    int stride[4] = { v->stride, 0, 0, 0 };
    sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride);

    pthread_mutex_lock(&v->lock);
    if (v->seek_gen == d->gen && !v->quit) {
        s->frame = (rpod_video_frame_t){
            .pixels = s->buf, .w = v->w, .h = v->h, .stride = v->stride,
            /* A preview's keyframe is due now, whatever its time. */
            .pts_ms = d->preview ? INT64_MIN : ms,
        };
        s->state = SLOT_READY;
    }
    pthread_mutex_unlock(&v->lock);
    d->preview = false;
}

static void drain(rpod_video_t *v, dec_t *d)
{
    while (avcodec_receive_frame(d->dec, d->frame) >= 0) {
        publish(v, d);
        av_frame_unref(d->frame);
        if (v->seek_gen != d->gen) {
            return; /* a seek came in: the rest are from before it */
        }
    }
}

/* Takes up a seek that's come in, if one has. */
static void take_seek(rpod_video_t *v, dec_t *d)
{
    pthread_mutex_lock(&v->lock);
    if (v->seek_gen == d->gen) {
        pthread_mutex_unlock(&v->lock);
        return;
    }
    d->gen = v->seek_gen;
    int64_t target = v->seek_ms < 0 ? 0 : v->seek_ms;
    d->preview = v->seek_preview;
    d->skip_until_ms = d->preview ? INT64_MIN : target;
    v->eof = false;
    pthread_mutex_unlock(&v->lock);

    int64_t ts = d->origin + av_rescale_q(target, (AVRational){ 1, 1000 }, d->tb);
    if (av_seek_frame(d->fmt, d->stream, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        av_seek_frame(d->fmt, d->stream, d->origin, AVSEEK_FLAG_BACKWARD);
    }
    avcodec_flush_buffers(d->dec);
}

static void *decode_thread(void *arg)
{
    rpod_video_t *v = arg;
    dec_t d = { .stream = -1, .skip_until_ms = INT64_MIN };
    bool ok = open_input(v, &d);
    pthread_mutex_lock(&v->lock);
    v->state = ok ? RPOD_VIDEO_READY : RPOD_VIDEO_FAILED;
    d.gen = v->seek_gen;
    pthread_mutex_unlock(&v->lock);

    while (ok) {
        take_seek(v, &d);
        pthread_mutex_lock(&v->lock);
        while (v->eof && !v->quit && v->seek_gen == d.gen) {
            pthread_cond_wait(&v->cond, &v->lock); /* the end: until a seek back */
        }
        bool quit = v->quit;
        pthread_mutex_unlock(&v->lock);
        if (quit) {
            break;
        }
        if (v->seek_gen != d.gen) {
            continue;
        }

        int r = av_read_frame(d.fmt, d.pkt);
        if (r < 0) {
            avcodec_send_packet(d.dec, NULL); /* the frames still inside it */
            drain(v, &d);
            pthread_mutex_lock(&v->lock);
            if (v->seek_gen == d.gen) {
                v->eof = true;
            }
            pthread_mutex_unlock(&v->lock);
            continue;
        }
        if (d.pkt->stream_index == d.stream) {
            /* A damaged packet is skipped, as players do. */
            if (avcodec_send_packet(d.dec, d.pkt) >= 0) {
                drain(v, &d);
            }
        }
        av_packet_unref(d.pkt);
    }

    sws_freeContext(d.sws);
    av_packet_free(&d.pkt);
    av_frame_free(&d.frame);
    avcodec_free_context(&d.dec);
    avformat_close_input(&d.fmt);
    return NULL;
}

/* --- The LVGL thread's side --------------------------------------------------- */

rpod_video_t *rpod_video_open(const char *path, int max_w, int max_h)
{
    rpod_video_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        return NULL;
    }
    v->path = strdup(path);
    v->max_w = max_w;
    v->max_h = max_h;
    v->clock_ms = INT64_MIN;
    pthread_mutex_init(&v->lock, NULL);
    pthread_cond_init(&v->cond, NULL);
    if (v->path == NULL || pthread_create(&v->thread, NULL, decode_thread, v) != 0) {
        free(v->path);
        free(v);
        return NULL;
    }
    return v;
}

void rpod_video_close(rpod_video_t *v)
{
    if (v == NULL) {
        return;
    }
    pthread_mutex_lock(&v->lock);
    v->quit = true;
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
    pthread_join(v->thread, NULL);
    for (int i = 0; i < SLOTS; i++) {
        av_free(v->slots[i].buf);
    }
    pthread_cond_destroy(&v->cond);
    pthread_mutex_destroy(&v->lock);
    free(v->path);
    free(v);
}

rpod_video_state_t rpod_video_state(rpod_video_t *v, int *w, int *h, unsigned *duration_ms)
{
    pthread_mutex_lock(&v->lock);
    rpod_video_state_t s = v->state;
    if (w != NULL) {
        *w = v->w;
    }
    if (h != NULL) {
        *h = v->h;
    }
    if (duration_ms != NULL) {
        *duration_ms = v->duration_ms;
    }
    pthread_mutex_unlock(&v->lock);
    return s;
}

const rpod_video_frame_t *rpod_video_frame_at(rpod_video_t *v, int64_t media_ms)
{
    pthread_mutex_lock(&v->lock);
    v->clock_ms = media_ms;
    slot_t *best = NULL;
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &v->slots[i];
        if (s->state == SLOT_READY && s->frame.pts_ms <= media_ms &&
            (best == NULL || s->frame.pts_ms > best->frame.pts_ms)) {
            best = s;
        }
    }
    if (best != NULL) {
        /* Everything due before it is late now: gone. */
        for (int i = 0; i < SLOTS; i++) {
            slot_t *s = &v->slots[i];
            if (s != best && (s->state == SLOT_SHOWN || (s->state == SLOT_READY && s->frame.pts_ms <= best->frame.pts_ms))) {
                s->state = SLOT_FREE;
            }
        }
        best->state = SLOT_SHOWN;
        v->shown = best;
        pthread_cond_broadcast(&v->cond);
    }
    pthread_mutex_unlock(&v->lock);
    return best != NULL ? &best->frame : NULL;
}

const rpod_video_frame_t *rpod_video_current(rpod_video_t *v)
{
    pthread_mutex_lock(&v->lock);
    const rpod_video_frame_t *f = v->shown != NULL ? &v->shown->frame : NULL;
    pthread_mutex_unlock(&v->lock);
    return f;
}

void rpod_video_seek(rpod_video_t *v, int64_t media_ms, bool preview)
{
    pthread_mutex_lock(&v->lock);
    v->seek_gen++;
    v->seek_ms = media_ms;
    v->seek_preview = preview;
    v->eof = false;
    /* What was waiting belongs to the old position; what's up stays up. */
    for (int i = 0; i < SLOTS; i++) {
        if (v->slots[i].state == SLOT_READY) {
            v->slots[i].state = SLOT_FREE;
        }
    }
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
}

bool rpod_video_ended(rpod_video_t *v)
{
    pthread_mutex_lock(&v->lock);
    bool ended = v->state == RPOD_VIDEO_FAILED;
    if (v->state == RPOD_VIDEO_READY && v->eof) {
        ended = true;
        for (int i = 0; i < SLOTS; i++) {
            if (v->slots[i].state == SLOT_READY) {
                ended = false;
            }
        }
    }
    pthread_mutex_unlock(&v->lock);
    return ended;
}
