/*
 * rpod-convert: turns a video into the one kind rPod plays (docs/PLAN.md
 * §8.7) -- the iPod's way, where iTunes made an "iPod version" of a movie
 * before it was synced.
 *
 *   rpod-convert [--title T] [--poster P.jpg] INPUT OUTPUT.mp4
 *
 * The result fits the 320x240 screen (aspect kept, square pixels, phone
 * rotation applied), runs at 30 fps or less, and is H.264 with stereo AAC at
 * 44.1 kHz in an MP4 with its index up front. It's small (a few MB a
 * minute), cheap to decode on the Pi Zero 2 W while the panel is being fed,
 * and plain enough that MPD plays its sound (rPod's playback clock) and any
 * browser plays the whole thing. A video without sound gets a silent track,
 * so MPD has something to play. --poster also writes a frame from a tenth
 * of the way in as a JPEG, for the device's and the web UI's lists.
 *
 * Run by rpod-web after an upload (web/videos.c), niced, one at a time; its
 * progress goes to stdout as lines rpod-web reads:
 *
 *   duration <seconds>     once, if the input says (0 if it doesn't)
 *   time <seconds>         how far it's got, every half second or so
 *   done                   finished: OUTPUT is complete
 *
 * Errors go to stderr, the last line "error: <what to tell the user>", and
 * the exit status is 1. OUTPUT is left half-written on failure; the caller
 * removes it.
 *
 * Decoding the source is most of the work -- a 1080p H.264 one goes at
 * around real time on the Pi's four A53s, HEVC slower. Going 1080p down to
 * 320 wide hides any blocking, so for a source at least twice the output's
 * size the deblocking filter is skipped, which saves a fifth of it.
 */

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <jpeglib.h>

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The screen (docs/PLAN.md §5). */
#define OUT_MAX_W 320
#define OUT_MAX_H 240
#define OUT_MAX_FPS 30.5 /* 29.97 and 30 stay as they are */

#define AUDIO_RATE 44100
#define AUDIO_BITRATE 160000
#define AUDIO_FRAME 1024

/* A keyframe this often: a seek or a scan lands on one (ui/screens/video_screens.c). */
#define KEYFRAME_S 2

/* x264's quality, and a cap so a busy scene can't spike the bitrate. */
#define VIDEO_CRF "23"
#define VIDEO_MAXRATE 900000
#define VIDEO_BUFSIZE 1800000

#define POSTER_FRACTION 0.1
#define POSTER_MAX_S 30.0
#define POSTER_QUALITY 85

#define PROGRESS_EVERY_S 0.5

typedef struct {
    AVFormatContext *ifmt;
    AVFormatContext *ofmt;
    int64_t start; /* the input's start, AV_TIME_BASE units: output time 0 */

    int vin;
    AVCodecContext *vdec;
    AVFilterGraph *vgraph;
    AVFilterContext *vsrc, *vsink;
    AVCodecContext *venc;
    AVStream *vout;
    int64_t last_vpts; /* the last frame sent to the encoder, encoder time base */

    int ain; /* -1: no sound, so silence */
    AVCodecContext *adec;
    AVFilterGraph *agraph;
    AVFilterContext *asrc, *asink;
    AVCodecContext *aenc;
    AVStream *aout;
    int64_t silence_pts; /* the next silent sample, 1/AUDIO_RATE */

    const char *poster_path;
    double poster_at;
    bool poster_done;
    AVFrame *first_frame; /* the poster if the stream ends before poster_at */

    double duration;
    double last_report;
    double video_end_s; /* end of the last frame encoded, seconds */

    AVFrame *frame, *filtered;
    AVPacket *pkt;
} conv_t;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void report(conv_t *c, double t)
{
    double now = now_s();
    if (now - c->last_report >= PROGRESS_EVERY_S) {
        c->last_report = now;
        printf("time %.2f\n", t);
        fflush(stdout);
    }
}

