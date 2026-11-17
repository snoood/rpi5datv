/*
 * rpi5datv - programme source, encoder and TS multiplexer (see source.h)
 */
#include "source.h"
#include "avsync.h"
#include "testcard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define FPS 25
#define AUDIO_RATE 48000
#define AUDIO_PER_FRAME (AUDIO_RATE / FPS)   /* 1920 */

#if LIBAVFORMAT_VERSION_MAJOR >= 61
#define AVIO_WBUF const uint8_t
#else
#define AVIO_WBUF uint8_t
#endif

/* ------------------------------------------------------------------ */
/* Planning                                                           */
/* ------------------------------------------------------------------ */

void source_plan(source_cfg_t *cfg)
{
    const double r = cfg->ts_bitrate;

    if (cfg->audio && r >= 250000) {
        if (r >= 1500000) {
            cfg->audio_bitrate = 128000;
            cfg->audio_channels = 2;
        } else {
            cfg->audio_bitrate = 64000;
            cfg->audio_channels = 1;
        }
    } else {
        cfg->audio_bitrate = 0;
        cfg->audio_channels = 0;
    }

    /* TS/PES/PSI/PCR overhead plus a safety margin so the mux never
     * has to exceed muxrate. */
    double v = r * 0.80 - cfg->audio_bitrate * 1.10;
    if (v < 40000)
        v = 40000;
    cfg->video_bitrate = (int)v;

    if (cfg->width <= 0 || cfg->height <= 0) {
        if (v >= 1200000)      { cfg->width = 1024; cfg->height = 576; }
        else if (v >= 600000)  { cfg->width = 768;  cfg->height = 432; }
        else if (v >= 300000)  { cfg->width = 640;  cfg->height = 360; }
        else if (v >= 150000)  { cfg->width = 512;  cfg->height = 288; }
        else                   { cfg->width = 384;  cfg->height = 216; }
    }
}

/* ------------------------------------------------------------------ */
/* Output: encoders + mpegts muxer writing into the ring buffer       */
/* ------------------------------------------------------------------ */

typedef struct {
    source_cfg_t *cfg;
    AVFormatContext *oc;
    AVIOContext *avio;
    AVCodecContext *venc, *aenc;
    AVStream *vst, *ast;
    AVFrame *vframe;          /* encoder input picture */
    AVFrame *aframe;
    AVAudioFifo *afifo;
    AVPacket *pkt;
    int64_t vpts;             /* frames sent */
    int64_t apts;             /* samples sent to encoder */
    int64_t a_in;             /* samples pushed into fifo */
    int failed;
} out_t;

static int ts_write(void *opaque, AVIO_WBUF *buf, int size)
{
    out_t *o = opaque;
    if (ringbuf_write(o->cfg->ring, buf, (size_t)size) < 0)
        return AVERROR_EXIT;
    return size;
}

static void log_err(const char *what, int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "%s: %s\n", what, buf);
}

