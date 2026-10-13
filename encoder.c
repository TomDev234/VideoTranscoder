/*
 * One encode pipeline for every legal profile: re-encode, stream copy,
 * audio gain, and 2-pass. Cancel is checked between packets. A partial
 * output and the T: stats file are removed unless the trailer is written.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <proto/dos.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
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
    const VTProfile *profile;
    AVFormatContext *in;
    AVFormatContext *out;
    AVCodecContext *vdec;
    AVCodecContext *venc;
    AVCodecContext *adec;
    AVCodecContext *aenc;
    AVFrame *decoded;
    AVFrame *scaled;
    AVFrame *audio;
    AVFrame *filtered;
    AVPacket *packet;
    AVPacket *encoded;
    struct SwsContext *sws;
    struct SwrContext *swr;
    AVAudioFifo *fifo;
    AVFilterGraph *graph;
    AVFilterContext *asrc;
    AVFilterContext *asink;
    AVStream *vst;
    AVStream *ast;
    const volatile LONG *cancel;
    VTEncodeProgressFunc progress;
    APTR progress_user;
    char *error;
    ULONG error_len;
    char *stats_mem;
    char stats_path[VT_PATH_LEN];
    long long duration_us;
    long long video_pts;
    long long audio_pts;
    int video_index;
    int audio_index;
    int video_packets;
    int enc_w;
    int enc_h;
    int sws_w;
    int sws_h;
    int sws_fmt;
    int last_percent;
    int pass_base;
    int pass_span;
    int global_header;
    BOOL video_copy;
    BOOL audio_copy;
    BOOL discard_output;
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

static const char *muxer_name(VTContainer container)
{
    switch (container) {
    case VT_CONTAINER_MP4:
        return "mp4";
    case VT_CONTAINER_MKV:
        return "matroska";
    case VT_CONTAINER_MOV:
        return "mov";
    case VT_CONTAINER_AVI:
        return "avi";
    default:
        return NULL;
    }
}

static const char *video_missing(VTVideoCodec codec)
{
    switch (codec) {
    case VT_VCODEC_MPEG2:
        return "Could not find the MPEG-2 encoder.";
    case VT_VCODEC_MPEG4:
        return "Could not find the MPEG-4 encoder.";
    case VT_VCODEC_H264:
        return "Could not find the H.264 encoder.";
    default:
        return "Could not find the video encoder.";
    }
}

static const char *audio_missing(VTAudioCodec codec)
{
    switch (codec) {
    case VT_ACODEC_MP3:
        return "Could not find the MP3 encoder.";
    case VT_ACODEC_AC3:
        return "Could not find the AC-3 encoder.";
    case VT_ACODEC_AAC:
        return "Could not find the AAC encoder.";
    default:
        return "Could not find the audio encoder.";
    }
}

static BOOL map_video_id(enum AVCodecID id, VTVideoCodec *out)
{
    if (id == AV_CODEC_ID_MPEG2VIDEO)
        *out = VT_VCODEC_MPEG2;
    else if (id == AV_CODEC_ID_MPEG4)
        *out = VT_VCODEC_MPEG4;
    else if (id == AV_CODEC_ID_H264)
        *out = VT_VCODEC_H264;
    else
        return FALSE;
    return TRUE;
}

static BOOL map_audio_id(enum AVCodecID id, VTAudioCodec *out)
{
    if (id == AV_CODEC_ID_MP3)
        *out = VT_ACODEC_MP3;
    else if (id == AV_CODEC_ID_AC3)
        *out = VT_ACODEC_AC3;
    else if (id == AV_CODEC_ID_AAC)
        *out = VT_ACODEC_AAC;
    else
        return FALSE;
    return TRUE;
}

static const AVCodec *find_video_encoder(VTVideoCodec codec)
{
    if (codec == VT_VCODEC_MPEG2)
        return avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (codec == VT_VCODEC_MPEG4)
        return avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (codec == VT_VCODEC_H264) {
        const AVCodec *codec_h264 = avcodec_find_encoder_by_name("libx264");

        if (codec_h264)
            return codec_h264;
        return avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    return NULL;
}

static const AVCodec *find_audio_encoder(VTAudioCodec codec)
{
    if (codec == VT_ACODEC_MP3) {
        const AVCodec *lame = avcodec_find_encoder_by_name("libmp3lame");

        if (lame)
            return lame;
        return avcodec_find_encoder(AV_CODEC_ID_MP3);
    }
    if (codec == VT_ACODEC_AC3)
        return avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (codec == VT_ACODEC_AAC)
        return avcodec_find_encoder(AV_CODEC_ID_AAC);
    return NULL;
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
    int raw = 0;
    int percent;
    long long us;

    if (cx->duration_us > 0 && cx->venc && cx->venc->time_base.den > 0 && !cx->video_copy) {
        us = (long long)av_rescale_q(cx->video_pts, cx->venc->time_base, AV_TIME_BASE_Q);
        if (us < 0)
            us = 0;
        raw = (int)((us * 100) / cx->duration_us);
    } else if (cx->input.size > 0 && cx->input.bytes_read > 0) {
        raw = (int)((cx->input.bytes_read * 100) / cx->input.size);
    }
    if (raw < 0)
        raw = 0;
    if (raw > 100)
        raw = 100;
    percent = cx->pass_base + (raw * cx->pass_span) / 100;
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
        if (!cx->discard_output && stream && cx->out) {
            cx->encoded->stream_index = stream->index;
            av_packet_rescale_ts(cx->encoded, enc->time_base, stream->time_base);
            err = av_interleaved_write_frame(cx->out, cx->encoded);
            if (err < 0) {
                av_packet_unref(cx->encoded);
                return fail(cx, err, "Could not write the output");
            }
        }
        av_packet_unref(cx->encoded);
    }
}

static int write_copy(struct EncodeCtx *cx, int in_index, AVStream *out_stream, BOOL video)
{
    AVStream *in_stream = cx->in->streams[in_index];
    int err;

    av_packet_rescale_ts(cx->packet, in_stream->time_base, out_stream->time_base);
    cx->packet->stream_index = out_stream->index;
    cx->packet->pos = -1;
    err = av_interleaved_write_frame(cx->out, cx->packet);
    if (err < 0)
        return fail(cx, err, "Could not write the output");
    if (video)
        cx->video_packets++;
    report(cx);
    return 0;
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

static int emit_audio(struct EncodeCtx *cx, AVFrame *frame)
{
    int err = convert_audio(cx, frame);

    if (err < 0)
        return err;
    return drain_audio(cx, FALSE);
}

static int pull_gain(struct EncodeCtx *cx)
{
    int err;

    if (!cx->asink)
        return 0;
    for (;;) {
        if (stopped(cx))
            return AVERROR_EXIT;
        err = av_buffersink_get_frame(cx->asink, cx->filtered);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return 0;
        if (err < 0)
            return fail(cx, err, "Could not apply audio gain.");
        err = emit_audio(cx, cx->filtered);
        av_frame_unref(cx->filtered);
        if (err < 0)
            return err;
    }
}

static int handle_audio(AVFrame *frame, void *user)
{
    struct EncodeCtx *cx = (struct EncodeCtx *)user;
    int err;

    if (!cx->asrc)
        return emit_audio(cx, frame);
    err = av_buffersrc_add_frame(cx->asrc, frame);
    if (err < 0)
        return fail(cx, err, "Could not apply audio gain.");
    return pull_gain(cx);
}

static int flush_gain(struct EncodeCtx *cx)
{
    int err;

    if (!cx->asrc)
        return 0;
    err = av_buffersrc_add_frame(cx->asrc, NULL);
    if (err < 0)
        return fail(cx, err, "Could not apply audio gain.");
    return pull_gain(cx);
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

static void make_stats_path(const char *output, char *out, ULONG out_len)
{
    unsigned long hash = 5381;
    const unsigned char *cursor;

    for (cursor = (const unsigned char *)(output ? output : ""); *cursor; cursor++)
        hash = (hash * 33u) + *cursor;
    snprintf(out, out_len, "T:VT%08lx.pass", hash);
}

static BOOL write_stats_file(const char *path, const char *text)
{
    BPTR fh;
    const char *cursor = text;
    long long left;

    left = (long long)strlen(text);
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh)
        return FALSE;
    while (left > 0) {
        LONG chunk = left > 32768 ? 32768 : (LONG)left;
        LONG wrote = Write(fh, (APTR)cursor, chunk);

        if (wrote <= 0) {
            Close(fh);
            return FALSE;
        }
        cursor += wrote;
        left -= wrote;
    }
    Close(fh);
    return TRUE;
}

static char *read_stats_file(const char *path)
{
    BPTR fh;
    long long size;
    long long got_total = 0;
    char *buf;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return NULL;
    if (Seek64(fh, 0, OFFSET_END) < 0) {
        Close(fh);
        return NULL;
    }
    size = (long long)Seek64(fh, 0, OFFSET_BEGINNING);
    if (size <= 0 || size > 8LL * 1024LL * 1024LL) {
        Close(fh);
        return NULL;
    }
    buf = (char *)av_malloc((size_t)size + 1);
    if (!buf) {
        Close(fh);
        return NULL;
    }
    while (got_total < size) {
        LONG chunk = (size - got_total) > 32768 ? 32768 : (LONG)(size - got_total);
        LONG got = Read(fh, buf + got_total, chunk);

        if (got <= 0) {
            av_free(buf);
            Close(fh);
            return NULL;
        }
        got_total += got;
    }
    buf[size] = '\0';
    Close(fh);
    return buf;
}

static void delete_stats(struct EncodeCtx *cx)
{
    char extra[VT_PATH_LEN];

    if (!cx->stats_path[0])
        return;
    VT_DeleteIfExists(cx->stats_path);
    snprintf(extra, sizeof extra, "%s.mbtree", cx->stats_path);
    VT_DeleteIfExists(extra);
    cx->stats_path[0] = '\0';
}

static int capture_stats(struct EncodeCtx *cx)
{
    char *from_encoder = NULL;

    if (cx->venc && cx->venc->stats_out && cx->venc->stats_out[0]) {
        from_encoder = av_strdup(cx->venc->stats_out);
        if (!from_encoder)
            return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    }
    /* Close first. libx264 writes its log on close; MPEG-4 leaves it in memory. */
    avcodec_free_context(&cx->venc);
    if (VT_FileExists(cx->stats_path)) {
        av_freep(&from_encoder);
        cx->stats_mem = read_stats_file(cx->stats_path);
        if (!cx->stats_mem)
            return fail(cx, 0, "Could not read the 2-pass stats.");
        return 0;
    }
    if (!from_encoder)
        return fail(cx, 0, "Two-pass stats are missing.");
    if (!write_stats_file(cx->stats_path, from_encoder)) {
        av_free(from_encoder);
        return fail(cx, 0, "Could not write the 2-pass stats.");
    }
    cx->stats_mem = from_encoder;
    return 0;
}