static int even(double v)
{
    int i = (int)lround(v / 2.0) * 2;
    return i < 16 ? 16 : i;
}

/* --- Rotation: how a phone says which way up it was held --------------------
 *
 * The same reading of the display matrix as ffmpeg's own command line
 * (fftools' get_rotation() and its autorotate filters), less arbitrary
 * angles, which nothing records. */

static const int32_t *display_matrix(const AVStream *st)
{
    const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    return sd != NULL && sd->size >= 9 * sizeof(int32_t) ? (const int32_t *)sd->data : NULL;
}

static double rotation(const int32_t *m)
{
    double theta = m != NULL ? -round(av_display_rotation_get(m)) : 0.0;
    return theta - 360.0 * floor(theta / 360.0 + 0.9 / 360.0);
}

/* Filters that turn the picture upright, with a trailing comma, or "". */
static const char *upright_filters(const int32_t *m, bool *quarter_turn)
{
    double theta = rotation(m);
    *quarter_turn = false;
    if (fabs(theta - 90.0) < 1.0) {
        *quarter_turn = true;
        return m[3] > 0 ? "transpose=cclock_flip," : "transpose=clock,";
    }
    if (fabs(theta - 270.0) < 1.0) {
        *quarter_turn = true;
        return m[3] < 0 ? "transpose=clock_flip," : "transpose=cclock,";
    }
    if (fabs(theta - 180.0) < 1.0) {
        if (m[0] < 0 && m[4] < 0) {
            return "hflip,vflip,";
        }
        return m[0] < 0 ? "hflip," : m[4] < 0 ? "vflip," : "";
    }
    return m != NULL && m[4] < 0 ? "vflip," : "";
}

/* --- Opening ------------------------------------------------------------------ */

static AVCodecContext *open_decoder(const AVStream *st, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (codec == NULL) {
        return NULL;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (dec == NULL || avcodec_parameters_to_context(dec, st->codecpar) < 0) {
        avcodec_free_context(&dec);
        return NULL;
    }
    dec->pkt_timebase = st->time_base;
    dec->thread_count = threads;
    if (avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec);
        return NULL;
    }
    return dec;
}

/* Lowers a frame rate above the screen's by halving it until it isn't --
 * 60 to 30, 50 to 25 -- so frames are dropped evenly; NULL to leave it be. */
static bool reduced_rate(AVRational in, AVRational *out)
{
    if (in.num <= 0 || in.den <= 0 || av_q2d(in) <= OUT_MAX_FPS) {
        return false;
    }
    while (av_q2d(in) > OUT_MAX_FPS) {
        in.den *= 2;
    }
    av_reduce(&out->num, &out->den, in.num, in.den, INT32_MAX);
    return true;
}

static AVFilterGraph *build_graph(const char *src_name, const char *src_args, const char *chain,
                                  const char *sink_name, AVFilterContext **src, AVFilterContext **sink)
{
    AVFilterGraph *g = avfilter_graph_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (g == NULL || outputs == NULL || inputs == NULL ||
        avfilter_graph_create_filter(src, avfilter_get_by_name(src_name), "in", src_args, NULL, g) < 0 ||
        avfilter_graph_create_filter(sink, avfilter_get_by_name(sink_name), "out", NULL, NULL, g) < 0) {
        die("couldn't set up the conversion");
    }
    g->nb_threads = 1;
    outputs->name = av_strdup("in");
    outputs->filter_ctx = *src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = *sink;
    if (avfilter_graph_parse_ptr(g, chain, &inputs, &outputs, NULL) < 0 || avfilter_graph_config(g, NULL) < 0) {
        die("couldn't set up the conversion (%s)", chain);
    }
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return g;
}

