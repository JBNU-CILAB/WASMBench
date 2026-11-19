/* FFmpeg 8.1.3 (libavcodec) video encode / decode.
 *
 * The input is two seconds of 160x90 random noise (workloads/media), stored in
 * the module as an H.264 stream and decoded once in setup. Every frame is new
 * noise, so prediction from neighbouring pixels or from the previous frame
 * finds little and the encoders code mostly residual.
 *
 * Knobs (-D):
 *   ENCODER  libx264    libx264 | mpeg4 | mpeg2video | mjpeg | prores_ks | ffv1
 *   MODE     0          0 decode: encoded once in setup, decoded ITERS times
 *                         (checksum over the decoded frames)
 *                       1 encode: encoded ITERS times
 *                         (checksum over the bitstream)
 *   ITERS    64 / 8     measured iterations (MODE 0 / MODE 1)
 *   FRAMES   48         frames taken from the source video, at most 48
 *   GOP      48         keyframe interval
 *   PRESET   veryfast / medium       libx264 preset (MODE 0 / MODE 1)
 *   CRF      23         libx264 constant rate factor
 *   QSCALE   4          fixed quantizer for mpeg4, mpeg2video and mjpeg
 * SEED is not used.
 */
#define BENCH_NAME "ffmpeg"
#include "bench.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include "ffmpeg_video.h"

#define STR_(x) #x
#define STR(x) STR_(x)

#ifdef ENCODER
#define ENCODER_NAME STR(ENCODER)
#else
#define ENCODER_NAME "libx264"
#endif
#ifndef MODE
#define MODE 0
#endif
#ifndef ITERS
#define ITERS (MODE == 0 ? 64 : 8)
#endif
#ifndef FRAMES
#define FRAMES 48
#endif
#ifndef GOP
#define GOP 48
#endif
#ifdef PRESET
#define PRESET_NAME STR(PRESET)
#else
#define PRESET_NAME (MODE == 0 ? "veryfast" : "medium")
#endif
#ifdef CRF
#define CRF_VALUE STR(CRF)
#else
#define CRF_VALUE "23"
#endif
#ifndef QSCALE
#define QSCALE 4
#endif

static int WIDTH, HEIGHT, CW, CH;
#define FRAME_BYTES ((size_t)WIDTH * HEIGHT + 2 * (size_t)CW * CH)

static AVCodecContext *open_decoder(enum AVCodecID id) {
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) bench_fail("no decoder for codec %d", (int)id);
    AVCodecContext *d = avcodec_alloc_context3(codec);
    d->thread_count = 1;
    d->flags |= AV_CODEC_FLAG_BITEXACT;
    return d;
}