static int open_video_encoder(struct EncodeCtx *cx, int pass)
{
    const AVCodec *codec;
    AVCodecContext *enc;
    AVRational fps;
    AVDictionary *opts = NULL;
    BOOL h264;
    int err;

    if (pass == 2 && !cx->stats_mem)
        return fail(cx, 0, "Two-pass stats are missing.");
    codec = find_video_encoder(cx->profile->video_codec);
    if (!codec)
        return fail(cx, AVERROR_ENCODER_NOT_FOUND, video_missing(cx->profile->video_codec));
    enc = avcodec_alloc_context3(codec);
    if (!enc)
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    h264 = (BOOL)(cx->profile->video_codec == VT_VCODEC_H264);
    fps = av_guess_frame_rate(cx->in, cx->in->streams[cx->video_index], NULL);
    if (fps.num <= 0 || fps.den <= 0)
        fps = av_make_q(25, 1);
    enc->width = cx->enc_w;
    enc->height = cx->enc_h;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->framerate = fps;
    enc->time_base = av_inv_q(fps);
    enc->bit_rate = (int64_t)cx->profile->video_bitrate * 1000;
    enc->gop_size = h264 ? 250 : 15;
    enc->max_b_frames = 0;
    enc->thread_count = 1;
    enc->sample_aspect_ratio = cx->in->streams[cx->video_index]->sample_aspect_ratio;
    if (enc->sample_aspect_ratio.num <= 0 || enc->sample_aspect_ratio.den <= 0)
        enc->sample_aspect_ratio = av_make_q(1, 1);
    if (pass == 1)
        enc->flags |= AV_CODEC_FLAG_PASS1;
    if (pass == 2) {
        enc->flags |= AV_CODEC_FLAG_PASS2;
        enc->stats_in = cx->stats_mem;
    }
    if (cx->global_header)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (h264) {
        av_dict_set(&opts, "preset", "fast", 0);
        av_dict_set(&opts, "mbtree", "0", 0);
        av_dict_set(&opts, "threads", "1", 0);
    }
    if ((pass == 1 || pass == 2) && cx->stats_path[0])
        av_dict_set(&opts, "stats", cx->stats_path, 0);
    err = avcodec_open2(enc, codec, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        avcodec_free_context(&enc);
        return fail(cx, err, "Could not open the video encoder.");
    }
    cx->venc = enc;
    return 0;
}