static int drain_encoder(out_t *o, AVCodecContext *enc, AVStream *st)
{
    int ret;
    for (;;) {
        ret = avcodec_receive_packet(enc, o->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        av_packet_rescale_ts(o->pkt, enc->time_base, st->time_base);
        o->pkt->stream_index = st->index;
        ret = av_interleaved_write_frame(o->oc, o->pkt);
        if (ret < 0)
            return ret;
    }
}

static int encode(out_t *o, AVCodecContext *enc, AVStream *st, AVFrame *f)
{
    int ret = avcodec_send_frame(enc, f);
    if (ret < 0)
        return ret;
    return drain_encoder(o, enc, st);
}

static int out_open(out_t *o, source_cfg_t *cfg)
{
    int ret;
    memset(o, 0, sizeof(*o));
    o->cfg = cfg;
    o->pkt = av_packet_alloc();

    ret = avformat_alloc_output_context2(&o->oc, NULL, "mpegts", NULL);
    if (ret < 0) {
        log_err("mpegts muxer", ret);
        return ret;
    }

    /* ---- video encoder ---- */
    const AVCodec *vc = avcodec_find_encoder_by_name("libx264");
    if (!vc)
        vc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vc) {
        fprintf(stderr, "No H.264 encoder available (install libavcodec with libx264)\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    o->venc = avcodec_alloc_context3(vc);
    o->venc->width = cfg->width;
    o->venc->height = cfg->height;
    o->venc->pix_fmt = AV_PIX_FMT_YUV420P;
    o->venc->time_base = (AVRational){ 1, FPS };
    o->venc->framerate = (AVRational){ FPS, 1 };
    o->venc->sample_aspect_ratio = (AVRational){ 1, 1 };
    o->venc->bit_rate = cfg->video_bitrate;
    o->venc->rc_max_rate = cfg->video_bitrate;
    o->venc->rc_buffer_size = cfg->video_bitrate;   /* 1 s VBV */
    o->venc->gop_size = 2 * FPS;
    o->venc->max_b_frames = 0;
    o->venc->thread_count = 0;
    if (strcmp(vc->name, "libx264") == 0) {
        av_opt_set(o->venc->priv_data, "preset", "veryfast", 0);
        av_opt_set(o->venc->priv_data, "tune", "zerolatency", 0);
        av_opt_set(o->venc->priv_data, "profile", "main", 0);
        av_opt_set(o->venc->priv_data, "x264-params", "force-cfr=1", 0);
    }
    ret = avcodec_open2(o->venc, vc, NULL);
    if (ret < 0) {
        log_err("open H.264 encoder", ret);
        return ret;
    }
    o->vst = avformat_new_stream(o->oc, NULL);
    o->vst->time_base = o->venc->time_base;
    avcodec_parameters_from_context(o->vst->codecpar, o->venc);

    o->vframe = av_frame_alloc();
    o->vframe->format = AV_PIX_FMT_YUV420P;
    o->vframe->width = cfg->width;
    o->vframe->height = cfg->height;
    if ((ret = av_frame_get_buffer(o->vframe, 0)) < 0)
        return ret;

    /* ---- audio encoder (MPEG-1 Layer II) ---- */
    if (cfg->audio_bitrate > 0) {
        const AVCodec *ac = avcodec_find_encoder(AV_CODEC_ID_MP2);
        if (!ac) {
            fprintf(stderr, "No MP2 encoder; continuing without audio\n");
            cfg->audio_bitrate = 0;
        } else {
            o->aenc = avcodec_alloc_context3(ac);
            o->aenc->sample_fmt = AV_SAMPLE_FMT_S16;
            o->aenc->sample_rate = AUDIO_RATE;
            av_channel_layout_default(&o->aenc->ch_layout, cfg->audio_channels);
            o->aenc->bit_rate = cfg->audio_bitrate;
            o->aenc->time_base = (AVRational){ 1, AUDIO_RATE };
            ret = avcodec_open2(o->aenc, ac, NULL);
            if (ret < 0) {
                log_err("open MP2 encoder", ret);
                return ret;
            }
            o->ast = avformat_new_stream(o->oc, NULL);
            o->ast->time_base = o->aenc->time_base;
            avcodec_parameters_from_context(o->ast->codecpar, o->aenc);

            o->aframe = av_frame_alloc();
            o->aframe->format = AV_SAMPLE_FMT_S16;
            o->aframe->sample_rate = AUDIO_RATE;
            o->aframe->nb_samples = o->aenc->frame_size;
            av_channel_layout_copy(&o->aframe->ch_layout, &o->aenc->ch_layout);
            if ((ret = av_frame_get_buffer(o->aframe, 0)) < 0)
                return ret;
            o->afifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, cfg->audio_channels,
                                           AUDIO_RATE);
        }
    }

    /* ---- service information ---- */
    av_dict_set(&o->oc->metadata, "service_name", cfg->service_name, 0);
    av_dict_set(&o->oc->metadata, "service_provider", cfg->provider_name, 0);

    /* ---- custom I/O into the ring buffer ---- */
    const int bufsize = 188 * 64;
    uint8_t *iobuf = av_malloc(bufsize);
    o->avio = avio_alloc_context(iobuf, bufsize, 1, o, NULL, ts_write, NULL);
    o->oc->pb = o->avio;

    /* Same default as the ffmpeg CLI (-muxdelay 0.7): the first PCR leads
     * the first DTS so the decoder buffer can fill. Without it the muxer
     * produces "dts < pcr" (invalid TS) on every frame. */
    o->oc->max_delay = 700000;

    AVDictionary *opts = NULL;
    av_dict_set_int(&opts, "muxrate", (int64_t)llround(cfg->ts_bitrate), 0);
    /* PSI/SI repetition: within TR 101 290 limits (PAT/PMT <= 0.5 s,
     * SDT <= 2 s) but sparse, since every packet counts at low rates. */
    av_dict_set(&opts, "pat_period", "0.4", 0);
    av_dict_set(&opts, "sdt_period", "1.5", 0);
    av_dict_set_int(&opts, "mpegts_service_id", 1, 0);
    ret = avformat_write_header(o->oc, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        log_err("write TS header", ret);
        return ret;
    }
    return 0;
}

static int out_audio_drain(out_t *o, int flush)
{
    int ret;
    const int fs = o->aenc->frame_size;
    while (av_audio_fifo_size(o->afifo) >= fs ||
           (flush && av_audio_fifo_size(o->afifo) > 0)) {
        if ((ret = av_frame_make_writable(o->aframe)) < 0)
            return ret;
        int n = av_audio_fifo_read(o->afifo, (void **)o->aframe->data, fs);
        if (n < fs) /* pad final partial frame with silence */
            av_samples_set_silence(o->aframe->data, n, fs - n,
                                   o->aenc->ch_layout.nb_channels, AV_SAMPLE_FMT_S16);
        o->aframe->pts = o->apts;
        o->apts += fs;
        if ((ret = encode(o, o->aenc, o->ast, o->aframe)) < 0)
            return ret;
    }
    return 0;
}

/* Push interleaved s16 samples. */
static int out_audio_push(out_t *o, const int16_t *s, int nb)
{
    if (!o->aenc)
        return 0;
    void *planes[1] = { (void *)s };
    if (av_audio_fifo_write(o->afifo, planes, nb) < nb)
        return AVERROR(ENOMEM);
    o->a_in += nb;
    return out_audio_drain(o, 0);
}

static int out_audio_silence(out_t *o, int nb)
{
    if (!o->aenc || nb <= 0)
        return 0;
    int ch = o->aenc->ch_layout.nb_channels;
    int16_t *z = calloc((size_t)nb * ch, sizeof(int16_t));
    if (!z)
        return AVERROR(ENOMEM);
    int ret = out_audio_push(o, z, nb);
    free(z);
    return ret;
}

/* Send o->vframe as the next picture. */
static int out_video(out_t *o)
{
    o->vframe->pts = o->vpts++;
    return encode(o, o->venc, o->vst, o->vframe);
}

static void out_close(out_t *o, int flush)
{
    if (o->oc && flush && !o->failed) {
        if (o->aenc) {
            out_audio_drain(o, 1);
            encode(o, o->aenc, o->ast, NULL);
        }
        if (o->venc)
            encode(o, o->venc, o->vst, NULL);
        av_write_trailer(o->oc);
    }
    if (o->avio) {
        av_freep(&o->avio->buffer);
        avio_context_free(&o->avio);
    }
    avcodec_free_context(&o->venc);
    avcodec_free_context(&o->aenc);
    av_frame_free(&o->vframe);
    av_frame_free(&o->aframe);
    if (o->afifo)
        av_audio_fifo_free(o->afifo);
    av_packet_free(&o->pkt);
    if (o->oc)
        avformat_free_context(o->oc);
    o->oc = NULL;
}

/* ------------------------------------------------------------------ */
/* Test card source                                                   */
/* ------------------------------------------------------------------ */

static int run_testcard(source_cfg_t *cfg, out_t *o)
{
    const int w = cfg->width, h = cfg->height;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    struct SwsContext *sws = sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h,
                                            AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                            NULL, NULL, NULL);
    if (!rgb || !sws) {
        free(rgb);
        return AVERROR(ENOMEM);
    }
    testcard_t tc = { w, h, cfg->service_name, cfg->provider_name, cfg->info_line };

    int ch = o->aenc ? o->aenc->ch_layout.nb_channels : 0;
    int16_t *tone = ch ? malloc(sizeof(int16_t) * AUDIO_PER_FRAME * ch) : NULL;
    double ph = 0;
    int ret = 0;

    for (long frame = 0; !*cfg->stop; frame++) {
        testcard_render(&tc, frame, rgb);
        if ((ret = av_frame_make_writable(o->vframe)) < 0)
            break;
        const uint8_t *src[1] = { rgb };
        int src_ls[1] = { w * 3 };
        sws_scale(sws, src, src_ls, 0, h, o->vframe->data, o->vframe->linesize);
        if ((ret = out_video(o)) < 0)
            break;

        if (tone) {
            /* 1 kHz line-up tone at -12 dBFS */
            for (int i = 0; i < AUDIO_PER_FRAME; i++) {
                int16_t v = (int16_t)(8192.0 * sin(ph));
                ph += 2.0 * M_PI * 1000.0 / AUDIO_RATE;
                if (ph > 2.0 * M_PI)
                    ph -= 2.0 * M_PI;
                for (int c = 0; c < ch; c++)
                    tone[i * ch + c] = v;
            }
            if ((ret = out_audio_push(o, tone, AUDIO_PER_FRAME)) < 0)
                break;
        }
    }
    free(tone);
    free(rgb);
    sws_freeContext(sws);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Video file source                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    source_cfg_t *cfg;
    out_t *o;
    AVFormatContext *ic;
    int vidx, aidx;
    AVCodecContext *vdec, *adec;
    struct SwsContext *sws;
    int sws_w, sws_h, sws_fmt;
    int dst_x, dst_y, dst_w, dst_h;
    SwrContext *swr;
    int16_t *abuf;
    int abuf_cap;
    AVFrame *dframe;
    int have_last;            /* o->vframe holds a valid picture */
    avsync_t sync;            /* A/V mapping and corrections (avsync.c) */
} filesrc_t;