static void open_video(conv_t *c)
{
    AVStream *st = c->ifmt->streams[c->vin];
    int threads = 3;
    const char *env = getenv("RPOD_CONVERT_THREADS");
    if (env != NULL && atoi(env) > 0) {
        threads = atoi(env);
    }
    c->vdec = open_decoder(st, threads);
    if (c->vdec == NULL) {
        die("rPod can't read %s video", avcodec_get_name(st->codecpar->codec_id));
    }
    int w = c->vdec->width, h = c->vdec->height;
    if (w <= 0 || h <= 0) {
        die("that video has no picture size");
    }

    /* Where it fits on the screen, the right way up and the right shape. */
    AVRational sar = av_guess_sample_aspect_ratio(c->ifmt, st, NULL);
    if (sar.num <= 0 || sar.den <= 0) {
        sar = (AVRational){ 1, 1 };
    }
    bool quarter;
    const char *upright = upright_filters(display_matrix(st), &quarter);
    double aspect = (double)w * sar.num / ((double)h * sar.den);
    if (quarter) {
        aspect = 1.0 / aspect;
    }
    int ow, oh;
    if (aspect >= (double)OUT_MAX_W / OUT_MAX_H) {
        ow = OUT_MAX_W;
        oh = even(OUT_MAX_W / aspect);
    } else {
        oh = OUT_MAX_H;
        ow = even(OUT_MAX_H * aspect);
    }

    /* Deblocking is invisible after shrinking it this much (see the top). */
    if ((double)w * h >= 4.0 * ow * oh) {
        c->vdec->skip_loop_filter = AVDISCARD_ALL;
    }

    AVRational rate = av_guess_frame_rate(c->ifmt, st, NULL);
    AVRational lower;
    char fps[48] = "";
    if (reduced_rate(rate, &lower)) {
        snprintf(fps, sizeof(fps), "fps=%d/%d,", lower.num, lower.den);
        rate = lower;
    }

    char args[256];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", w, h,
             c->vdec->pix_fmt, st->time_base.num, st->time_base.den, sar.num, sar.den);
    if (rate.num > 0 && rate.den > 0) {
        snprintf(args + strlen(args), sizeof(args) - strlen(args), ":frame_rate=%d/%d", rate.num, rate.den);
    }
    char chain[256];
    snprintf(chain, sizeof(chain), "%sscale=%d:%d:flags=bicubic,setsar=1,%sformat=yuv420p", upright, ow, oh, fps);
    c->vgraph = build_graph("buffer", args, chain, "buffersink", &c->vsrc, &c->vsink);

    const AVCodec *enc = avcodec_find_encoder_by_name("libx264");
    bool x264 = enc != NULL;
    if (!x264) {
        enc = avcodec_find_encoder(AV_CODEC_ID_MPEG4); /* plays here, if not in a browser */
    }
    c->venc = enc != NULL ? avcodec_alloc_context3(enc) : NULL;
    if (c->venc == NULL) {
        die("no video encoder");
    }
    c->venc->width = av_buffersink_get_w(c->vsink);
    c->venc->height = av_buffersink_get_h(c->vsink);
    c->venc->pix_fmt = AV_PIX_FMT_YUV420P;
    c->venc->sample_aspect_ratio = (AVRational){ 1, 1 };
    c->venc->time_base = av_buffersink_get_time_base(c->vsink);
    AVRational sink_rate = av_buffersink_get_frame_rate(c->vsink);
    if (sink_rate.num > 0 && sink_rate.den > 0) {
        rate = sink_rate;
    }
    if (rate.num <= 0 || rate.den <= 0) {
        rate = (AVRational){ 30, 1 };
    }
    c->venc->framerate = rate;
    c->venc->gop_size = (int)lround(av_q2d(rate) * KEYFRAME_S);
    c->venc->thread_count = 2;
    if (c->ofmt->oformat->flags & AVFMT_GLOBALHEADER) {
        c->venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVDictionary *opts = NULL;
    if (x264) {
        av_dict_set(&opts, "preset", "veryfast", 0);
        av_dict_set(&opts, "profile", "main", 0);
        av_dict_set(&opts, "crf", VIDEO_CRF, 0);
        c->venc->rc_max_rate = VIDEO_MAXRATE;
        c->venc->rc_buffer_size = VIDEO_BUFSIZE;
    } else {
        c->venc->bit_rate = VIDEO_MAXRATE / 2;
    }
    if (avcodec_open2(c->venc, enc, &opts) < 0) {
        die("couldn't start the video encoder");
    }
    av_dict_free(&opts);

    c->vout = avformat_new_stream(c->ofmt, NULL);
    if (c->vout == NULL || avcodec_parameters_from_context(c->vout->codecpar, c->venc) < 0) {
        die("couldn't set up the output");
    }
    c->vout->time_base = c->venc->time_base;
    c->vout->avg_frame_rate = rate;
    c->last_vpts = AV_NOPTS_VALUE;
    fprintf(stderr, "rpod-convert: %dx%d %s -> %dx%d at %.3g fps%s\n", w, h,
            avcodec_get_name(st->codecpar->codec_id), c->venc->width, c->venc->height, av_q2d(rate),
            c->vdec->skip_loop_filter == AVDISCARD_ALL ? " (no deblocking)" : "");
}