static AVStream *add_encoded_stream(struct EncodeCtx *cx, AVCodecContext *enc)
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

static AVStream *add_copy_stream(struct EncodeCtx *cx, int in_index)
{
    AVStream *in_stream = cx->in->streams[in_index];
    AVStream *stream;
    int err;

    stream = avformat_new_stream(cx->out, NULL);
    if (!stream) {
        fail(cx, AVERROR(ENOMEM), "Out of memory.");
        return NULL;
    }
    if ((err = avcodec_parameters_copy(stream->codecpar, in_stream->codecpar)) < 0) {
        fail(cx, err, "Could not copy the stream.");
        return NULL;
    }
    stream->codecpar->codec_tag = 0;
    stream->time_base = in_stream->time_base;
    stream->avg_frame_rate = in_stream->avg_frame_rate;
    stream->sample_aspect_ratio = in_stream->sample_aspect_ratio;
    return stream;
}

static int open_gain(struct EncodeCtx *cx)
{
    AVFilterGraph *graph;
    const AVFilter *src_filter;
    const AVFilter *vol_filter;
    const AVFilter *sink_filter;
    AVFilterContext *src = NULL;
    AVFilterContext *vol = NULL;
    AVFilterContext *sink = NULL;
    char layout_name[128];
    char args[512];
    char volume[64];
    const char *sample_name;
    int err;

    if (cx->audio_copy || !cx->profile || cx->profile->audio_gain_db == 0)
        return 0;
    graph = avfilter_graph_alloc();
    src_filter = avfilter_get_by_name("abuffer");
    vol_filter = avfilter_get_by_name("volume");
    sink_filter = avfilter_get_by_name("abuffersink");
    sample_name = av_get_sample_fmt_name(cx->adec->sample_fmt);
    if (!graph || !src_filter || !vol_filter || !sink_filter || !sample_name) {
        avfilter_graph_free(&graph);
        return fail(cx, 0, "Could not set up audio gain.");
    }
    if (av_channel_layout_describe(&cx->adec->ch_layout, layout_name, sizeof layout_name) < 0) {
        avfilter_graph_free(&graph);
        return fail(cx, 0, "Could not set up audio gain.");
    }
    snprintf(args, sizeof args,
             "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             cx->adec->sample_rate, cx->adec->sample_rate, sample_name, layout_name);
    snprintf(volume, sizeof volume, "volume=%lddB", (long)cx->profile->audio_gain_db);
    err = avfilter_graph_create_filter(&src, src_filter, "in", args, NULL, graph);
    if (err >= 0)
        err = avfilter_graph_create_filter(&vol, vol_filter, "gain", volume, NULL, graph);
    if (err >= 0)
        err = avfilter_graph_create_filter(&sink, sink_filter, "out", NULL, NULL, graph);
    if (err >= 0)
        err = avfilter_link(src, 0, vol, 0);
    if (err >= 0)
        err = avfilter_link(vol, 0, sink, 0);
    if (err >= 0)
        err = avfilter_graph_config(graph, NULL);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return fail(cx, err, "Could not set up audio gain.");
    }
    cx->filtered = av_frame_alloc();
    if (!cx->filtered) {
        avfilter_graph_free(&graph);
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    }
    cx->graph = graph;
    cx->asrc = src;
    cx->asink = sink;
    return 0;
}