/* Decodes the embedded H.264 source into FRAMES packed 8-bit 4:2:0 frames. */
static uint8_t *load_video(void) {
    AVCodecContext *d = open_decoder(AV_CODEC_ID_H264);
    if (avcodec_open2(d, d->codec, NULL) < 0) bench_fail("cannot open h264 decoder");
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) bench_fail("no h264 parser");
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    uint8_t *in = bench_alloc(sizeof ffmpeg_video + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(in, ffmpeg_video, sizeof ffmpeg_video);
    memset(in + sizeof ffmpeg_video, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    uint8_t *video = NULL;
    int n = 0;
    size_t off = 0;
    for (;;) {
        int eof = off == sizeof ffmpeg_video;
        int used = av_parser_parse2(parser, d, &pkt->data, &pkt->size, in + off,
                                    (int)(sizeof ffmpeg_video - off), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        off += (size_t)used;
        int r = pkt->size ? avcodec_send_packet(d, pkt) : eof ? avcodec_send_packet(d, NULL) : 0;
        if (r < 0) bench_fail("source send_packet %d", r);
        while ((r = avcodec_receive_frame(d, fr)) == 0) {
            if (fr->format != AV_PIX_FMT_YUV420P) bench_fail("source is not yuv420p");
            if (!video) {
                WIDTH = fr->width; HEIGHT = fr->height; CW = WIDTH / 2; CH = HEIGHT / 2;
                video = bench_alloc(FRAMES * FRAME_BYTES);
            }
            if (n < FRAMES) {
                uint8_t *Y = video + (size_t)n * FRAME_BYTES, *U = Y + WIDTH * HEIGHT, *V = U + CW * CH;
                for (int y = 0; y < HEIGHT; y++) memcpy(Y + y * WIDTH, fr->data[0] + y * fr->linesize[0], WIDTH);
                for (int y = 0; y < CH; y++) {
                    memcpy(U + y * CW, fr->data[1] + y * fr->linesize[1], CW);
                    memcpy(V + y * CW, fr->data[2] + y * fr->linesize[2], CW);
                }
            }
            n++;
            av_frame_unref(fr);
        }
        if (r == AVERROR_EOF) break;
        if (r != AVERROR(EAGAIN)) bench_fail("source receive_frame %d", r);
    }
    if (n < FRAMES) bench_fail("source has %d frames, FRAMES is %d", n, FRAMES);
    free(in);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    av_parser_close(parser);
    avcodec_free_context(&d);
    return video;
}

/* Copies one source frame into the encoder's pixel format. */
static void fill_frame(AVFrame *fr, const uint8_t *src) {
    const uint8_t *pl[3] = { src, src + WIDTH * HEIGHT, src + WIDTH * HEIGHT + CW * CH };
    if (fr->format == AV_PIX_FMT_YUV422P10) {
        for (int p = 0; p < 3; p++) {
            int w = p ? CW : WIDTH, sw = w;
            for (int y = 0; y < HEIGHT; y++) {
                uint16_t *d = (uint16_t *)(fr->data[p] + y * fr->linesize[p]);
                const uint8_t *s = pl[p] + (p ? y / 2 : y) * sw;
                for (int x = 0; x < w; x++) d[x] = (uint16_t)(s[x] << 2);
            }
        }
        return;
    }
    for (int y = 0; y < HEIGHT; y++) memcpy(fr->data[0] + y * fr->linesize[0], pl[0] + y * WIDTH, WIDTH);
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < CH; y++) memcpy(fr->data[p] + y * fr->linesize[p], pl[p] + y * CW, CW);
}

typedef struct {
    enum AVCodecID codec;
    uint8_t *extradata;
    int extradata_size;
    uint8_t *data;
    size_t len, cap;
    int *sizes, count, size_cap;
} Stream;

static void stream_add(Stream *s, const AVPacket *pkt) {
    if (s->len + (size_t)pkt->size > s->cap) {
        s->cap = (s->len + (size_t)pkt->size) * 2;
        if (!(s->data = realloc(s->data, s->cap))) bench_fail("out of memory");
    }
    if (s->count == s->size_cap) {
        s->size_cap = s->size_cap ? s->size_cap * 2 : 64;
        if (!(s->sizes = realloc(s->sizes, sizeof(int) * (size_t)s->size_cap))) bench_fail("out of memory");
    }
    memcpy(s->data + s->len, pkt->data, (size_t)pkt->size);
    s->len += (size_t)pkt->size;
    s->sizes[s->count++] = pkt->size;
}

static void drain_encoder(AVCodecContext *e, AVPacket *pkt, Stream *s) {
    int r;
    while ((r = avcodec_receive_packet(e, pkt)) == 0) {
        stream_add(s, pkt);
        av_packet_unref(pkt);
    }
    if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) bench_fail("receive_packet %d", r);
}

