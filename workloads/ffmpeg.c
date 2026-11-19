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


/* Frame geometry, taken from the source video. Frames are stored as 8-bit
 * 4:2:0 with the Y, U and V planes packed back to back. */
static int luma_width;
static int luma_height;
static int chroma_width;
static int chroma_height;

static size_t luma_bytes(void) {
    return (size_t)luma_width * luma_height;
}

static size_t chroma_bytes(void) {
    return (size_t)chroma_width * chroma_height;
}

static size_t frame_bytes(void) {
    return luma_bytes() + 2 * chroma_bytes();
}


/* Copies `rows` rows of `width` bytes between buffers with different strides. */
static void copy_plane(uint8_t *dst, int dst_stride,
                       const uint8_t *src, int src_stride,
                       int width, int rows) {
    for (int y = 0; y < rows; y++) {
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, (size_t)width);
    }
}

static AVCodecContext *new_decoder(enum AVCodecID id) {
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        bench_fail("no decoder for codec %d", (int)id);
    }

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    dec->thread_count = 1;
    dec->flags |= AV_CODEC_FLAG_BITEXACT;
    return dec;
}


/* ---- source video ------------------------------------------------------ */

/* Appends one decoded source frame to `video` in the packed layout. */
static void store_source_frame(uint8_t *video, int index, const AVFrame *frame) {
    uint8_t *y_plane = video + (size_t)index * frame_bytes();
    uint8_t *u_plane = y_plane + luma_bytes();
    uint8_t *v_plane = u_plane + chroma_bytes();

    copy_plane(y_plane, luma_width, frame->data[0], frame->linesize[0], luma_width, luma_height);
    copy_plane(u_plane, chroma_width, frame->data[1], frame->linesize[1], chroma_width, chroma_height);
    copy_plane(v_plane, chroma_width, frame->data[2], frame->linesize[2], chroma_width, chroma_height);
}