static void clear_picture(AVFrame *f)
{
    for (int y = 0; y < f->height; y++)
        memset(f->data[0] + (size_t)y * f->linesize[0], 16, f->width);
    for (int y = 0; y < f->height / 2; y++) {
        memset(f->data[1] + (size_t)y * f->linesize[1], 128, f->width / 2);
        memset(f->data[2] + (size_t)y * f->linesize[2], 128, f->width / 2);
    }
}

/* Scale decoded frame into o->vframe, letterboxed/pillarboxed. */
static int scale_into_output(filesrc_t *fsrc, AVFrame *src)
{
    out_t *o = fsrc->o;
    int ret;
    if (!fsrc->sws || src->width != fsrc->sws_w || src->height != fsrc->sws_h ||
        src->format != fsrc->sws_fmt) {
        sws_freeContext(fsrc->sws);
        const int W = o->vframe->width, H = o->vframe->height;
        AVRational sar = src->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0)
            sar = (AVRational){ 1, 1 };
        double dar = (double)src->width * sar.num / ((double)src->height * sar.den);
        int dw, dh;
        if (dar > (double)W / H) {
            dw = W;
            dh = ((int)lround(W / dar)) & ~1;
        } else {
            dh = H;
            dw = ((int)lround(H * dar)) & ~1;
        }
        if (dw < 2) dw = 2;
        if (dh < 2) dh = 2;
        fsrc->dst_w = dw;
        fsrc->dst_h = dh;
        fsrc->dst_x = ((W - dw) / 2) & ~1;
        fsrc->dst_y = ((H - dh) / 2) & ~1;
        fsrc->sws = sws_getContext(src->width, src->height, src->format, dw, dh,
                                   AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
        if (!fsrc->sws)
            return AVERROR(EINVAL);
        fsrc->sws_w = src->width;
        fsrc->sws_h = src->height;
        fsrc->sws_fmt = src->format;
        if ((ret = av_frame_make_writable(o->vframe)) < 0)
            return ret;
        clear_picture(o->vframe);
    }
    if ((ret = av_frame_make_writable(o->vframe)) < 0)
        return ret;
    AVFrame *d = o->vframe;
    uint8_t *dst[3] = {
        d->data[0] + (size_t)fsrc->dst_y * d->linesize[0] + fsrc->dst_x,
        d->data[1] + (size_t)(fsrc->dst_y / 2) * d->linesize[1] + fsrc->dst_x / 2,
        d->data[2] + (size_t)(fsrc->dst_y / 2) * d->linesize[2] + fsrc->dst_x / 2,
    };
    sws_scale(fsrc->sws, (const uint8_t *const *)src->data, src->linesize, 0,
              src->height, dst, d->linesize);
    return 0;
}