static int open_audio(struct EncodeCtx *cx)
{
    const AVCodec *codec;
    AVChannelLayout layout;
    int err = 0;

    memset(&layout, 0, sizeof layout);
    if ((err = open_decoder(cx, &cx->adec, cx->audio_index)) < 0)
        return err;
    if (cx->adec->sample_rate <= 0 || cx->adec->ch_layout.nb_channels <= 0)
        return fail(cx, 0, "The audio stream has no rate or channels.");
    codec = find_audio_encoder(cx->profile->audio_codec);
    if (!codec)
        return fail(cx, AVERROR_ENCODER_NOT_FOUND, audio_missing(cx->profile->audio_codec));
    cx->aenc = avcodec_alloc_context3(codec);
    if (!cx->aenc)
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    cx->aenc->sample_fmt = pick_sample_fmt(codec);
    cx->aenc->sample_rate = pick_sample_rate(codec, cx->adec->sample_rate);
    cx->aenc->bit_rate = (int64_t)cx->profile->audio_bitrate * 1000;
    cx->aenc->time_base = av_make_q(1, cx->aenc->sample_rate);
    cx->aenc->thread_count = 1;
    av_channel_layout_default(&layout, cx->adec->ch_layout.nb_channels > 1 ? 2 : 1);
    if ((err = av_channel_layout_copy(&cx->aenc->ch_layout, &layout)) < 0) {
        av_channel_layout_uninit(&layout);
        return fail(cx, err, "Out of memory.");
    }
    if (cx->global_header)
        cx->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((err = avcodec_open2(cx->aenc, codec, NULL)) < 0) {
        av_channel_layout_uninit(&layout);
        return fail(cx, err, "Could not open the audio encoder.");
    }
    if ((err = swr_alloc_set_opts2(&cx->swr, &cx->aenc->ch_layout, cx->aenc->sample_fmt,
                                   cx->aenc->sample_rate, &cx->adec->ch_layout,
                                   cx->adec->sample_fmt, cx->adec->sample_rate, 0, NULL)) < 0 ||
        !cx->swr || (err = swr_init(cx->swr)) < 0) {
        av_channel_layout_uninit(&layout);
        return fail(cx, err, "Could not set up audio conversion.");
    }
    cx->fifo = av_audio_fifo_alloc(cx->aenc->sample_fmt, cx->aenc->ch_layout.nb_channels, 1024);
    if (!cx->fifo) {
        av_channel_layout_uninit(&layout);
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    }
    err = open_gain(cx);
    av_channel_layout_uninit(&layout);
    return err;
}

