/*
 * Read and write through dos.library so volume:paths stay files.
 * Seek64 is the MorphOS 64-bit seek; classic Seek is limited to 2GB.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "vt_avio.h"
#include "vt_util.h"

#define VT_AVIO_BUFFER 32768

void VT_AVError(char *dst, ULONG dst_len, int err, const char *what)
{
    char detail[128];
    char combined[VT_ERROR_LEN];

    if (!dst || dst_len == 0)
        return;
    detail[0] = '\0';
    if (err < 0)
        av_strerror(err, detail, sizeof detail);
    if (what && what[0] && detail[0])
        snprintf(combined, sizeof combined, "%s: %s", what, detail);
    else if (what && what[0])
        snprintf(combined, sizeof combined, "%s", what);
    else if (detail[0])
        snprintf(combined, sizeof combined, "%s", detail);
    else
        snprintf(combined, sizeof combined, "%s", "Encode failed.");
    VT_StrCopyTrunc(dst, dst_len, combined);
}

void VT_FileProbeURL(const char *path, char *out, ULONG out_len)
{
    const char *base = path ? path : "";
    const char *scan;
    char name[VT_PATH_LEN];

    for (scan = base; *scan; scan++) {
        if (*scan == '/' || *scan == ':')
            base = scan + 1;
    }
    if (!base[0])
        base = "video";
    snprintf(name, sizeof name, "file:%s", base);
    VT_StrCopyTrunc(out, out_len, name);
}

static int vt_interrupt(void *opaque)
{
    const volatile LONG *cancel = (const volatile LONG *)opaque;

    if (cancel && *cancel)
        return 1;
    return 0;
}

static long long measure_size(BPTR fh)
{
    QUAD size;

    if (Seek64(fh, 0, OFFSET_END) < 0)
        return -1;
    size = Seek64(fh, 0, OFFSET_BEGINNING);
    if (size < 0)
        return -1;
    return (long long)size;
}

static int vt_read(void *opaque, uint8_t *buf, int buf_size)
{
    VTFile *file = (VTFile *)opaque;
    LONG ask;
    LONG got;

    if (!file || !file->fh || buf_size <= 0)
        return AVERROR(EINVAL);
    ask = buf_size > VT_AVIO_BUFFER ? VT_AVIO_BUFFER : buf_size;
    got = Read(file->fh, buf, ask);
    if (got < 0)
        return AVERROR(EIO);
    if (got == 0)
        return AVERROR_EOF;
    file->bytes_read += got;
    return (int)got;
}

static int vt_write(void *opaque, const uint8_t *buf, int buf_size)
{
    VTFile *file = (VTFile *)opaque;
    int left = buf_size;

    if (!file || !file->fh || buf_size < 0)
        return AVERROR(EINVAL);
    while (left > 0) {
        LONG ask = left > VT_AVIO_BUFFER ? VT_AVIO_BUFFER : left;
        LONG wrote = Write(file->fh, (APTR)buf, ask);

        if (wrote <= 0)
            return AVERROR(EIO);
        buf += wrote;
        left -= (int)wrote;
    }
    return buf_size;
}

static int64_t vt_seek(void *opaque, int64_t offset, int whence)
{
    VTFile *file = (VTFile *)opaque;
    LONG mode;
    QUAD pos;

    if (!file || !file->fh)
        return AVERROR(EINVAL);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE)
        return file->size >= 0 ? file->size : AVERROR(EIO);
    if (whence == SEEK_SET)
        mode = OFFSET_BEGINNING;
    else if (whence == SEEK_CUR)
        mode = OFFSET_CURRENT;
    else if (whence == SEEK_END)
        mode = OFFSET_END;
    else
        return AVERROR(EINVAL);
    if (Seek64(file->fh, (QUAD)offset, mode) < 0)
        return AVERROR(EIO);
    pos = Seek64(file->fh, 0, OFFSET_CURRENT);
    if (pos < 0)
        return AVERROR(EIO);
    return (int64_t)pos;
}

static BOOL open_file(VTFile *file, const char *path, LONG mode, BOOL writing)
{
    unsigned char *buffer;
    BPTR fh;

    if (!file || !path || !path[0])
        return FALSE;
    memset(file, 0, sizeof *file);
    fh = Open((STRPTR)path, mode);
    if (!fh)
        return FALSE;
    file->fh = fh;
    file->writing = writing;
    file->size = measure_size(fh);
    if (file->size < 0) {
        Close(fh);
        memset(file, 0, sizeof *file);
        return FALSE;
    }
    buffer = (unsigned char *)av_malloc(VT_AVIO_BUFFER);
    if (!buffer) {
        Close(fh);
        memset(file, 0, sizeof *file);
        return FALSE;
    }
    file->pb = avio_alloc_context(buffer, VT_AVIO_BUFFER, writing ? 1 : 0, file,
                                  writing ? NULL : vt_read,
                                  writing ? vt_write : NULL,
                                  vt_seek);
    if (!file->pb) {
        av_free(buffer);
        Close(fh);
        memset(file, 0, sizeof *file);
        return FALSE;
    }
    file->pb->seekable = AVIO_SEEKABLE_NORMAL;
    return TRUE;
}

BOOL VT_FileOpenRead(VTFile *file, const char *path)
{
    return open_file(file, path, MODE_OLDFILE, FALSE);
}

BOOL VT_FileOpenWrite(VTFile *file, const char *path)
{
    return open_file(file, path, MODE_NEWFILE, TRUE);
}

void VT_FileClose(VTFile *file)
{
    unsigned char *buffer;

    if (!file)
        return;
    if (file->pb) {
        if (file->writing)
            avio_flush(file->pb);
        buffer = file->pb->buffer;
        file->pb->buffer = NULL;
        avio_context_free(&file->pb);
        av_free(buffer);
    }
    if (file->fh) {
        Close(file->fh);
        file->fh = 0;
    }
    file->size = 0;
    file->bytes_read = 0;
    file->writing = FALSE;
}

int VT_FormatOpenInput(AVFormatContext **fmt, VTFile *file, const char *path,
                       const volatile LONG *cancel, char *error, ULONG error_len)
{
    char url[VT_PATH_LEN];
    AVFormatContext *in;
    int err;

    if (fmt)
        *fmt = NULL;
    if (!fmt || !file || !path) {
        VT_AVError(error, error_len, 0, "Could not open the source.");
        return AVERROR(EINVAL);
    }
    if (!VT_FileOpenRead(file, path)) {
        VT_AVError(error, error_len, 0, "Could not open the source.");
        return AVERROR(EIO);
    }
    in = avformat_alloc_context();
    if (!in) {
        VT_FileClose(file);
        VT_AVError(error, error_len, 0, "Out of memory.");
        return AVERROR(ENOMEM);
    }
    in->pb = file->pb;
    in->flags |= AVFMT_FLAG_CUSTOM_IO;
    in->interrupt_callback.callback = vt_interrupt;
    in->interrupt_callback.opaque = (void *)cancel;
    in->probesize = 5 * 1000 * 1000;
    in->max_analyze_duration = 5 * (int64_t)AV_TIME_BASE;
    VT_FileProbeURL(path, url, sizeof url);
    err = avformat_open_input(&in, url, NULL, NULL);
    if (err < 0) {
        if (cancel && *cancel)
            err = AVERROR_EXIT;
        VT_FileClose(file);
        if (err == AVERROR_EXIT)
            VT_AVError(error, error_len, 0, "Cancelled.");
        else
            VT_AVError(error, error_len, err, "Could not read the source.");
        return err;
    }
    *fmt = in;
    return 0;
}

void VT_FormatCloseInput(AVFormatContext **fmt, VTFile *file)
{
    if (fmt && *fmt) {
        (*fmt)->pb = NULL;
        avformat_close_input(fmt);
    }
    VT_FileClose(file);
}
