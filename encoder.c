/*
 * Decode the best video (and audio, when present) and write one
 * single-pass MPEG-4 / MP3 Matroska file. Cancel is checked between
 * packets. The partial output is removed unless the trailer is written.
 */
#include <errno.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "encoder.h"
#include "vt_avio.h"
#include "vt_util.h"

struct EncodeCtx {
    VTFile input;
    VTFile output;
    AVFormatContext *in;
    AVFormatContext *out;
    AVCodecContext *vdec;
    AVCodecContext *venc;
    AVCodecContext *adec;
    AVCodecContext *aenc;
    AVFrame *decoded;
    AVFrame *scaled;
    AVFrame *audio;
    AVPacket *packet;
    AVPacket *encoded;
    struct SwsContext *sws;
    struct SwrContext *swr;
    AVAudioFifo *fifo;
    AVStream *vst;
    AVStream *ast;
    const volatile LONG *cancel;
    VTEncodeProgressFunc progress;
    APTR progress_user;
    char *error;
    ULONG error_len;
    long long duration_us;
    long long video_pts;
    long long audio_pts;
    int video_index;
    int audio_index;
    int enc_w;
    int enc_h;
    int sws_w;
    int sws_h;
    int sws_fmt;
    int last_percent;
    BOOL output_created;
    BOOL header_ok;
};

static int stopped(const struct EncodeCtx *cx)
{
    return cx->cancel && *cx->cancel;
}

static int fail(struct EncodeCtx *cx, int err, const char *what)
{
    if (stopped(cx))
        return AVERROR_EXIT;
    VT_AVError(cx->error, cx->error_len, err, what);
    return err < 0 ? err : AVERROR(EINVAL);
}

static enum AVSampleFormat pick_sample_fmt(const AVCodec *codec)
{
    const void *configs = NULL;
    const enum AVSampleFormat *fmts;
    const enum AVSampleFormat prefer[4] = {
        AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT
    };
    int index;
    int choice;

    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0, &configs, NULL) < 0 || !configs)
        return AV_SAMPLE_FMT_S16P;
    fmts = (const enum AVSampleFormat *)configs;
    for (choice = 0; choice < 4; choice++) {
        for (index = 0; fmts[index] != AV_SAMPLE_FMT_NONE; index++) {
            if (fmts[index] == prefer[choice])
                return prefer[choice];
        }
    }
    return fmts[0];
}

static int pick_sample_rate(const AVCodec *codec, int source)
{
    const void *configs = NULL;
    const int *rates;
    int index;

    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_RATE,
                                     0, &configs, NULL) < 0 || !configs)
        return (source >= 8000 && source <= 48000) ? source : 44100;
    rates = (const int *)configs;
    if (source > 0) {
        for (index = 0; rates[index]; index++) {
            if (rates[index] == source)
                return source;
        }
    }
    for (index = 0; rates[index]; index++) {
        if (rates[index] == 44100)
            return 44100;
    }
    return rates[0] ? rates[0] : 44100;
}

static void report(struct EncodeCtx *cx)
{
    int percent = 0;
    long long us;

    if (cx->duration_us > 0 && cx->venc && cx->venc->time_base.den > 0) {
        us = (long long)av_rescale_q(cx->video_pts, cx->venc->time_base, AV_TIME_BASE_Q);
        if (us < 0)
            us = 0;
        percent = (int)((us * 100) / cx->duration_us);
    } else if (cx->input.size > 0 && cx->input.bytes_read > 0) {
        percent = (int)((cx->input.bytes_read * 100) / cx->input.size);
    }
    if (percent < 0)
        percent = 0;
    if (percent > 99)
        percent = 99;
    if (percent == cx->last_percent)
        return;
    cx->last_percent = percent;
    if (cx->progress)
        cx->progress(percent, cx->progress_user);
}