static int alloc_frames(struct EncodeCtx *cx)
{
    cx->decoded = av_frame_alloc();
    cx->audio = av_frame_alloc();
    cx->packet = av_packet_alloc();
    cx->encoded = av_packet_alloc();
    if (!cx->decoded || !cx->audio || !cx->packet || !cx->encoded)
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    if (cx->video_copy)
        return 0;
    cx->scaled = av_frame_alloc();
    if (!cx->scaled)
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    cx->scaled->format = AV_PIX_FMT_YUV420P;
    cx->scaled->width = cx->enc_w;
    cx->scaled->height = cx->enc_h;
    if (av_frame_get_buffer(cx->scaled, 32) < 0)
        return fail(cx, AVERROR(ENOMEM), "Out of memory.");
    return 0;
}

static int read_packets(struct EncodeCtx *cx, BOOL video_only)
{
    int err;

    while ((err = av_read_frame(cx->in, cx->packet)) >= 0) {
        int index = cx->packet->stream_index;

        if (stopped(cx)) {
            av_packet_unref(cx->packet);
            return AVERROR_EXIT;
        }
        if (index == cx->video_index) {
            if (cx->video_copy)
                err = cx->discard_output ? 0 : write_copy(cx, index, cx->vst, TRUE);
            else
                err = push_packet(cx, cx->vdec, cx->packet, handle_video);
        } else if (!video_only && index == cx->audio_index) {
            if (cx->audio_copy)
                err = write_copy(cx, index, cx->ast, FALSE);
            else if (cx->adec)
                err = push_packet(cx, cx->adec, cx->packet, handle_audio);
            else
                err = 0;
        } else {
            err = 0;
        }
        av_packet_unref(cx->packet);
        if (err < 0)
            return err;
    }
    if (err == AVERROR_EOF)
        return 0;
    return err < 0 ? fail(cx, err, "Could not read the source.") : 0;
}

