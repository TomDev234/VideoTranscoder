/*
 * Single-pass MPEG-4 / MP3 / MKV. Phase 4 extends VT_EncodeSupported
 * and the codec setup. This file does not include MUI.
 */
#ifndef VT_ENCODER_H
#define VT_ENCODER_H

#include "profile.h"

typedef enum {
    VT_ENCODE_OK = 0,
    VT_ENCODE_FAILED = 1,
    VT_ENCODE_CANCELLED = 2
} VTEncodeResult;

typedef void (*VTEncodeProgressFunc)(LONG percent, APTR user);

BOOL VT_EncodeSupported(const VTProfile *profile);
const char *VT_EncodeUnsupportedReason(void);

VTEncodeResult VT_EncodeFile(const char *source, const char *output,
                             const VTProfile *profile,
                             const volatile LONG *cancel,
                             VTEncodeProgressFunc progress, APTR user,
                             char *error, ULONG error_len);

#endif
