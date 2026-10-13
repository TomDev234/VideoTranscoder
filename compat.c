#include "compat.h"
#include "vt_util.h"

/*
 * Unlisted pairs stay zero, which the UI treats as disabled.
 * Copy is allowed in every container; the source stream is checked later.
 *
 * MKV: every MVP codec.
 * MP4 and MOV: MPEG-4, H.264, AAC, MP3.
 * AVI: MPEG-2, MPEG-4, MP3, AC-3.
 */
static const unsigned char video_ok[VT_CONTAINER_COUNT][VT_VCODEC_COUNT] = {
    [VT_CONTAINER_MP4] = {
        [VT_VCODEC_MPEG4] = 1,
        [VT_VCODEC_H264] = 1,
        [VT_VCODEC_COPY] = 1
    },
    [VT_CONTAINER_MKV] = {
        [VT_VCODEC_MPEG2] = 1,
        [VT_VCODEC_MPEG4] = 1,
        [VT_VCODEC_H264] = 1,
        [VT_VCODEC_COPY] = 1
    },
    [VT_CONTAINER_MOV] = {
        [VT_VCODEC_MPEG4] = 1,
        [VT_VCODEC_H264] = 1,
        [VT_VCODEC_COPY] = 1
    },
    [VT_CONTAINER_AVI] = {
        [VT_VCODEC_MPEG2] = 1,
        [VT_VCODEC_MPEG4] = 1,
        [VT_VCODEC_COPY] = 1
    }
};

static const unsigned char audio_ok[VT_CONTAINER_COUNT][VT_ACODEC_COUNT] = {
    [VT_CONTAINER_MP4] = {
        [VT_ACODEC_MP3] = 1,
        [VT_ACODEC_AAC] = 1,
        [VT_ACODEC_COPY] = 1
    },
    [VT_CONTAINER_MKV] = {
        [VT_ACODEC_MP3] = 1,
        [VT_ACODEC_AC3] = 1,
        [VT_ACODEC_AAC] = 1,
        [VT_ACODEC_COPY] = 1
    },
    [VT_CONTAINER_MOV] = {
        [VT_ACODEC_MP3] = 1,
        [VT_ACODEC_AAC] = 1,
        [VT_ACODEC_COPY] = 1
    },
    [VT_CONTAINER_AVI] = {
        [VT_ACODEC_MP3] = 1,
        [VT_ACODEC_AC3] = 1,
        [VT_ACODEC_COPY] = 1
    }
};

static const char *const container_token[VT_CONTAINER_COUNT] = {
    [VT_CONTAINER_MP4] = "mp4",
    [VT_CONTAINER_MKV] = "mkv",
    [VT_CONTAINER_MOV] = "mov",
    [VT_CONTAINER_AVI] = "avi"
};

static const char *const container_label[VT_CONTAINER_COUNT] = {
    [VT_CONTAINER_MP4] = "MP4",
    [VT_CONTAINER_MKV] = "MKV",
    [VT_CONTAINER_MOV] = "MOV",
    [VT_CONTAINER_AVI] = "AVI"
};

static const char *const container_ext[VT_CONTAINER_COUNT] = {
    [VT_CONTAINER_MP4] = ".mp4",
    [VT_CONTAINER_MKV] = ".mkv",
    [VT_CONTAINER_MOV] = ".mov",
    [VT_CONTAINER_AVI] = ".avi"
};

static const char *const video_token[VT_VCODEC_COUNT] = {
    [VT_VCODEC_MPEG2] = "mpeg2",
    [VT_VCODEC_MPEG4] = "mpeg4",
    [VT_VCODEC_H264] = "h264",
    [VT_VCODEC_COPY] = "copy"
};

static const char *const video_label[VT_VCODEC_COUNT] = {
    [VT_VCODEC_MPEG2] = "MPEG-2",
    [VT_VCODEC_MPEG4] = "MPEG-4",
    [VT_VCODEC_H264] = "H.264",
    [VT_VCODEC_COPY] = "Copy"
};