static int write_encoded(struct EncodeCtx *cx, AVCodecContext *enc, AVStream *stream, AVFrame *frame)
{
    int err;

    err = avcodec_send_frame(enc, frame);
    if (err < 0)
        return fail(cx, err, "Could not encode");
    for (;;) {
        if (stopped(cx))
            return AVERROR_EXIT;
        err = avcodec_receive_packet(enc, cx->encoded);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return 0;
        if (err < 0)
            return fail(cx, err, "Could not read an encoded packet");
        cx->encoded->stream_index = stream->index;
        av_packet_rescale_ts(cx->encoded, enc->time_base, stream->time_base);
        err = av_interleaved_write_frame(cx->out, cx->encoded);
        av_packet_unref(cx->encoded);
        if (err < 0)
            return fail(cx, err, "Could not write the output");
    }
}

static int handle_video(AVFrame *frame, void *user)
{
    struct EncodeCtx *cx = (struct EncodeCtx *)user;
    int err;

    if (stopped(cx))
        return AVERROR_EXIT;
    if (!frame || frame->width < 2 || frame->height < 2)
        return fail(cx, 0, "The picture size is unusable.");
    if (!cx->sws || cx->sws_w != frame->width || cx->sws_h != frame->height ||
        cx->sws_fmt != frame->format) {
        if (cx->sws)
            sws_freeContext(cx->sws);
        cx->sws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                                 cx->enc_w, cx->enc_h, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, NULL, NULL, NULL);
        cx->sws_w = frame->width;
        cx->sws_h = frame->height;
        cx->sws_fmt = frame->format;
        if (!cx->sws)
            return fail(cx, 0, "Could not convert the picture.");
    }
    if ((err = av_frame_make_writable(cx->scaled)) < 0)
        return fail(cx, err, "Out of memory.");
    sws_scale(cx->sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, (uint8_t *const *)cx->scaled->data, cx->scaled->linesize);
    cx->scaled->pts = cx->video_pts++;
    err = write_encoded(cx, cx->venc, cx->vst, cx->scaled);
    if (err < 0)
        return err;
    report(cx);
    return 0;
}

static int fifo_write(struct EncodeCtx *cx, uint8_t **data, int samples)
{
    if (samples <= 0)
        return 0;
    if (av_audio_fifo_write(cx->fifo, (void *const *)data, samples) < samples)
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    return 0;
}

static int convert_audio(struct EncodeCtx *cx, AVFrame *frame)
{
    int in_rate = cx->adec->sample_rate;
    int out_rate = cx->aenc->sample_rate;
    int in_samples = frame ? frame->nb_samples : 0;
    int out_samples;
    int got;
    int err;
    uint8_t **data = NULL;
    int linesize = 0;
    int64_t delay;
    int spins = 0;

    for (;;) {
        const uint8_t **input = NULL;

        if (++spins > 64)
            return 0;

        if (stopped(cx))
            return AVERROR_EXIT;
        delay = swr_get_delay(cx->swr, in_rate);
        out_samples = (int)av_rescale_rnd(delay + in_samples, out_rate, in_rate, AV_ROUND_UP);
        if (out_samples <= 0)
            return 0;
        if (out_samples > 16384)
            out_samples = 16384;
        err = av_samples_alloc_array_and_samples(&data, &linesize, cx->aenc->ch_layout.nb_channels,
                                                 out_samples, cx->aenc->sample_fmt, 0);
        if (err < 0)
            return fail(cx, err, "Out of memory.");
        if (frame)
            input = (const uint8_t **)frame->extended_data;
        got = swr_convert(cx->swr, (uint8_t *const *)data, out_samples,
                          (const uint8_t *const *)input, in_samples);
        if (got < 0) {
            av_freep(&data[0]);
            av_freep(&data);
            return fail(cx, got, "Could not resample audio.");
        }
        err = fifo_write(cx, data, got);
        av_freep(&data[0]);
        av_freep(&data);
        if (err < 0)
            return err;
        frame = NULL;
        in_samples = 0;
        if (got < out_samples)
            return 0;
    }
}

