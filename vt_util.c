#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "vt_util.h"

BOOL VT_StrCopy(char *dst, ULONG dst_len, const char *src)
{
    ULONG len;

    if (!dst || dst_len == 0)
        return FALSE;
    if (!src)
        src = "";
    len = (ULONG)strlen(src);
    if (len + 1 > dst_len) {
        dst[0] = '\0';
        return FALSE;
    }
    memcpy(dst, src, len + 1);
    return TRUE;
}

BOOL VT_StrCopyTrunc(char *dst, ULONG dst_len, const char *src)
{
    ULONG len;
    ULONG keep;

    if (!dst || dst_len == 0)
        return FALSE;
    if (!src)
        src = "";
    len = (ULONG)strlen(src);
    keep = len;
    if (keep + 1 > dst_len)
        keep = dst_len - 1;
    memcpy(dst, src, keep);
    dst[keep] = '\0';
    return TRUE;
}

BOOL VT_StrEq(const char *a, const char *b)
{
    if (a == b)
        return TRUE;
    if (!a || !b)
        return FALSE;
    return (BOOL)(strcmp(a, b) == 0);
}

BOOL VT_StrEqI(const char *a, const char *b)
{
    if (a == b)
        return TRUE;
    if (!a || !b)
        return FALSE;
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return FALSE;
        if (ca == 0)
            return TRUE;
    }
}

BOOL VT_EndsWithI(const char *text, const char *suffix)
{
    ULONG text_len;
    ULONG suffix_len;

    if (!text || !suffix)
        return FALSE;
    text_len = (ULONG)strlen(text);
    suffix_len = (ULONG)strlen(suffix);
    if (suffix_len > text_len)
        return FALSE;
    return VT_StrEqI(text + (text_len - suffix_len), suffix);
}

