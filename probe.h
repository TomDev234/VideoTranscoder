/*
 * Read duration, codecs and size. No MUI and no job mutation: the
 * background task sends the result to the main task.
 */
#ifndef VT_PROBE_H
#define VT_PROBE_H

#include "vt_types.h"

typedef enum {
    VT_PROBE_OK = 0,
    VT_PROBE_FAILED = 1,
    VT_PROBE_CANCELLED = 2
} VTProbeResult;

typedef struct VTProbeInfo {
    long long duration_us;
    unsigned long long size_bytes;
    char video_name[VT_CODEC_LABEL_LEN];
    char audio_name[VT_CODEC_LABEL_LEN];
} VTProbeInfo;

VTProbeResult VT_ProbeFile(const char *path, VTProbeInfo *info,
                           const volatile LONG *cancel,
                           char *error, ULONG error_len);

#endif