static void open_audio(conv_t *c)
{
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    c->aenc = enc != NULL ? avcodec_alloc_context3(enc) : NULL;
    if (c->aenc == NULL) {
        die("no AAC encoder");
    }
    c->aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->aenc->sample_rate = AUDIO_RATE;
    c->aenc->bit_rate = AUDIO_BITRATE;
    c->aenc->time_base = (AVRational){ 1, AUDIO_RATE };
    av_channel_layout_default(&c->aenc->ch_layout, 2);
    if (c->ofmt->oformat->flags & AVFMT_GLOBALHEADER) {
        c->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(c->aenc, enc, NULL) < 0) {
        die("couldn't start the audio encoder");
    }
    c->aout = avformat_new_stream(c->ofmt, NULL);
    if (c->aout == NULL || avcodec_parameters_from_context(c->aout->codecpar, c->aenc) < 0) {
        die("couldn't set up the output");
    }
    c->aout->time_base = c->aenc->time_base;

    if (c->ain < 0) {
        return; /* silence, made as it's needed */
    }
    AVStream *st = c->ifmt->streams[c->ain];
    c->adec = open_decoder(st, 1);
    if (c->adec == NULL) {
        /* A picture without its sound beats nothing at all. */
        fprintf(stderr, "rpod-convert: can't decode %s audio; it'll be silent\n",
                avcodec_get_name(st->codecpar->codec_id));
        c->ain = -1;
        return;
    }
    AVChannelLayout layout;
    if (c->adec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&layout, c->adec->ch_layout.nb_channels);
    } else if (av_channel_layout_copy(&layout, &c->adec->ch_layout) < 0) {
        die("out of memory");
    }
    char desc[128];
    av_channel_layout_describe(&layout, desc, sizeof(desc));
    av_channel_layout_uninit(&layout);
    char args[256];
    snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", st->time_base.num,
             st->time_base.den, c->adec->sample_rate, av_get_sample_fmt_name(c->adec->sample_fmt), desc);
    /* Timestamps made to start at 0 and fill any gap with silence, as
     * ffmpeg's -async 1 did: the sound is what the picture follows. */
    char chain[256];
    snprintf(chain, sizeof(chain),
             "aresample=async=1:min_hard_comp=0.1:first_pts=0,"
             "aformat=sample_fmts=fltp:sample_rates=%d:channel_layouts=stereo",
             AUDIO_RATE);
    c->agraph = build_graph("abuffer", args, chain, "abuffersink", &c->asrc, &c->asink);
    av_buffersink_set_frame_size(c->asink, c->aenc->frame_size > 0 ? (unsigned)c->aenc->frame_size : AUDIO_FRAME);
}

/* --- The poster ---------------------------------------------------------------- */