static int finish_streams(struct EncodeCtx *cx, BOOL video_only)
{
    int err;

    if (!cx->video_copy && cx->vdec) {
        if ((err = push_packet(cx, cx->vdec, NULL, handle_video)) < 0)
            return err;
        if ((err = write_encoded(cx, cx->venc, cx->vst, NULL)) < 0)
            return err;
    }
    if (video_only || cx->audio_copy || !cx->adec)
        return 0;
    if ((err = push_packet(cx, cx->adec, NULL, handle_audio)) < 0)
        return err;
    if ((err = flush_gain(cx)) < 0)
        return err;
    if ((err = convert_audio(cx, NULL)) < 0)
        return err;
    return drain_audio(cx, TRUE);
}

static int rewind_input(struct EncodeCtx *cx)
{
    int err = av_seek_frame(cx->in, -1, 0, AVSEEK_FLAG_BACKWARD);

    if (err < 0)
        return fail(cx, err, "Could not rewind the source for pass 2.");
    if (cx->vdec)
        avcodec_flush_buffers(cx->vdec);
    return 0;
}

static void close_encode(struct EncodeCtx *cx, const char *source, const char *output, VTEncodeResult result)
{
    if (cx->fifo)
        av_audio_fifo_free(cx->fifo);
    if (cx->swr)
        swr_free(&cx->swr);
    if (cx->sws)
        sws_freeContext(cx->sws);
    if (cx->graph)
        avfilter_graph_free(&cx->graph);
    cx->asrc = NULL;
    cx->asink = NULL;
    av_frame_free(&cx->decoded);
    av_frame_free(&cx->scaled);
    av_frame_free(&cx->audio);
    av_frame_free(&cx->filtered);
    av_packet_free(&cx->packet);
    av_packet_free(&cx->encoded);
    avcodec_free_context(&cx->vdec);
    avcodec_free_context(&cx->venc);
    avcodec_free_context(&cx->adec);
    avcodec_free_context(&cx->aenc);
    av_freep(&cx->stats_mem);
    delete_stats(cx);
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
    return (BOOL)(profile && VT_ProfileValidate(profile));
}

const char *VT_EncodeUnsupportedReason(void)
{
    return "The profile is not a legal container and codec pair.";
}