/* Emit the held picture once (plus silence if the file has no audio). */
static int emit_held(filesrc_t *fsrc)
{
    int ret = out_video(fsrc->o);
    if (ret < 0)
        return ret;
    if (fsrc->aidx < 0)
        ret = out_audio_silence(fsrc->o, AUDIO_PER_FRAME);
    return ret;
}

static double frame_time(AVFormatContext *ic, int idx, AVFrame *f)
{
    int64_t ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE)
        return NAN;
    return ts * av_q2d(ic->streams[idx]->time_base);
}

static int handle_video_frame(filesrc_t *fsrc, AVFrame *f)
{
    out_t *o = fsrc->o;
    avsync_video_t v = avsync_video(&fsrc->sync, frame_time(fsrc->ic, fsrc->vidx, f), o->vpts);
    if (v.jumped && fsrc->cfg->verbose)
        fprintf(stderr, "Input timestamps jumped %+.2f s; re-synchronising\n", v.jump_s);

    int ret;
    /* Output frames whose time precedes this picture show the old one. */
    while (fsrc->have_last && !*fsrc->cfg->stop &&
           (double)o->vpts / FPS < v.target - 1e-6) {
        if ((ret = emit_held(fsrc)) < 0)
            return ret;
    }
    if ((ret = scale_into_output(fsrc, f)) < 0)
        return ret;
    fsrc->have_last = 1;
    return 0;
}

