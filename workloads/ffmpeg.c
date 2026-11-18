/* FFmpeg 8.1.3 (libavcodec) video encode / decode over generated video.
 *
 * The input is generated, not read: a panning textured background with moving
 * textured objects and a little per-frame noise, so that motion search, intra
 * prediction and the entropy coder all see realistic work. Integer only.
 *
 * Knobs (-D):
 *   ENCODER  libx264    libx264 | mpeg4 | mpeg2video | mjpeg | prores_ks | ffv1
 *   MODE     0          0 decode: encoded once in setup, decoded ITERS times
 *                         (checksum over the decoded frames)
 *                       1 encode: encoded ITERS times
 *                         (checksum over the bitstream)
 *   ITERS    16 / 2     measured iterations (MODE 0 / MODE 1)
 *   WIDTH    640        frame width, even
 *   HEIGHT   360        frame height, even
 *   FRAMES   48         frames per iteration
 *   GOP      48         keyframe interval
 *   PRESET   veryfast / medium       libx264 preset (MODE 0 / MODE 1)
 *   CRF      23         libx264 constant rate factor
 *   QSCALE   4          fixed quantizer for mpeg4, mpeg2video and mjpeg
 *   SEED     0x9E3779B9 video RNG seed
 */
#define BENCH_NAME "ffmpeg"
#include "bench.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

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
#define ITERS (MODE == 0 ? 16 : 2)
#endif
#ifndef WIDTH
#define WIDTH 640
#endif
#ifndef HEIGHT
#define HEIGHT 360
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

#define CW (WIDTH / 2)
#define CH (HEIGHT / 2)
#define FRAME_BYTES (WIDTH * HEIGHT + 2 * CW * CH)
#define TW (WIDTH * 2)
#define TH (HEIGHT * 2)
#define NOBJ 6

static void make_texture(uint8_t *t) {
    for (int i = 0; i < TW * TH; i++) t[i] = (uint8_t)(crnd() >> 24);
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < TH; y++)
            for (int x = 1; x < TW - 1; x++)
                t[y * TW + x] = (uint8_t)((t[y * TW + x - 1] + 2 * t[y * TW + x] + t[y * TW + x + 1]) >> 2);
        for (int y = 1; y < TH - 1; y++)
            for (int x = 0; x < TW; x++)
                t[y * TW + x] = (uint8_t)((t[(y - 1) * TW + x] + 2 * t[y * TW + x] + t[(y + 1) * TW + x]) >> 2);
    }
}

/* FRAMES frames of 8-bit 4:2:0, planes packed back to back. */
static void make_video(uint8_t *out, const uint8_t *tex) {
    int ox[NOBJ], oy[NOBJ], vx[NOBJ], vy[NOBJ], sz[NOBJ], lum[NOBJ], cu[NOBJ], cv[NOBJ];
    for (int k = 0; k < NOBJ; k++) {
        ox[k] = (int)(crnd() % WIDTH); oy[k] = (int)(crnd() % HEIGHT);
        vx[k] = (int)(crnd() % 9) - 4;  vy[k] = (int)(crnd() % 7) - 3;
        sz[k] = HEIGHT / 10 + (int)(crnd() % (HEIGHT / 5));
        lum[k] = (int)(crnd() % 160) - 80;
        cu[k] = 64 + (int)(crnd() % 128); cv[k] = 64 + (int)(crnd() % 128);
    }
    for (int f = 0; f < FRAMES; f++) {
        uint8_t *Y = out + (size_t)f * FRAME_BYTES, *U = Y + WIDTH * HEIGHT, *V = U + CW * CH;
        int px = (f * 3) % (TW - WIDTH), py = f % (TH - HEIGHT);
        for (int y = 0; y < HEIGHT; y++)
            for (int x = 0; x < WIDTH; x++)
                Y[y * WIDTH + x] = (uint8_t)(16 + ((tex[(y + py) * TW + x + px] * 3 / 4 + y * 64 / HEIGHT) & 0xff) * 219 / 255);
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++) {
                U[y * CW + x] = (uint8_t)(96 + x * 64 / CW);
                V[y * CW + x] = (uint8_t)(160 - y * 64 / CH);
            }
        for (int k = 0; k < NOBJ; k++) {
            int cx = ox[k] + vx[k] * f, cy = oy[k] + vy[k] * f, r = sz[k] / 2;
            for (int y = cy - r; y < cy + r; y++) {
                if (y < 0 || y >= HEIGHT) continue;
                for (int x = cx - r; x < cx + r; x++) {
                    if (x < 0 || x >= WIDTH) continue;
                    int dx = x - cx, dy = y - cy;
                    if (dx * dx + dy * dy > r * r) continue;
                    int v = tex[((dy + r) * 2 % TH) * TW + (dx + r) * 2 % TW] / 2 + 64 + lum[k];
                    Y[y * WIDTH + x] = (uint8_t)(v < 16 ? 16 : v > 235 ? 235 : v);
                    U[(y / 2) * CW + x / 2] = (uint8_t)cu[k];
                    V[(y / 2) * CW + x / 2] = (uint8_t)cv[k];
                }
            }
        }
        for (int i = 0; i < WIDTH * HEIGHT / 16; i++) {
            uint32_t r = crnd();
            int p = (int)(r % (WIDTH * HEIGHT)), d = (int)((r >> 24) & 7) - 3;
            int v = Y[p] + d;
            Y[p] = (uint8_t)(v < 16 ? 16 : v > 235 ? 235 : v);
        }
    }
}

/* Copies one generated frame into the encoder's pixel format. */
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
    const AVCodec *codec = avcodec_find_decoder(s->codec);
    if (!codec) bench_fail("no decoder for %s", ENCODER_NAME);
    AVCodecContext *d = avcodec_alloc_context3(codec);
    d->thread_count = 1;
    d->flags |= AV_CODEC_FLAG_BITEXACT;
    d->width = WIDTH; d->height = HEIGHT;
    if (s->extradata_size) {
        d->extradata = av_mallocz((size_t)s->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(d->extradata, s->extradata, (size_t)s->extradata_size);
        d->extradata_size = s->extradata_size;
    }
    if (avcodec_open2(d, codec, NULL) < 0) bench_fail("cannot open decoder %s", codec->name);
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
    corpus_seed(SEED);
    uint8_t *tex = bench_alloc((size_t)TW * TH);
    uint8_t *video = bench_alloc((size_t)FRAMES * FRAME_BYTES);
    make_texture(tex);
    make_video(video, tex);
    free(tex);

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