static void encode(const uint8_t *video, Stream *s) {
    const AVCodec *codec = avcodec_find_encoder_by_name(ENCODER_NAME);
    if (!codec) bench_fail("encoder %s is not built in", ENCODER_NAME);
    AVCodecContext *e = avcodec_alloc_context3(codec);
    e->width = WIDTH; e->height = HEIGHT;
    e->pix_fmt = AV_PIX_FMT_YUV420P;
    e->time_base = (AVRational){1, 24};
    e->framerate = (AVRational){24, 1};
    e->gop_size = GOP;
    e->thread_count = 1;
    e->flags |= AV_CODEC_FLAG_BITEXACT;
    switch (codec->id) {
    case AV_CODEC_ID_H264:
        av_opt_set(e->priv_data, "preset", PRESET_NAME, 0);
        av_opt_set(e->priv_data, "crf", CRF_VALUE, 0);
        break;
    case AV_CODEC_ID_MJPEG:
        e->color_range = AVCOL_RANGE_JPEG;
        /* fall through */
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MPEG2VIDEO:
        e->flags |= AV_CODEC_FLAG_QSCALE;
        e->global_quality = FF_QP2LAMBDA * QSCALE;
        break;
    case AV_CODEC_ID_PRORES:
        e->pix_fmt = AV_PIX_FMT_YUV422P10;
        break;
    default:
        break;
    }
    if (avcodec_open2(e, codec, NULL) < 0) bench_fail("cannot open encoder %s", ENCODER_NAME);

    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    frame->format = e->pix_fmt; frame->width = WIDTH; frame->height = HEIGHT;
    frame->color_range = e->color_range;
    if (av_frame_get_buffer(frame, 0) < 0) bench_fail("frame buffer");
    s->codec = codec->id;
    s->len = 0; s->count = 0;
    for (int f = 0; f < FRAMES; f++) {
        if (av_frame_make_writable(frame) < 0) bench_fail("make_writable");
        fill_frame(frame, video + (size_t)f * FRAME_BYTES);
        frame->pts = f;
        if (avcodec_send_frame(e, frame) < 0) bench_fail("send_frame");
        drain_encoder(e, pkt, s);
    }
    avcodec_send_frame(e, NULL);
    drain_encoder(e, pkt, s);
    av_freep(&s->extradata);
    s->extradata_size = e->extradata_size;
    if (e->extradata_size) {
        s->extradata = av_mallocz((size_t)e->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(s->extradata, e->extradata, (size_t)e->extradata_size);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&e);
}

static uint32_t hash_frame(uint32_t h, const AVFrame *fr) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fr->format);
    for (int p = 0; p < av_pix_fmt_count_planes(fr->format); p++) {
        int rows = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(fr->height, desc->log2_chroma_h) : fr->height;
        int bytes = av_image_get_linesize(fr->format, fr->width, p);
        for (int y = 0; y < rows; y++) h = fold32(h, fr->data[p] + y * fr->linesize[p], (size_t)bytes);
    }
    return h;
}

static uint32_t decode(const Stream *s, uint32_t h, int *nframes) {
    AVCodecContext *d = open_decoder(s->codec);
    d->width = WIDTH; d->height = HEIGHT;
    if (s->extradata_size) {
        d->extradata = av_mallocz((size_t)s->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(d->extradata, s->extradata, (size_t)s->extradata_size);
        d->extradata_size = s->extradata_size;
    }
    if (avcodec_open2(d, d->codec, NULL) < 0) bench_fail("cannot open decoder %s", d->codec->name);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    size_t off = 0;
    *nframes = 0;
    for (int i = 0; i <= s->count; i++) {
        int r;
        if (i < s->count) {
            pkt->data = s->data + off; pkt->size = s->sizes[i];
            off += (size_t)s->sizes[i];
            r = avcodec_send_packet(d, pkt);
        } else {
            r = avcodec_send_packet(d, NULL);
        }
        if (r < 0) bench_fail("send_packet %d", r);
        while ((r = avcodec_receive_frame(d, fr)) == 0) {
            h = hash_frame(h, fr);
            (*nframes)++;
            av_frame_unref(fr);
        }
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) bench_fail("receive_frame %d", r);
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&d);
    return h;
}

BENCH_EXPORT uint32_t runtime(void) {
    av_log_set_level(AV_LOG_ERROR);
    uint8_t *video = load_video();

    Stream s = {0};
    uint32_t h = 2166136261u;
    if (MODE == 0) encode(video, &s);
    for (long it = 0; it < ITERS; it++) {
        if (MODE == 0) {
            int n;
            h = decode(&s, h, &n);
            if (n != FRAMES) bench_fail("decoded %d of %d frames", n, FRAMES);
        } else {
            encode(video, &s);
            h = fold32(h, s.data, s.len);
        }
    }
    free(video); free(s.data); free(s.sizes); av_freep(&s.extradata);
    return h;
}

BENCH_MAIN