/* Decodes the embedded H.264 source into FRAMES packed frames. */
static uint8_t *load_video(void) {
    AVCodecContext *dec = new_decoder(AV_CODEC_ID_H264);
    if (avcodec_open2(dec, dec->codec, NULL) < 0) {
        bench_fail("cannot open h264 decoder");
    }

    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        bench_fail("no h264 parser");
    }

    /* The parser may read past the end, so it needs zeroed padding. */
    const size_t size = sizeof ffmpeg_video;
    uint8_t *input = bench_alloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(input, ffmpeg_video, size);
    memset(input + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *video = NULL;
    int decoded = 0;
    size_t offset = 0;

    for (;;) {
        int at_end = (offset == size);

        int used = av_parser_parse2(parser, dec, &packet->data, &packet->size,
                                    input + offset, (int)(size - offset),
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        offset += (size_t)used;

        int ret = 0;
        if (packet->size > 0) {
            ret = avcodec_send_packet(dec, packet);
        } else if (at_end) {
            ret = avcodec_send_packet(dec, NULL);
        }
        if (ret < 0) {
            bench_fail("source send_packet %d", ret);
        }

        while ((ret = avcodec_receive_frame(dec, frame)) == 0) {
            if (frame->format != AV_PIX_FMT_YUV420P) {
                bench_fail("source is not yuv420p");
            }

            if (!video) {
                luma_width = frame->width;
                luma_height = frame->height;
                chroma_width = luma_width / 2;
                chroma_height = luma_height / 2;
                video = bench_alloc(FRAMES * frame_bytes());
            }

            if (decoded < FRAMES) {
                store_source_frame(video, decoded, frame);
            }
            decoded++;
            av_frame_unref(frame);
        }

        if (ret == AVERROR_EOF) {
            break;
        }
        if (ret != AVERROR(EAGAIN)) {
            bench_fail("source receive_frame %d", ret);
        }
    }

    if (decoded < FRAMES) {
        bench_fail("source has %d frames, FRAMES is %d", decoded, FRAMES);
    }

    free(input);
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_parser_close(parser);
    avcodec_free_context(&dec);
    return video;
}


/* ---- encoded stream ---------------------------------------------------- */

/* An encoded video kept in memory: all packets back to back, plus each
 * packet's size so the decoder can be fed one packet at a time. */
typedef struct {
    enum AVCodecID codec;

    uint8_t *extradata;
    int extradata_size;

    uint8_t *data;
    size_t length;
    size_t capacity;

    int *packet_sizes;
    int packet_count;
    int packet_capacity;
} Stream;

static void stream_add(Stream *s, const AVPacket *packet) {
    size_t needed = s->length + (size_t)packet->size;
    if (needed > s->capacity) {
        s->capacity = needed * 2;
        s->data = realloc(s->data, s->capacity);
        if (!s->data) {
            bench_fail("out of memory");
        }
    }

    if (s->packet_count == s->packet_capacity) {
        s->packet_capacity = s->packet_capacity ? s->packet_capacity * 2 : 64;
        s->packet_sizes = realloc(s->packet_sizes, sizeof(int) * (size_t)s->packet_capacity);
        if (!s->packet_sizes) {
            bench_fail("out of memory");
        }
    }

    memcpy(s->data + s->length, packet->data, (size_t)packet->size);
    s->length += (size_t)packet->size;
    s->packet_sizes[s->packet_count] = packet->size;
    s->packet_count++;
}

static void stream_free(Stream *s) {
    free(s->data);
    free(s->packet_sizes);
    av_freep(&s->extradata);
}


/* ---- encode ------------------------------------------------------------ */

/* Copies one packed source frame into the encoder's input frame. prores_ks
 * takes 10-bit 4:2:2, so for it each chroma row is used twice and every
 * sample is shifted up by two bits. */
static void fill_frame(AVFrame *frame, const uint8_t *src) {
    const uint8_t *planes[3] = {
        src,
        src + luma_bytes(),
        src + luma_bytes() + chroma_bytes(),
    };

    if (frame->format == AV_PIX_FMT_YUV422P10) {
        for (int p = 0; p < 3; p++) {
            int width = (p == 0) ? luma_width : chroma_width;

            for (int y = 0; y < luma_height; y++) {
                int src_row = (p == 0) ? y : y / 2;
                const uint8_t *in = planes[p] + (size_t)src_row * width;
                uint16_t *out = (uint16_t *)(frame->data[p] + (size_t)y * frame->linesize[p]);

                for (int x = 0; x < width; x++) {
                    out[x] = (uint16_t)(in[x] << 2);
                }
            }
        }
        return;
    }

    copy_plane(frame->data[0], frame->linesize[0], planes[0], luma_width, luma_width, luma_height);
    copy_plane(frame->data[1], frame->linesize[1], planes[1], chroma_width, chroma_width, chroma_height);
    copy_plane(frame->data[2], frame->linesize[2], planes[2], chroma_width, chroma_width, chroma_height);
}

static AVCodecContext *new_encoder(void) {
    const AVCodec *codec = avcodec_find_encoder_by_name(ENCODER_NAME);
    if (!codec) {
        bench_fail("encoder %s is not built in", ENCODER_NAME);
    }

    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width = luma_width;
    enc->height = luma_height;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = (AVRational){1, 24};
    enc->framerate = (AVRational){24, 1};
    enc->gop_size = GOP;
    enc->thread_count = 1;
    enc->flags |= AV_CODEC_FLAG_BITEXACT;

    switch (codec->id) {
    case AV_CODEC_ID_H264:
        av_opt_set(enc->priv_data, "preset", PRESET_NAME, 0);
        av_opt_set(enc->priv_data, "crf", CRF_VALUE, 0);
        break;

    case AV_CODEC_ID_MJPEG:
        enc->color_range = AVCOL_RANGE_JPEG;
        /* fall through */
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MPEG2VIDEO:
        enc->flags |= AV_CODEC_FLAG_QSCALE;
        enc->global_quality = FF_QP2LAMBDA * QSCALE;
        break;

    case AV_CODEC_ID_PRORES:
        enc->pix_fmt = AV_PIX_FMT_YUV422P10;
        break;

    default:
        break;
    }

    if (avcodec_open2(enc, codec, NULL) < 0) {
        bench_fail("cannot open encoder %s", ENCODER_NAME);
    }
    return enc;
}

/* Moves every packet the encoder has ready into the stream. */
static void drain_encoder(AVCodecContext *enc, AVPacket *packet, Stream *s) {
    int ret;
    while ((ret = avcodec_receive_packet(enc, packet)) == 0) {
        stream_add(s, packet);
        av_packet_unref(packet);
    }

    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        bench_fail("receive_packet %d", ret);
    }
}