VTEncodeResult VT_EncodeFile(const char *source, const char *output, const VTProfile *profile,
                             const volatile LONG *cancel, VTEncodeProgressFunc progress, APTR user,
                             char *error, ULONG error_len)
{
    struct EncodeCtx cx;
    const AVOutputFormat *oformat;
    const char *muxer;
    VTVideoCodec copied_video;
    VTAudioCodec copied_audio;
    int err = 0;
    int video_pass = 0;
    BOOL two_pass;
    VTEncodeResult result = VT_ENCODE_FAILED;

    memset(&cx, 0, sizeof cx);
    cx.video_index = -1;
    cx.audio_index = -1;
    cx.last_percent = -1;
    cx.pass_span = 100;
    cx.cancel = cancel;
    cx.progress = progress;
    cx.progress_user = user;
    cx.error = error;
    cx.error_len = error_len;
    cx.profile = profile;
    if (error && error_len)
        error[0] = '\0';
    if (!VT_EncodeSupported(profile))
        return fail(&cx, 0, VT_EncodeUnsupportedReason()) < 0 && stopped(&cx) ? VT_ENCODE_CANCELLED : VT_ENCODE_FAILED;
    if (!source || !output || !output[0]) {
        fail(&cx, 0, "The output path is missing.");
        return VT_ENCODE_FAILED;
    }

    cx.video_copy = (BOOL)(profile->video_codec == VT_VCODEC_COPY);
    cx.audio_copy = (BOOL)(profile->audio_codec == VT_ACODEC_COPY);
    two_pass = (BOOL)(profile->two_pass && !cx.video_copy);
    muxer = muxer_name(profile->container);
    oformat = muxer ? av_guess_format(muxer, NULL, NULL) : NULL;
    if (!oformat) {
        fail(&cx, 0, "Could not create the output file.");
        return VT_ENCODE_FAILED;
    }
    cx.global_header = (oformat->flags & AVFMT_GLOBALHEADER) ? 1 : 0;

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

    if (cx.video_copy) {
        if (!map_video_id(cx.in->streams[cx.video_index]->codecpar->codec_id, &copied_video) ||
            !VT_CompatVideo(profile->container, copied_video)) {
            fail(&cx, 0, "This container cannot store the copied video.");
            goto cleanup;
        }
    } else {
        if ((err = open_decoder(&cx, &cx.vdec, cx.video_index)) < 0)
            goto cleanup;
        cx.enc_w = cx.vdec->width & ~1;
        cx.enc_h = cx.vdec->height & ~1;
        if (cx.enc_w < 2 || cx.enc_h < 2) {
            fail(&cx, 0, "The picture size is unusable.");
            goto cleanup;
        }
    }
    if (cx.audio_copy && cx.audio_index >= 0) {
        if (!map_audio_id(cx.in->streams[cx.audio_index]->codecpar->codec_id, &copied_audio) ||
            !VT_CompatAudio(profile->container, copied_audio)) {
            fail(&cx, 0, "This container cannot store the copied audio.");
            goto cleanup;
        }
    }
    if ((err = alloc_frames(&cx)) < 0)
        goto cleanup;

    if (two_pass) {
        make_stats_path(output, cx.stats_path, sizeof cx.stats_path);
        cx.discard_output = TRUE;
        cx.pass_base = 0;
        cx.pass_span = 50;
        if ((err = open_video_encoder(&cx, 1)) < 0)
            goto cleanup;
        if ((err = read_packets(&cx, TRUE)) < 0)
            goto cleanup;
        if ((err = finish_streams(&cx, TRUE)) < 0)
            goto cleanup;
        if (cx.video_pts <= 0) {
            fail(&cx, 0, "The file has no video frames.");
            goto cleanup;
        }
        if ((err = capture_stats(&cx)) < 0)
            goto cleanup;
        cx.discard_output = FALSE;
        cx.video_pts = 0;
        cx.video_packets = 0;
        cx.pass_base = 50;
        cx.pass_span = 50;
        cx.last_percent = -1;
        if ((err = rewind_input(&cx)) < 0)
            goto cleanup;
        video_pass = 2;
    }

    if ((err = avformat_alloc_output_context2(&cx.out, oformat, NULL, NULL)) < 0 || !cx.out) {
        fail(&cx, err, "Could not create the output file.");
        goto cleanup;
    }
    cx.out->flags |= AVFMT_FLAG_AUTO_BSF;
    if (!cx.video_copy) {
        if ((err = open_video_encoder(&cx, video_pass)) < 0)
            goto cleanup;
        cx.vst = add_encoded_stream(&cx, cx.venc);
        if (!cx.vst)
            goto cleanup;
        cx.vst->avg_frame_rate = cx.venc->framerate;
        cx.vst->sample_aspect_ratio = cx.venc->sample_aspect_ratio;
    } else {
        cx.vst = add_copy_stream(&cx, cx.video_index);
        if (!cx.vst)
            goto cleanup;
    }

    if (cx.audio_index >= 0 && cx.audio_copy) {
        cx.ast = add_copy_stream(&cx, cx.audio_index);
        if (!cx.ast)
            goto cleanup;
    } else if (cx.audio_index >= 0) {
        if ((err = open_audio(&cx)) < 0)
            goto cleanup;
        cx.ast = add_encoded_stream(&cx, cx.aenc);
        if (!cx.ast)
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

    if ((err = read_packets(&cx, FALSE)) < 0)
        goto cleanup;
    if ((err = finish_streams(&cx, FALSE)) < 0)
        goto cleanup;
    if (cx.video_copy) {
        if (cx.video_packets <= 0) {
            fail(&cx, 0, "The file has no video frames.");
            goto cleanup;
        }
    } else if (cx.video_pts <= 0) {
        fail(&cx, 0, "The file has no video frames.");
        goto cleanup;
    }
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
    if (result != VT_ENCODE_OK && (stopped(&cx) || err == AVERROR_EXIT))
        result = VT_ENCODE_CANCELLED;
    if (result == VT_ENCODE_CANCELLED && error && error_len)
        error[0] = '\0';
    close_encode(&cx, source, output, result);
    return result;
}
