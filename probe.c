#include <string.h>

#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>

#include "probe.h"
#include "vt_avio.h"
#include "vt_util.h"

static void copy_codec(AVFormatContext *fmt, enum AVMediaType type, char *name, ULONG name_len)
{
    unsigned int index;

    if (!name || name_len == 0)
        return;
    name[0] = '\0';
    if (!fmt)
        return;
    for (index = 0; index < fmt->nb_streams; index++) {
        AVCodecParameters *par = fmt->streams[index]->codecpar;

        if (!par || par->codec_type != type)
            continue;
        VT_StrCopyTrunc(name, name_len, avcodec_get_name(par->codec_id));
        return;
    }
}

VTProbeResult VT_ProbeFile(const char *path, VTProbeInfo *info,
                           const volatile LONG *cancel, char *error, ULONG error_len)
{
    VTFile file;
    AVFormatContext *fmt = NULL;
    int err;

    if (info)
        memset(info, 0, sizeof *info);
    if (!path || !info) {
        VT_AVError(error, error_len, 0, "Could not probe the source.");
        return VT_PROBE_FAILED;
    }
    memset(&file, 0, sizeof file);
    err = VT_FormatOpenInput(&fmt, &file, path, cancel, error, error_len);
    if (err < 0) {
        if (err == AVERROR_EXIT || (cancel && *cancel))
            return VT_PROBE_CANCELLED;
        return VT_PROBE_FAILED;
    }
    err = avformat_find_stream_info(fmt, NULL);
    if (err < 0) {
        VT_FormatCloseInput(&fmt, &file);
        if (cancel && *cancel)
            return VT_PROBE_CANCELLED;
        VT_AVError(error, error_len, err, "Could not read the streams.");
        return VT_PROBE_FAILED;
    }
    if (fmt->duration > 0)
        info->duration_us = (long long)fmt->duration;
    if (file.size > 0)
        info->size_bytes = (unsigned long long)file.size;
    copy_codec(fmt, AVMEDIA_TYPE_VIDEO, info->video_name, sizeof info->video_name);
    copy_codec(fmt, AVMEDIA_TYPE_AUDIO, info->audio_name, sizeof info->audio_name);
    VT_FormatCloseInput(&fmt, &file);
    if (cancel && *cancel)
        return VT_PROBE_CANCELLED;
    return VT_PROBE_OK;
}