/* Encodes FRAMES source frames into `s`, replacing what it held. */
static void encode(const uint8_t *video, Stream *s) {
    AVCodecContext *enc = new_encoder();

    AVFrame *frame = av_frame_alloc();
    frame->format = enc->pix_fmt;
    frame->width = luma_width;
    frame->height = luma_height;
    frame->color_range = enc->color_range;
    if (av_frame_get_buffer(frame, 0) < 0) {
        bench_fail("frame buffer");
    }

    AVPacket *packet = av_packet_alloc();

    s->codec = enc->codec_id;
    s->length = 0;
    s->packet_count = 0;

    for (int f = 0; f < FRAMES; f++) {
        if (av_frame_make_writable(frame) < 0) {
            bench_fail("make_writable");
        }
        fill_frame(frame, video + (size_t)f * frame_bytes());
        frame->pts = f;

        if (avcodec_send_frame(enc, frame) < 0) {
            bench_fail("send_frame");
        }
        drain_encoder(enc, packet, s);
    }

    /* A NULL frame flushes the frames the encoder is still holding. */
    avcodec_send_frame(enc, NULL);
    drain_encoder(enc, packet, s);

    /* Codec headers (e.g. ffv1's) the decoder needs before the first packet. */
    av_freep(&s->extradata);
    s->extradata_size = enc->extradata_size;
    if (enc->extradata_size > 0) {
        s->extradata = av_mallocz((size_t)enc->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(s->extradata, enc->extradata, (size_t)enc->extradata_size);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
}


/* ---- decode ------------------------------------------------------------ */

/* Folds the visible pixels of every plane into `h`, whatever the format. */
static uint32_t hash_frame(uint32_t h, const AVFrame *frame) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int planes = av_pix_fmt_count_planes(frame->format);

    for (int p = 0; p < planes; p++) {
        int is_chroma = (p == 1 || p == 2);
        int rows = is_chroma ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
        int row_bytes = av_image_get_linesize(frame->format, frame->width, p);

        for (int y = 0; y < rows; y++) {
            h = fold32(h, frame->data[p] + (size_t)y * frame->linesize[p], (size_t)row_bytes);
        }
    }
    return h;
}

/* Decodes the whole stream, folding every frame into `h`. */
static uint32_t decode(const Stream *s, uint32_t h, int *frames_out) {
    AVCodecContext *dec = new_decoder(s->codec);
    dec->width = luma_width;
    dec->height = luma_height;

    if (s->extradata_size > 0) {
        dec->extradata = av_mallocz((size_t)s->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(dec->extradata, s->extradata, (size_t)s->extradata_size);
        dec->extradata_size = s->extradata_size;
    }

    if (avcodec_open2(dec, dec->codec, NULL) < 0) {
        bench_fail("cannot open decoder %s", dec->codec->name);
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    size_t offset = 0;
    *frames_out = 0;

    /* One pass per packet, then one more with NULL to flush the decoder. */
    for (int i = 0; i <= s->packet_count; i++) {
        int ret;
        if (i < s->packet_count) {
            packet->data = s->data + offset;
            packet->size = s->packet_sizes[i];
            offset += (size_t)s->packet_sizes[i];
            ret = avcodec_send_packet(dec, packet);
        } else {
            ret = avcodec_send_packet(dec, NULL);
        }
        if (ret < 0) {
            bench_fail("send_packet %d", ret);
        }

        while ((ret = avcodec_receive_frame(dec, frame)) == 0) {
            h = hash_frame(h, frame);
            (*frames_out)++;
            av_frame_unref(frame);
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            bench_fail("receive_frame %d", ret);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec);
    return h;
}


/* ---- benchmark --------------------------------------------------------- */

BENCH_EXPORT uint32_t runtime(void) {
    av_log_set_level(AV_LOG_ERROR);

    uint8_t *video = load_video();
    Stream stream = {0};
    uint32_t h = 2166136261u;

    if (MODE == 0) {
        encode(video, &stream);
    }

    for (long it = 0; it < ITERS; it++) {
        if (MODE == 0) {
            int frames;
            h = decode(&stream, h, &frames);
            if (frames != FRAMES) {
                bench_fail("decoded %d of %d frames", frames, FRAMES);
            }
        } else {
            encode(video, &stream);
            h = fold32(h, stream.data, stream.length);
        }
    }

    free(video);
    stream_free(&stream);
    return h;
}

BENCH_MAIN