static int drain_audio(struct EncodeCtx *cx, BOOL finish)
{
    int frame_size = cx->aenc->frame_size > 0 ? cx->aenc->frame_size : 1152;
    int err;

    for (;;) {
        int available = av_audio_fifo_size(cx->fifo);
        int count;

        if (stopped(cx))
            return AVERROR_EXIT;
        if (available <= 0)
            break;
        if (!finish && available < frame_size)
            break;
        count = available > frame_size ? frame_size : available;
        av_frame_unref(cx->audio);
        cx->audio->nb_samples = count;
        cx->audio->format = cx->aenc->sample_fmt;
        cx->audio->sample_rate = cx->aenc->sample_rate;
        if ((err = av_channel_layout_copy(&cx->audio->ch_layout, &cx->aenc->ch_layout)) < 0)
            return fail(cx, err, "Out of memory.");
        if ((err = av_frame_get_buffer(cx->audio, 0)) < 0)
            return fail(cx, err, "Out of memory.");
        if (av_audio_fifo_read(cx->fifo, (void *const *)cx->audio->extended_data, count) < count)
            return fail(cx, 0, "Could not buffer audio.");
        cx->audio->pts = cx->audio_pts;
        cx->audio_pts += count;
        err = write_encoded(cx, cx->aenc, cx->ast, cx->audio);
        av_frame_unref(cx->audio);
        if (err < 0)
            return err;
    }
    if (!finish)
        return 0;
    return write_encoded(cx, cx->aenc, cx->ast, NULL);
}

static int handle_audio(AVFrame *frame, void *user)
{
    struct EncodeCtx *cx = (struct EncodeCtx *)user;
    int err;

    err = convert_audio(cx, frame);
    if (err < 0)
        return err;
    return drain_audio(cx, FALSE);
}

static int pull_frames(struct EncodeCtx *cx, AVCodecContext *dec, int (*handle)(AVFrame *, void *))
{
    for (;;) {
        int err;

        if (stopped(cx))
            return AVERROR_EXIT;
        err = avcodec_receive_frame(dec, cx->decoded);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return 0;
        if (err < 0)
            return fail(cx, err, "Could not decode");
        err = handle(cx->decoded, cx);
        av_frame_unref(cx->decoded);
        if (err < 0)
            return err;
    }
}

static int push_packet(struct EncodeCtx *cx, AVCodecContext *dec, AVPacket *packet,
                       int (*handle)(AVFrame *, void *))
{
    int err = avcodec_send_packet(dec, packet);

    if (err < 0 && err != AVERROR_EOF)
        return fail(cx, err, "Could not decode");
    return pull_frames(cx, dec, handle);
}

static int open_decoder(struct EncodeCtx *cx, AVCodecContext **dest, int index)
{
    const AVCodec *codec;
    AVCodecContext *dec;
    AVStream *stream;
    int err;

    stream = cx->in->streams[index];
    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return fail(cx, AVERROR_DECODER_NOT_FOUND, "Could not find a decoder.");
    dec = avcodec_alloc_context3(codec);
    if (!dec)
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    if ((err = avcodec_parameters_to_context(dec, stream->codecpar)) < 0) {
        avcodec_free_context(&dec);
        return fail(cx, err, "Could not read the stream.");
    }
    dec->pkt_timebase = stream->time_base;
    dec->thread_count = 1;
    if ((err = avcodec_open2(dec, codec, NULL)) < 0) {
        avcodec_free_context(&dec);
        return fail(cx, err, "Could not open the decoder.");
    }
    *dest = dec;
    return 0;
}

static AVStream *add_stream(struct EncodeCtx *cx, AVCodecContext *enc)
{
    AVStream *stream;
    int err;

    stream = avformat_new_stream(cx->out, NULL);
    if (!stream) {
        fail(cx, AVERROR(ENOMEM), "Out of memory.");
        return NULL;
    }
    if ((err = avcodec_parameters_from_context(stream->codecpar, enc)) < 0) {
        fail(cx, err, "Could not set up the output stream.");
        return NULL;
    }
    stream->codecpar->codec_tag = 0;
    stream->time_base = enc->time_base;
    return stream;
}

