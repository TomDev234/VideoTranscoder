/*
 * Shared sizes for the VideoTranscoder model layer.
 * Paths follow MorphOS limits; names are shorter so profile
 * filenames stay inside a FileInfoBlock entry.
 */
#ifndef VT_TYPES_H
#define VT_TYPES_H

#include <exec/types.h>

#define VT_NAME_LEN             64
#define VT_PATH_LEN             1024
#define VT_ERROR_LEN            512
#define VT_CODEC_LABEL_LEN      32
#define VT_DEFAULT_PROFILE_NAME "Default"

#endif