static void write_poster(conv_t *c, const AVFrame *f)
{
    c->poster_done = true;
    struct SwsContext *sws = sws_getContext(f->width, f->height, f->format, f->width, f->height, AV_PIX_FMT_RGB24,
                                            SWS_BICUBIC, NULL, NULL, NULL);
    /* Rows padded and aligned: swscale's SIMD writes whole blocks, past the
     * end of a row that isn't a multiple of them (a 136-wide portrait
     * poster corrupted the heap). */
    int stride[4] = { FFALIGN(f->width * 3, 64), 0, 0, 0 };
    uint8_t *rgb = av_malloc((size_t)stride[0] * (size_t)(f->height + 1));
    FILE *out = fopen(c->poster_path, "wb");
    if (sws == NULL || rgb == NULL || out == NULL) {
        fprintf(stderr, "rpod-convert: couldn't write the poster %s\n", c->poster_path);
        goto done;
    }
    uint8_t *dst[4] = { rgb, NULL, NULL, NULL };
    sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride);

    struct jpeg_compress_struct ci;
    struct jpeg_error_mgr err;
    ci.err = jpeg_std_error(&err);
    jpeg_create_compress(&ci);
    jpeg_stdio_dest(&ci, out);
    ci.image_width = (JDIMENSION)f->width;
    ci.image_height = (JDIMENSION)f->height;
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, POSTER_QUALITY, TRUE);
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = rgb + (size_t)ci.next_scanline * (size_t)stride[0];
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
done:
    if (out != NULL && fclose(out) != 0) {
        fprintf(stderr, "rpod-convert: couldn't write the poster %s\n", c->poster_path);
    }
    av_free(rgb);
    sws_freeContext(sws);
}

/* --- The loop ------------------------------------------------------------------ */

static void write_packets(conv_t *c, AVCodecContext *enc, AVStream *ost)
{
    for (;;) {
        int r = avcodec_receive_packet(enc, c->pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            return;
        }
        if (r < 0) {
            die("the encoder failed");
        }
        av_packet_rescale_ts(c->pkt, enc->time_base, ost->time_base);
        c->pkt->stream_index = ost->index;
        if (av_interleaved_write_frame(c->ofmt, c->pkt) < 0) {
            die("couldn't write the video (is the card full?)");
        }
    }
}

static void encode(conv_t *c, AVCodecContext *enc, AVStream *ost, AVFrame *f)
{
    if (avcodec_send_frame(enc, f) < 0) {
        die("the encoder failed");
    }
    write_packets(c, enc, ost);
}

/* Silence up to `until_s`, for a video with no sound of its own. */
static void silence_until(conv_t *c, double until_s)
{
    int n = c->aenc->frame_size > 0 ? c->aenc->frame_size : AUDIO_FRAME;
    while ((double)c->silence_pts / AUDIO_RATE < until_s) {
        AVFrame *f = av_frame_alloc();
        if (f == NULL) {
            die("out of memory");
        }
        f->format = c->aenc->sample_fmt;
        f->sample_rate = AUDIO_RATE;
        f->nb_samples = n;
        av_channel_layout_copy(&f->ch_layout, &c->aenc->ch_layout);
        if (av_frame_get_buffer(f, 0) < 0) {
            die("out of memory");
        }
        av_samples_set_silence(f->data, 0, n, f->ch_layout.nb_channels, f->format);
        f->pts = c->silence_pts;
        c->silence_pts += n;
        encode(c, c->aenc, c->aout, f);
        av_frame_free(&f);
    }
}