static void close_encode(struct EncodeCtx *cx, const char *source, const char *output, VTEncodeResult result)
{
    if (cx->fifo)
        av_audio_fifo_free(cx->fifo);
    if (cx->swr)
        swr_free(&cx->swr);
    if (cx->sws)
        sws_freeContext(cx->sws);
    av_frame_free(&cx->decoded);
    av_frame_free(&cx->scaled);
    av_frame_free(&cx->audio);
    av_packet_free(&cx->packet);
    av_packet_free(&cx->encoded);
    avcodec_free_context(&cx->vdec);
    avcodec_free_context(&cx->venc);
    avcodec_free_context(&cx->adec);
    avcodec_free_context(&cx->aenc);
    if (cx->out) {
        cx->out->pb = NULL;
        avformat_free_context(cx->out);
        cx->out = NULL;
    }
    VT_FormatCloseInput(&cx->in, &cx->input);
    VT_FileClose(&cx->output);
    if (result != VT_ENCODE_OK && cx->output_created && output && output[0] &&
        !VT_StrEqI(output, source))
        VT_DeleteIfExists(output);
}

BOOL VT_EncodeSupported(const VTProfile *profile)
{
    if (!profile)
        return FALSE;
    if (profile->container != VT_CONTAINER_MKV)
        return FALSE;
    if (profile->video_codec != VT_VCODEC_MPEG4)
        return FALSE;
    if (profile->audio_codec != VT_ACODEC_MP3)
        return FALSE;
    if (profile->two_pass)
        return FALSE;
    if (profile->audio_gain_db != 0)
        return FALSE;
    return TRUE;
}

const char *VT_EncodeUnsupportedReason(void)
{
    return "This version encodes single-pass MPEG-4, MP3 and MKV.";
}