static int handle_audio_frame(filesrc_t *fsrc, AVFrame *f)
{
    out_t *o = fsrc->o;
    if (!o->aenc)
        return 0;
    int ch = o->aenc->ch_layout.nb_channels;
    int need = swr_get_out_samples(fsrc->swr, f->nb_samples);
    if (need > fsrc->abuf_cap) {
        free(fsrc->abuf);
        fsrc->abuf = malloc(sizeof(int16_t) * need * ch);
        fsrc->abuf_cap = fsrc->abuf ? need : 0;
        if (!fsrc->abuf)
            return AVERROR(ENOMEM);
    }
    double s = frame_time(fsrc->ic, fsrc->aidx, f);
    uint8_t *outp[1] = { (uint8_t *)fsrc->abuf };
    int n = swr_convert(fsrc->swr, outp, need, (const uint8_t **)f->extended_data,
                        f->nb_samples);
    if (n < 0)
        return n;

    avsync_audio_t a = avsync_audio(&fsrc->sync, s, n, o->a_in, o->vpts);
    if (fsrc->cfg->verbose) {
        if (a.jumped)
            fprintf(stderr, "Audio timestamps jumped; re-synchronising\n");
        else if (a.insert)
            fprintf(stderr, "Audio behind by %.0f ms; inserted silence\n", a.error_s * 1000);
        else if (a.skip)
            fprintf(stderr, "Audio ahead by %.0f ms; dropping %d samples\n",
                    -a.error_s * 1000, a.skip);
    }
    int ret;
    if (a.insert > 0 && (ret = out_audio_silence(o, (int)a.insert)) < 0)
        return ret;
    if (a.skip >= n)
        return 0;
    return out_audio_push(o, fsrc->abuf + (size_t)a.skip * ch, n - a.skip);
}