static void pull_video(conv_t *c)
{
    AVRational tb = av_buffersink_get_time_base(c->vsink);
    for (;;) {
        int r = av_buffersink_get_frame(c->vsink, c->filtered);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            return;
        }
        if (r < 0) {
            die("the conversion failed");
        }
        AVFrame *f = c->filtered;
        /* Frames that land on the same tick (a variable-rate phone video,
         * timestamps rounded) would make the muxer refuse the second. */
        if (c->last_vpts != AV_NOPTS_VALUE && f->pts <= c->last_vpts) {
            av_frame_unref(f);
            continue;
        }
        c->last_vpts = f->pts;
        double t = (double)f->pts * av_q2d(tb);
        if (c->poster_path != NULL && !c->poster_done) {
            if (t >= c->poster_at) {
                write_poster(c, f);
            } else if (c->first_frame == NULL) {
                c->first_frame = av_frame_clone(f);
            }
        }
        f->pict_type = AV_PICTURE_TYPE_NONE;
        encode(c, c->venc, c->vout, f);
        double frame_s = av_q2d(av_inv_q(c->venc->framerate));
        c->video_end_s = t + (frame_s > 0 && frame_s < 1 ? frame_s : 1.0 / 30);
        if (c->ain < 0) {
            silence_until(c, c->video_end_s);
        }
        report(c, t);
        av_frame_unref(f);
    }
}

static void pull_audio(conv_t *c)
{
    for (;;) {
        int r = av_buffersink_get_frame(c->asink, c->filtered);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            return;
        }
        if (r < 0) {
            die("the conversion failed");
        }
        encode(c, c->aenc, c->aout, c->filtered);
        av_frame_unref(c->filtered);
    }
}

/* Decodes what `dec` has ready into its filter graph -- `pkt` NULL drains
 * it at the end. */
static void decode(conv_t *c, AVCodecContext *dec, const AVStream *st, AVFilterContext *src, const AVPacket *pkt,
                   bool video)
{
    if (avcodec_send_packet(dec, pkt) < 0 && pkt != NULL) {
        return; /* a damaged packet: skip it, as players do */
    }
    int64_t offset = c->start != AV_NOPTS_VALUE ? av_rescale_q(c->start, AV_TIME_BASE_Q, st->time_base) : 0;
    for (;;) {
        int r = avcodec_receive_frame(dec, c->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            break;
        }
        if (r < 0) {
            break; /* likewise */
        }
        int64_t pts = c->frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) {
            av_frame_unref(c->frame);
            continue;
        }
        c->frame->pts = pts - offset;
        if (video && c->frame->pts < 0) {
            av_frame_unref(c->frame); /* before the start: pre-roll */
            continue;
        }
        if (av_buffersrc_add_frame_flags(src, c->frame, 0) < 0) {
            die("the conversion failed");
        }
        if (video) {
            pull_video(c);
        } else {
            pull_audio(c);
        }
    }
    if (pkt == NULL) {
        if (av_buffersrc_add_frame(src, NULL) < 0) {
            die("the conversion failed");
        }
        if (video) {
            pull_video(c);
        } else {
            pull_audio(c);
        }
    }
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

