/*
 * Strings, AmigaDOS paths, and the small text files used by profiles and prefs.
 * One implementation of each helper so the models do not grow their own copies.
 */
#ifndef VT_UTIL_H
#define VT_UTIL_H

#include "vt_types.h"

#define VT_TEXT_FILE_MAX 65535L

typedef BOOL (*VTLineFunc)(const char *line, APTR user);

/* Strict copy. FALSE leaves dst empty when src does not fit. */
BOOL VT_StrCopy(char *dst, ULONG dst_len, const char *src);

/* Display text may be shortened. FALSE only when dst is unusable. */
BOOL VT_StrCopyTrunc(char *dst, ULONG dst_len, const char *src);

BOOL VT_StrEq(const char *a, const char *b);
BOOL VT_StrEqI(const char *a, const char *b);
BOOL VT_EndsWithI(const char *text, const char *suffix);
void VT_Trim(char *text);
BOOL VT_IsSingleLine(const char *text);
BOOL VT_ParseLong(const char *text, LONG *out);
BOOL VT_ParseKeyValue(const char *line, char *key, ULONG key_len, char *value, ULONG value_len);
BOOL VT_IsFormatHeader(const char *line, const char *magic, LONG version);

/* Skips blank lines and '#' comments. FALSE if a line is too long or func says so. */
BOOL VT_ForEachLine(const char *text, VTLineFunc func, APTR user);

BOOL VT_BaseName(const char *path, char *out, ULONG out_len);
BOOL VT_StripExtension(const char *name, char *stem, ULONG stem_len);
BOOL VT_JoinDrawerFile(const char *drawer, const char *file_name, char *out, ULONG out_len);

BOOL VT_FileExists(const char *path);
BOOL VT_DeleteIfExists(const char *path);
BOOL VT_EnsureDir(const char *path);
BOOL VT_ReadTextFile(const char *path, char **out_text, LONG max_bytes);
void VT_FreeText(char *text);
BOOL VT_WriteTextFileAtomic(const char *path, const char *text);

BOOL VT_ArrayGrow(APTR *block, ULONG *capacity, ULONG count, ULONG elem_size, ULONG max_count);

#endif