void VT_Trim(char *text)
{
    char *start;
    char *end;
    ULONG len;

    if (!text)
        return;
    start = text;
    while (*start == ' ' || *start == '\t')
        start++;
    if (start != text) {
        len = (ULONG)strlen(start);
        memmove(text, start, len + 1);
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';
}

BOOL VT_IsSingleLine(const char *text)
{
    const char *cursor;

    if (!text)
        return FALSE;
    for (cursor = text; *cursor; cursor++) {
        if (*cursor == '\n' || *cursor == '\r')
            return FALSE;
    }
    return TRUE;
}

BOOL VT_ParseLong(const char *text, LONG *out)
{
    const char *cursor;
    long sign = 1;
    unsigned long value = 0;
    BOOL any = FALSE;

    if (!text || !out)
        return FALSE;
    cursor = text;
    if (*cursor == '-') {
        sign = -1;
        cursor++;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        any = TRUE;
        value = value * 10UL + (unsigned long)(*cursor - '0');
        if (value > 2147483647UL)
            return FALSE;
        cursor++;
    }
    if (!any || *cursor != '\0')
        return FALSE;
    if (sign < 0 && value > 2147483647UL)
        return FALSE;
    *out = (LONG)(sign * (long)value);
    return TRUE;
}

BOOL VT_ParseKeyValue(const char *line, char *key, ULONG key_len, char *value, ULONG value_len)
{
    const char *eq;
    ULONG key_bytes;
    ULONG value_bytes;

    if (!line || !key || !value || key_len == 0 || value_len == 0)
        return FALSE;
    eq = line;
    while (*eq && *eq != '=')
        eq++;
    if (*eq != '=')
        return FALSE;
    key_bytes = (ULONG)(eq - line);
    if (key_bytes == 0 || key_bytes + 1 > key_len)
        return FALSE;
    memcpy(key, line, key_bytes);
    key[key_bytes] = '\0';
    VT_Trim(key);
    if (key[0] == '\0')
        return FALSE;

    eq++;
    value_bytes = (ULONG)strlen(eq);
    if (value_bytes + 1 > value_len)
        return FALSE;
    memcpy(value, eq, value_bytes + 1);
    VT_Trim(value);
    return TRUE;
}

BOOL VT_IsFormatHeader(const char *line, const char *magic, LONG version)
{
    char expect[32];
    int wrote;

    if (!line || !magic)
        return FALSE;
    wrote = snprintf(expect, sizeof expect, "%s %ld", magic, (long)version);
    if (wrote < 0 || (ULONG)wrote >= sizeof expect)
        return FALSE;
    return VT_StrEq(line, expect);
}

static BOOL next_line(const char **cursor, char *buf, ULONG buflen, BOOL *truncated)
{
    const char *src;
    ULONG used = 0;
    BOOL cut = FALSE;

    if (!cursor || !*cursor || !buf || buflen < 2)
        return FALSE;
    src = *cursor;
    if (*src == '\0')
        return FALSE;
    while (*src != '\0' && *src != '\n' && *src != '\r') {
        if (used + 1 < buflen)
            buf[used++] = *src;
        else
            cut = TRUE;
        src++;
    }
    buf[used] = '\0';
    if (*src == '\r')
        src++;
    if (*src == '\n')
        src++;
    *cursor = src;
    if (truncated)
        *truncated = cut;
    return TRUE;
}

BOOL VT_ForEachLine(const char *text, VTLineFunc func, APTR user)
{
    const char *cursor;
    char line[VT_PATH_LEN + 64];

    if (!text || !func)
        return FALSE;
    cursor = text;
    while (*cursor) {
        BOOL truncated = FALSE;

        if (!next_line(&cursor, line, sizeof line, &truncated))
            break;
        if (truncated)
            return FALSE;
        VT_Trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (!func(line, user))
            return FALSE;
    }
    return TRUE;
}

BOOL VT_BaseName(const char *path, char *out, ULONG out_len)
{
    const char *base;
    const char *cursor;

    if (!path || !path[0] || !out)
        return FALSE;
    base = path;
    for (cursor = path; *cursor; cursor++) {
        if (*cursor == '/' || *cursor == ':')
            base = cursor + 1;
    }
    if (base[0] == '\0')
        return FALSE;
    return VT_StrCopy(out, out_len, base);
}

BOOL VT_StripExtension(const char *name, char *stem, ULONG stem_len)
{
    const char *dot = NULL;
    const char *cursor;
    ULONG stem_bytes;

    if (!name || !name[0] || !stem || stem_len == 0)
        return FALSE;
    for (cursor = name; *cursor; cursor++) {
        if (*cursor == '.')
            dot = cursor;
    }
    if (!dot || dot == name)
        return VT_StrCopy(stem, stem_len, name);
    stem_bytes = (ULONG)(dot - name);
    if (stem_bytes == 0 || stem_bytes + 1 > stem_len)
        return FALSE;
    memcpy(stem, name, stem_bytes);
    stem[stem_bytes] = '\0';
    return TRUE;
}

BOOL VT_JoinDrawerFile(const char *drawer, const char *file_name, char *out, ULONG out_len)
{
    ULONG drawer_len;
    char last;
    int wrote;

    if (!drawer || !drawer[0] || !file_name || !file_name[0] || !out || out_len == 0)
        return FALSE;
    drawer_len = (ULONG)strlen(drawer);
    last = drawer[drawer_len - 1];
    if (last == ':' || last == '/')
        wrote = snprintf(out, out_len, "%s%s", drawer, file_name);
    else
        wrote = snprintf(out, out_len, "%s/%s", drawer, file_name);
    if (wrote < 0 || (ULONG)wrote >= out_len) {
        out[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

BOOL VT_FileExists(const char *path)
{
    BPTR lock;

    if (!path || !path[0])
        return FALSE;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock)
        return FALSE;
    UnLock(lock);
    return TRUE;
}

BOOL VT_DeleteIfExists(const char *path)
{
    if (!path || !path[0])
        return FALSE;
    if (!VT_FileExists(path))
        return TRUE;
    return (BOOL)(DeleteFile((STRPTR)path) != 0);
}

BOOL VT_EnsureDir(const char *path)
{
    BPTR lock;

    if (!path || !path[0])
        return FALSE;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock)
        lock = CreateDir((STRPTR)path);
    if (!lock)
        return FALSE;
    UnLock(lock);
    return TRUE;
}

BOOL VT_ReadTextFile(const char *path, char **out_text, LONG max_bytes)
{
    BPTR handle;
    LONG size;
    char *text;
    LONG got;

    if (!path || !out_text || max_bytes < 1)
        return FALSE;
    *out_text = NULL;
    handle = Open((STRPTR)path, MODE_OLDFILE);
    if (!handle)
        return FALSE;

    /*
     * Seek returns the previous position. The second call moves back to
     * the start and therefore returns the length.
     */
    if (Seek(handle, 0, OFFSET_END) < 0) {
        Close(handle);
        return FALSE;
    }
    size = Seek(handle, 0, OFFSET_BEGINNING);
    if (size < 0 || size > max_bytes) {
        Close(handle);
        return FALSE;
    }
    text = (char *)AllocVec((ULONG)size + 1, MEMF_PUBLIC | MEMF_CLEAR);
    if (!text) {
        Close(handle);
        return FALSE;
    }
    if (size > 0) {
        got = Read(handle, text, size);
        if (got != size) {
            FreeVec(text);
            Close(handle);
            return FALSE;
        }
    }
    text[size] = '\0';
    Close(handle);
    *out_text = text;
    return TRUE;
}

void VT_FreeText(char *text)
{
    if (text)
        FreeVec(text);
}

static BOOL suffix_path(const char *path, const char *suffix, char *out, ULONG out_len)
{
    int wrote;

    wrote = snprintf(out, out_len, "%s%s", path, suffix);
    if (wrote < 0 || (ULONG)wrote >= out_len) {
        if (out_len > 0)
            out[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

BOOL VT_WriteTextFileAtomic(const char *path, const char *text)
{
    char tmp_path[VT_PATH_LEN];
    char bak_path[VT_PATH_LEN];
    BPTR handle;
    LONG length;
    LONG wrote;
    BOOL had_original = FALSE;

    if (!path || !path[0] || !text)
        return FALSE;
    length = (LONG)strlen(text);
    if (length <= 0 || length > VT_TEXT_FILE_MAX)
        return FALSE;
    if (!suffix_path(path, ".tmp", tmp_path, sizeof tmp_path))
        return FALSE;
    if (!suffix_path(path, ".bak", bak_path, sizeof bak_path))
        return FALSE;

    DeleteFile((STRPTR)tmp_path);
    handle = Open((STRPTR)tmp_path, MODE_NEWFILE);
    if (!handle)
        return FALSE;
    wrote = Write(handle, (APTR)text, length);
    Close(handle);
    if (wrote != length) {
        DeleteFile((STRPTR)tmp_path);
        return FALSE;
    }

    had_original = VT_FileExists(path);
    if (had_original) {
        DeleteFile((STRPTR)bak_path);
        if (!Rename((STRPTR)path, (STRPTR)bak_path)) {
            DeleteFile((STRPTR)tmp_path);
            return FALSE;
        }
    }
    if (!Rename((STRPTR)tmp_path, (STRPTR)path)) {
        if (had_original)
            Rename((STRPTR)bak_path, (STRPTR)path);
        DeleteFile((STRPTR)tmp_path);
        return FALSE;
    }
    if (had_original)
        DeleteFile((STRPTR)bak_path);
    return TRUE;
}

BOOL VT_ArrayGrow(APTR *block, ULONG *capacity, ULONG count, ULONG elem_size, ULONG max_count)
{
    ULONG new_cap;
    ULONG bytes;
    APTR fresh;

    if (!block || !capacity || elem_size == 0 || max_count == 0)
        return FALSE;
    if (*capacity >= max_count)
        return FALSE;
    new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
    if (new_cap < *capacity || new_cap > max_count)
        new_cap = max_count;
    if (count >= new_cap)
        return FALSE;
    if (new_cap > (0xFFFFFFFFUL / elem_size))
        return FALSE;
    bytes = new_cap * elem_size;
    fresh = AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fresh)
        return FALSE;
    if (*block && count > 0)
        CopyMem(*block, fresh, count * elem_size);
    if (*block)
        FreeVec(*block);
    *block = fresh;
    *capacity = new_cap;
    return TRUE;
}