int main(int argc, char **argv)
{
    conv_t conv = { .vin = -1, .ain = -1 };
    conv_t *c = &conv;
    const char *title = NULL;
    int i = 1;
    for (; i + 1 < argc && argv[i][0] == '-' && argv[i][1] == '-'; i += 2) {
        if (strcmp(argv[i], "--title") == 0) {
            title = argv[i + 1];
        } else if (strcmp(argv[i], "--poster") == 0) {
            c->poster_path = argv[i + 1];
        } else {
            break;
        }
    }
    if (argc - i != 2) {
        fprintf(stderr, "usage: rpod-convert [--title T] [--poster P.jpg] INPUT OUTPUT.mp4\n");
        return 2;
    }
    const char *in_path = argv[i], *out_path = argv[i + 1];
    av_log_set_level(AV_LOG_ERROR);

    if (avformat_open_input(&c->ifmt, in_path, NULL, NULL) < 0) {
        die("rPod can't read that file");
    }
    if (avformat_find_stream_info(c->ifmt, NULL) < 0) {
        die("that file seems damaged");
    }
    c->vin = av_find_best_stream(c->ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (c->vin < 0 || (c->ifmt->streams[c->vin]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        die("there's no video in that file");
    }
    int ain = av_find_best_stream(c->ifmt, AVMEDIA_TYPE_AUDIO, -1, c->vin, NULL, 0);
    c->ain = ain >= 0 ? ain : -1;
    c->start = c->ifmt->start_time;
    c->duration = c->ifmt->duration != AV_NOPTS_VALUE ? (double)c->ifmt->duration / AV_TIME_BASE : 0.0;
    c->poster_at = fmin(c->duration * POSTER_FRACTION, POSTER_MAX_S);
    printf("duration %.2f\n", c->duration);
    fflush(stdout);

    if (avformat_alloc_output_context2(&c->ofmt, NULL, "mp4", out_path) < 0) {
        die("couldn't set up the output");
    }
    open_video(c);
    open_audio(c);

    /* The title it shows under: what it says it is, or what it's called. */
    const AVDictionaryEntry *tag = av_dict_get(c->ifmt->metadata, "title", NULL, 0);
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", base_name(in_path));
    char *dot = strrchr(stem, '.');
    if (dot != NULL && dot != stem) {
        *dot = '\0';
    }
    av_dict_set(&c->ofmt->metadata, "title", title != NULL ? title : tag != NULL ? tag->value : stem, 0);
    av_dict_set(&c->ofmt->metadata, "encoder", "rpod-convert", 0);

    if (avio_open(&c->ofmt->pb, out_path, AVIO_FLAG_WRITE) < 0) {
        die("couldn't write the video");
    }
    AVDictionary *mux = NULL;
    av_dict_set(&mux, "movflags", "+faststart", 0); /* the index first: opens at once */
    if (avformat_write_header(c->ofmt, &mux) < 0) {
        die("couldn't write the video");
    }
    av_dict_free(&mux);

    c->frame = av_frame_alloc();
    c->filtered = av_frame_alloc();
    c->pkt = av_packet_alloc();
    AVPacket *in = av_packet_alloc();
    if (c->frame == NULL || c->filtered == NULL || c->pkt == NULL || in == NULL) {
        die("out of memory");
    }
    c->last_report = now_s();
    while (av_read_frame(c->ifmt, in) >= 0) {
        if (in->stream_index == c->vin) {
            decode(c, c->vdec, c->ifmt->streams[c->vin], c->vsrc, in, true);
        } else if (in->stream_index == c->ain) {
            decode(c, c->adec, c->ifmt->streams[c->ain], c->asrc, in, false);
        }
        av_packet_unref(in);
    }
    decode(c, c->vdec, c->ifmt->streams[c->vin], c->vsrc, NULL, true);
    if (c->ain >= 0) {
        decode(c, c->adec, c->ifmt->streams[c->ain], c->asrc, NULL, false);
    } else {
        silence_until(c, c->video_end_s);
    }
    if (c->last_vpts == AV_NOPTS_VALUE) {
        die("there's no picture in that video");
    }
    if (c->poster_path != NULL && !c->poster_done && c->first_frame != NULL) {
        write_poster(c, c->first_frame);
    }
    encode(c, c->venc, c->vout, NULL);
    encode(c, c->aenc, c->aout, NULL);
    if (av_write_trailer(c->ofmt) < 0 || avio_closep(&c->ofmt->pb) < 0) {
        die("couldn't finish the video (is the card full?)");
    }

    av_packet_free(&in);
    av_packet_free(&c->pkt);
    av_frame_free(&c->frame);
    av_frame_free(&c->filtered);
    av_frame_free(&c->first_frame);
    avfilter_graph_free(&c->vgraph);
    avfilter_graph_free(&c->agraph);
    avcodec_free_context(&c->vdec);
    avcodec_free_context(&c->adec);
    avcodec_free_context(&c->venc);
    avcodec_free_context(&c->aenc);
    avformat_free_context(c->ofmt);
    avformat_close_input(&c->ifmt);
    printf("done\n");
    fflush(stdout);
    return 0;
}