static int decode_packet(filesrc_t *fsrc, AVCodecContext *dec, AVPacket *pkt, int is_video)
{
    int ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && ret != AVERROR_EOF)
        return ret == AVERROR_INVALIDDATA ? 0 : ret;
    for (;;) {
        ret = avcodec_receive_frame(dec, fsrc->dframe);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        ret = is_video ? handle_video_frame(fsrc, fsrc->dframe)
                       : handle_audio_frame(fsrc, fsrc->dframe);
        av_frame_unref(fsrc->dframe);
        if (ret < 0)
            return ret;
    }
}

/* End of one pass through the file: hold the last picture for one
 * frame and bring audio and video back into step. */
static int end_segment(filesrc_t *fsrc)
{
    out_t *o = fsrc->o;
    int ret;
    if (fsrc->have_last && (ret = emit_held(fsrc)) < 0)
        return ret;
    if (o->aenc) {
        int64_t want = o->vpts * AUDIO_PER_FRAME;
        if (o->a_in < want) {
            if ((ret = out_audio_silence(o, (int)(want - o->a_in))) < 0)
                return ret;
        }
        while (fsrc->have_last && o->a_in > o->vpts * AUDIO_PER_FRAME + AUDIO_PER_FRAME) {
            if ((ret = out_video(o)) < 0)
                return ret;
        }
    }
    avsync_new_segment(&fsrc->sync);   /* next video frame re-bases */
    return 0;
}

