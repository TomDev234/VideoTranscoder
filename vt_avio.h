/*
 * DOS-backed AVIO. Amiga paths contain a colon, so libavformat must not
 * treat the volume name as a URL scheme. Probe and the encoder share this.
 */
#ifndef VT_AVIO_H
#define VT_AVIO_H

#include <dos/dos.h>
#include <libavformat/avformat.h>

#include "vt_types.h"

typedef struct VTFile {
    BPTR fh;
    AVIOContext *pb;
    long long size;
    long long bytes_read;
    BOOL writing;
} VTFile;

void VT_AVError(char *dst, ULONG dst_len, int err, const char *what);
void VT_FileProbeURL(const char *path, char *out, ULONG out_len);

BOOL VT_FileOpenRead(VTFile *file, const char *path);
BOOL VT_FileOpenWrite(VTFile *file, const char *path);
void VT_FileClose(VTFile *file);

/* On failure *fmt is NULL and file is closed. cancel may be NULL. */
int VT_FormatOpenInput(AVFormatContext **fmt, VTFile *file, const char *path,
                       const volatile LONG *cancel, char *error, ULONG error_len);
void VT_FormatCloseInput(AVFormatContext **fmt, VTFile *file);

#endif
