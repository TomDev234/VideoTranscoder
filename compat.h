/*
 * Container and codec matrix. Views ask this before enabling a gadget.
 * The encoder asks the same functions before it starts a job.
 */
#ifndef VT_COMPAT_H
#define VT_COMPAT_H

#include "vt_types.h"

typedef enum {
    VT_CONTAINER_MP4 = 0,
    VT_CONTAINER_MKV = 1,
    VT_CONTAINER_MOV = 2,
    VT_CONTAINER_AVI = 3,
    VT_CONTAINER_COUNT = 4
} VTContainer;

typedef enum {
    VT_VCODEC_MPEG2 = 0,
    VT_VCODEC_MPEG4 = 1,
    VT_VCODEC_H264 = 2,
    VT_VCODEC_COPY = 3,
    VT_VCODEC_COUNT = 4
} VTVideoCodec;

typedef enum {
    VT_ACODEC_MP3 = 0,
    VT_ACODEC_AC3 = 1,
    VT_ACODEC_AAC = 2,
    VT_ACODEC_COPY = 3,
    VT_ACODEC_COUNT = 4
} VTAudioCodec;

const char *VT_ContainerToken(VTContainer container);
const char *VT_ContainerLabel(VTContainer container);
const char *VT_ContainerExtension(VTContainer container);
BOOL VT_ContainerFromToken(const char *text, VTContainer *out);

const char *VT_VideoCodecToken(VTVideoCodec codec);
const char *VT_VideoCodecLabel(VTVideoCodec codec);
BOOL VT_VideoCodecFromToken(const char *text, VTVideoCodec *out);

const char *VT_AudioCodecToken(VTAudioCodec codec);
const char *VT_AudioCodecLabel(VTAudioCodec codec);
BOOL VT_AudioCodecFromToken(const char *text, VTAudioCodec *out);

BOOL VT_CompatVideo(VTContainer container, VTVideoCodec video);
BOOL VT_CompatAudio(VTContainer container, VTAudioCodec audio);
BOOL VT_CompatPair(VTContainer container, VTVideoCodec video, VTAudioCodec audio);

#endif