static int run_file(source_cfg_t *cfg, out_t *o)
{
    filesrc_t fsrc;
    memset(&fsrc, 0, sizeof(fsrc));
    fsrc.cfg = cfg;
    fsrc.o = o;
    fsrc.vidx = fsrc.aidx = -1;
    avsync_init(&fsrc.sync, FPS, AUDIO_RATE);
    int ret;

    if ((ret = avformat_open_input(&fsrc.ic, cfg->input, NULL, NULL)) < 0) {
        log_err(cfg->input, ret);
        return ret;
    }
    if ((ret = avformat_find_stream_info(fsrc.ic, NULL)) < 0) {
        log_err("stream info", ret);
        goto done;
    }

    const AVCodec *dc = NULL;
    fsrc.vidx = av_find_best_stream(fsrc.ic, AVMEDIA_TYPE_VIDEO, -1, -1, &dc, 0);
    if (fsrc.vidx < 0) {
        fprintf(stderr, "%s: no video stream\n", cfg->input);
        ret = fsrc.vidx;
        goto done;
    }
    fsrc.vdec = avcodec_alloc_context3(dc);
    avcodec_parameters_to_context(fsrc.vdec, fsrc.ic->streams[fsrc.vidx]->codecpar);
    fsrc.vdec->thread_count = 0;
    if ((ret = avcodec_open2(fsrc.vdec, dc, NULL)) < 0) {
        log_err("open video decoder", ret);
        goto done;
    }

    if (o->aenc) {
        const AVCodec *adc = NULL;
        int a = av_find_best_stream(fsrc.ic, AVMEDIA_TYPE_AUDIO, -1, fsrc.vidx, &adc, 0);
        if (a >= 0) {
            fsrc.adec = avcodec_alloc_context3(adc);
            avcodec_parameters_to_context(fsrc.adec, fsrc.ic->streams[a]->codecpar);
            if (avcodec_open2(fsrc.adec, adc, NULL) >= 0) {
                ret = swr_alloc_set_opts2(&fsrc.swr, &o->aenc->ch_layout,
                                          AV_SAMPLE_FMT_S16, AUDIO_RATE,
                                          &fsrc.adec->ch_layout, fsrc.adec->sample_fmt,
                                          fsrc.adec->sample_rate, 0, NULL);
                if (ret >= 0 && swr_init(fsrc.swr) >= 0)
                    fsrc.aidx = a;
            }
            if (fsrc.aidx < 0)
                fprintf(stderr, "Cannot decode audio track; sending silence\n");
        }
    }
    for (unsigned i = 0; i < fsrc.ic->nb_streams; i++)
        if ((int)i != fsrc.vidx && (int)i != fsrc.aidx)
            fsrc.ic->streams[i]->discard = AVDISCARD_ALL;

    if (cfg->verbose)
        av_dump_format(fsrc.ic, 0, cfg->input, 0);

    fsrc.dframe = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    ret = 0;
    while (!*cfg->stop) {
        ret = av_read_frame(fsrc.ic, pkt);
        if (ret == AVERROR_EOF || (ret < 0 && avio_feof(fsrc.ic->pb))) {
            /* flush decoders */
            decode_packet(&fsrc, fsrc.vdec, NULL, 1);
            if (fsrc.aidx >= 0)
                decode_packet(&fsrc, fsrc.adec, NULL, 0);
            if ((ret = end_segment(&fsrc)) < 0)
                break;
            if (!cfg->loop) {
                ret = 0;
                break;
            }
            int64_t start = fsrc.ic->start_time != AV_NOPTS_VALUE ? fsrc.ic->start_time : 0;
            if ((ret = avformat_seek_file(fsrc.ic, -1, INT64_MIN, start, start, 0)) < 0) {
                log_err("seek to start for loop", ret);
                break;
            }
            avcodec_flush_buffers(fsrc.vdec);
            if (fsrc.aidx >= 0)
                avcodec_flush_buffers(fsrc.adec);
            if (cfg->verbose)
                fprintf(stderr, "Looping %s\n", cfg->input);
            continue;
        }
        if (ret < 0) {
            log_err("read", ret);
            break;
        }
        if (pkt->stream_index == fsrc.vidx)
            ret = decode_packet(&fsrc, fsrc.vdec, pkt, 1);
        else if (pkt->stream_index == fsrc.aidx)
            ret = decode_packet(&fsrc, fsrc.adec, pkt, 0);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }
    av_packet_free(&pkt);
    if (cfg->verbose && (fsrc.sync.n_rebase || fsrc.sync.n_afix))
        fprintf(stderr, "Input re-synchronisations: %ld timestamp jumps, %ld audio corrections\n",
                fsrc.sync.n_rebase, fsrc.sync.n_afix);

done:
    av_frame_free(&fsrc.dframe);
    free(fsrc.abuf);
    swr_free(&fsrc.swr);
    sws_freeContext(fsrc.sws);
    avcodec_free_context(&fsrc.vdec);
    avcodec_free_context(&fsrc.adec);
    avformat_close_input(&fsrc.ic);
    return ret;
}

/* ------------------------------------------------------------------ */

int source_run(source_cfg_t *cfg)
{
    out_t o;
    int ret = out_open(&o, cfg);
    if (ret >= 0) {
        if (cfg->testcard)
            ret = run_testcard(cfg, &o);
        else
            ret = run_file(cfg, &o);
    }
    if (ret == AVERROR_EXIT || *cfg->stop) {
        o.failed = 1;               /* ring closed: we are shutting down */
        ret = 0;
    } else if (ret < 0) {
        o.failed = 1;
        log_err("source", ret);
    }
    out_close(&o, 1);
    ringbuf_close(cfg->ring);
    return ret < 0 ? -1 : 0;
}