VTEncodeResult VT_EncodeFile(const char *source, const char *output, const VTProfile *profile,
                             const volatile LONG *cancel, VTEncodeProgressFunc progress, APTR user,
                             char *error, ULONG error_len)
{
    struct EncodeCtx cx;
    const AVCodec *video_codec;
    const AVCodec *audio_codec = NULL;
    AVRational fps;
    AVChannelLayout layout;
    int err = 0;
    int global_header = 0;
    VTEncodeResult result = VT_ENCODE_FAILED;

    memset(&cx, 0, sizeof cx);
    memset(&layout, 0, sizeof layout);
    cx.video_index = -1;
    cx.audio_index = -1;
    cx.last_percent = -1;
    cx.cancel = cancel;
    cx.progress = progress;
    cx.progress_user = user;
    cx.error = error;
    cx.error_len = error_len;
    if (error && error_len)
        error[0] = '\0';
    if (!VT_EncodeSupported(profile))
        return fail(&cx, 0, VT_EncodeUnsupportedReason()) == AVERROR_EXIT ? VT_ENCODE_CANCELLED : VT_ENCODE_FAILED;
    if (!source || !output || !output[0]) {
        fail(&cx, 0, "The output path is missing.");
        return VT_ENCODE_FAILED;
    }

    err = VT_FormatOpenInput(&cx.in, &cx.input, source, cancel, error, error_len);
    if (err < 0)
        goto cleanup;
    if ((err = avformat_find_stream_info(cx.in, NULL)) < 0) {
        fail(&cx, err, "Could not read the streams.");
        goto cleanup;
    }
    if (cx.in->duration > 0)
        cx.duration_us = (long long)cx.in->duration;
    cx.video_index = av_find_best_stream(cx.in, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (cx.video_index < 0) {
        if (cx.video_index == AVERROR_DECODER_NOT_FOUND)
            fail(&cx, 0, "Could not find a video decoder.");
        else
            fail(&cx, 0, "No video stream.");
        goto cleanup;
    }
    cx.audio_index = av_find_best_stream(cx.in, AVMEDIA_TYPE_AUDIO, -1, cx.video_index, NULL, 0);
    if (cx.audio_index < 0)
        cx.audio_index = -1;
    if ((err = open_decoder(&cx, &cx.vdec, cx.video_index)) < 0)
        goto cleanup;
    cx.enc_w = cx.vdec->width & ~1;
    cx.enc_h = cx.vdec->height & ~1;
    if (cx.enc_w < 2 || cx.enc_h < 2) {
        fail(&cx, 0, "The picture size is unusable.");
        goto cleanup;
    }

    video_codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!video_codec) {
        fail(&cx, AVERROR_ENCODER_NOT_FOUND, "Could not find the MPEG-4 encoder.");
        goto cleanup;
    }
    if ((err = avformat_alloc_output_context2(&cx.out, NULL, "matroska", NULL)) < 0 || !cx.out) {
        fail(&cx, err, "Could not create the Matroska file.");
        goto cleanup;
    }
    if (cx.out->oformat->flags & AVFMT_GLOBALHEADER)
        global_header = 1;
    cx.venc = avcodec_alloc_context3(video_codec);
    if (!cx.venc) {
        fail(&cx, AVERROR(ENOMEM), "Out of memory.");
        goto cleanup;
    }
    fps = av_guess_frame_rate(cx.in, cx.in->streams[cx.video_index], NULL);
    if (fps.num <= 0 || fps.den <= 0)
        fps = av_make_q(25, 1);
    cx.venc->width = cx.enc_w;
    cx.venc->height = cx.enc_h;
    cx.venc->pix_fmt = AV_PIX_FMT_YUV420P;
    cx.venc->framerate = fps;
    cx.venc->time_base = av_inv_q(fps);
    cx.venc->bit_rate = (int64_t)profile->video_bitrate * 1000;
    cx.venc->gop_size = 15;
    cx.venc->max_b_frames = 0;
    cx.venc->thread_count = 1;
    cx.venc->sample_aspect_ratio = cx.in->streams[cx.video_index]->sample_aspect_ratio;
    if (cx.venc->sample_aspect_ratio.num <= 0 || cx.venc->sample_aspect_ratio.den <= 0)
        cx.venc->sample_aspect_ratio = av_make_q(1, 1);
    if (global_header)
        cx.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((err = avcodec_open2(cx.venc, video_codec, NULL)) < 0) {
        fail(&cx, err, "Could not open the MPEG-4 encoder.");
        goto cleanup;
    }
    cx.vst = add_stream(&cx, cx.venc);
    if (!cx.vst)
        goto cleanup;
    cx.vst->avg_frame_rate = cx.venc->framerate;
    cx.vst->sample_aspect_ratio = cx.venc->sample_aspect_ratio;

    if (cx.audio_index >= 0) {
        if ((err = open_decoder(&cx, &cx.adec, cx.audio_index)) < 0)
            goto cleanup;
        if (cx.adec->sample_rate <= 0 || cx.adec->ch_layout.nb_channels <= 0) {
            fail(&cx, 0, "The audio stream has no rate or channels.");
            goto cleanup;
        }
        audio_codec = avcodec_find_encoder_by_name("libmp3lame");
        if (!audio_codec)
            audio_codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
        if (!audio_codec) {
            fail(&cx, AVERROR_ENCODER_NOT_FOUND, "Could not find the MP3 encoder.");
            goto cleanup;
        }
        cx.aenc = avcodec_alloc_context3(audio_codec);
        if (!cx.aenc) {
            fail(&cx, AVERROR(ENOMEM), "Out of memory.");
            goto cleanup;
        }
        cx.aenc->sample_fmt = pick_sample_fmt(audio_codec);
        cx.aenc->sample_rate = pick_sample_rate(audio_codec, cx.adec->sample_rate);
        cx.aenc->bit_rate = (int64_t)profile->audio_bitrate * 1000;
        cx.aenc->time_base = av_make_q(1, cx.aenc->sample_rate);
        cx.aenc->thread_count = 1;
        av_channel_layout_default(&layout, cx.adec->ch_layout.nb_channels > 1 ? 2 : 1);
        if ((err = av_channel_layout_copy(&cx.aenc->ch_layout, &layout)) < 0) {
            fail(&cx, err, "Out of memory.");
            goto cleanup;
        }
        if (global_header)
            cx.aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((err = avcodec_open2(cx.aenc, audio_codec, NULL)) < 0) {
            fail(&cx, err, "Could not open the MP3 encoder.");
            goto cleanup;
        }
        if ((err = swr_alloc_set_opts2(&cx.swr, &cx.aenc->ch_layout, cx.aenc->sample_fmt,
                                       cx.aenc->sample_rate, &cx.adec->ch_layout,
                                       cx.adec->sample_fmt, cx.adec->sample_rate, 0, NULL)) < 0 ||
            !cx.swr || (err = swr_init(cx.swr)) < 0) {
            fail(&cx, err, "Could not set up audio conversion.");
            goto cleanup;
        }
        cx.fifo = av_audio_fifo_alloc(cx.aenc->sample_fmt, cx.aenc->ch_layout.nb_channels, 1024);
        if (!cx.fifo) {
            fail(&cx, AVERROR(ENOMEM), "Out of memory.");
            goto cleanup;
        }
        cx.ast = add_stream(&cx, cx.aenc);
        if (!cx.ast)
            goto cleanup;
    }

    cx.decoded = av_frame_alloc();
    cx.scaled = av_frame_alloc();
    cx.audio = av_frame_alloc();
    cx.packet = av_packet_alloc();
    cx.encoded = av_packet_alloc();
    if (!cx.decoded || !cx.scaled || !cx.audio || !cx.packet || !cx.encoded) {
        fail(&cx, AVERROR(ENOMEM), "Out of memory.");
        goto cleanup;
    }
    cx.scaled->format = AV_PIX_FMT_YUV420P;
    cx.scaled->width = cx.enc_w;
    cx.scaled->height = cx.enc_h;
    if ((err = av_frame_get_buffer(cx.scaled, 32)) < 0) {
        fail(&cx, err, "Out of memory.");
        goto cleanup;
    }
    if (!VT_FileOpenWrite(&cx.output, output)) {
        fail(&cx, 0, "Could not create the output.");
        goto cleanup;
    }
    cx.output_created = TRUE;
    cx.out->pb = cx.output.pb;
    cx.out->flags |= AVFMT_FLAG_CUSTOM_IO;
    if ((err = avformat_write_header(cx.out, NULL)) < 0) {
        fail(&cx, err, "Could not write the file header.");
        goto cleanup;
    }
    cx.header_ok = TRUE;

    while ((err = av_read_frame(cx.in, cx.packet)) >= 0) {
        int index = cx.packet->stream_index;

        if (stopped(&cx)) {
            av_packet_unref(cx.packet);
            err = AVERROR_EXIT;
            break;
        }
        if (index == cx.video_index)
            err = push_packet(&cx, cx.vdec, cx.packet, handle_video);
        else if (index == cx.audio_index)
            err = push_packet(&cx, cx.adec, cx.packet, handle_audio);
        else
            err = 0;
        av_packet_unref(cx.packet);
        if (err < 0)
            break;
    }
    if (err == AVERROR_EOF)
        err = 0;
    if (err < 0)
        goto cleanup;

    if ((err = push_packet(&cx, cx.vdec, NULL, handle_video)) < 0)
        goto cleanup;
    if (cx.audio_index >= 0) {
        if ((err = push_packet(&cx, cx.adec, NULL, handle_audio)) < 0)
            goto cleanup;
        if ((err = convert_audio(&cx, NULL)) < 0)
            goto cleanup;
        if ((err = drain_audio(&cx, TRUE)) < 0)
            goto cleanup;
    }
    if (cx.video_pts <= 0) {
        fail(&cx, 0, "The file has no video frames.");
        goto cleanup;
    }
    if ((err = write_encoded(&cx, cx.venc, cx.vst, NULL)) < 0)
        goto cleanup;
    if (stopped(&cx)) {
        err = AVERROR_EXIT;
        goto cleanup;
    }
    if ((err = av_write_trailer(cx.out)) < 0) {
        fail(&cx, err, "Could not finish the output.");
        goto cleanup;
    }
    result = VT_ENCODE_OK;
    err = 0;

cleanup:
    av_channel_layout_uninit(&layout);
    if (result != VT_ENCODE_OK && (stopped(&cx) || err == AVERROR_EXIT))
        result = VT_ENCODE_CANCELLED;
    if (result == VT_ENCODE_CANCELLED && error && error_len)
        error[0] = '\0';
    close_encode(&cx, source, output, result);
    return result;
}