static const char *const audio_token[VT_ACODEC_COUNT] = {
    [VT_ACODEC_MP3] = "mp3",
    [VT_ACODEC_AC3] = "ac3",
    [VT_ACODEC_AAC] = "aac",
    [VT_ACODEC_COPY] = "copy"
};

static const char *const audio_label[VT_ACODEC_COUNT] = {
    [VT_ACODEC_MP3] = "MP3",
    [VT_ACODEC_AC3] = "AC-3",
    [VT_ACODEC_AAC] = "AAC",
    [VT_ACODEC_COPY] = "Copy"
};

static const char *label_at(const char *const *table, ULONG count, ULONG index)
{
    if (index >= count)
        return NULL;
    return table[index];
}

static BOOL from_token(const char *const *table, ULONG count, const char *text, ULONG *out)
{
    ULONG index;

    if (!text || !text[0] || !out)
        return FALSE;
    for (index = 0; index < count; index++) {
        if (table[index] && VT_StrEqI(table[index], text)) {
            *out = index;
            return TRUE;
        }
    }
    return FALSE;
}

const char *VT_ContainerToken(VTContainer container)
{
    return label_at(container_token, VT_CONTAINER_COUNT, (ULONG)container);
}

const char *VT_ContainerLabel(VTContainer container)
{
    return label_at(container_label, VT_CONTAINER_COUNT, (ULONG)container);
}

const char *VT_ContainerExtension(VTContainer container)
{
    return label_at(container_ext, VT_CONTAINER_COUNT, (ULONG)container);
}

BOOL VT_ContainerFromToken(const char *text, VTContainer *out)
{
    ULONG index;

    if (!out)
        return FALSE;
    if (!from_token(container_token, VT_CONTAINER_COUNT, text, &index))
        return FALSE;
    *out = (VTContainer)index;
    return TRUE;
}

const char *VT_VideoCodecToken(VTVideoCodec codec)
{
    return label_at(video_token, VT_VCODEC_COUNT, (ULONG)codec);
}

const char *VT_VideoCodecLabel(VTVideoCodec codec)
{
    return label_at(video_label, VT_VCODEC_COUNT, (ULONG)codec);
}

BOOL VT_VideoCodecFromToken(const char *text, VTVideoCodec *out)
{
    ULONG index;

    if (!out)
        return FALSE;
    if (!from_token(video_token, VT_VCODEC_COUNT, text, &index))
        return FALSE;
    *out = (VTVideoCodec)index;
    return TRUE;
}

const char *VT_AudioCodecToken(VTAudioCodec codec)
{
    return label_at(audio_token, VT_ACODEC_COUNT, (ULONG)codec);
}

const char *VT_AudioCodecLabel(VTAudioCodec codec)
{
    return label_at(audio_label, VT_ACODEC_COUNT, (ULONG)codec);
}

BOOL VT_AudioCodecFromToken(const char *text, VTAudioCodec *out)
{
    ULONG index;

    if (!out)
        return FALSE;
    if (!from_token(audio_token, VT_ACODEC_COUNT, text, &index))
        return FALSE;
    *out = (VTAudioCodec)index;
    return TRUE;
}

BOOL VT_CompatVideo(VTContainer container, VTVideoCodec video)
{
    if ((ULONG)container >= VT_CONTAINER_COUNT)
        return FALSE;
    if ((ULONG)video >= VT_VCODEC_COUNT)
        return FALSE;
    return video_ok[container][video] ? TRUE : FALSE;
}

BOOL VT_CompatAudio(VTContainer container, VTAudioCodec audio)
{
    if ((ULONG)container >= VT_CONTAINER_COUNT)
        return FALSE;
    if ((ULONG)audio >= VT_ACODEC_COUNT)
        return FALSE;
    return audio_ok[container][audio] ? TRUE : FALSE;
}

BOOL VT_CompatPair(VTContainer container, VTVideoCodec video, VTAudioCodec audio)
{
    if (!VT_CompatVideo(container, video))
        return FALSE;
    return VT_CompatAudio(container, audio);
}
