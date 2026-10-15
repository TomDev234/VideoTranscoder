/*
 * Encode profiles. The stored object is the snapshot a job keeps.
 * Edit a stack copy, then call VT_ProfileStoreSave. Pointers from
 * VT_ProfileStoreAt stay valid until that profile is deleted or the
 * store is freed or loaded again.
 *
 * On disk: PROGDIR:Profiles/<name>.profile
 *   VTPROFILE 1
 *   name, container, video, video_bitrate, scale, two_pass, audio, audio_bitrate, gain_db
 * scale is original, 576, 720, or 1080. Older files omit it and stay original.
 */
#ifndef VT_PROFILE_H
#define VT_PROFILE_H

#include "compat.h"

#define VT_PROFILE_FORMAT_VERSION 1L
#define VT_PROFILE_MAGIC          "VTPROFILE"
#define VT_PROFILES_DIR           "PROGDIR:Profiles"
#define VT_PROFILE_SUFFIX         ".profile"
#define VT_PROFILE_MAX            256

#define VT_VIDEO_BITRATE_MIN      100L
#define VT_VIDEO_BITRATE_MAX      50000L
#define VT_AUDIO_BITRATE_MIN      32L
#define VT_MP3_BITRATE_MAX        320L
#define VT_AAC_BITRATE_MAX        320L
#define VT_AC3_BITRATE_MAX        640L
#define VT_GAIN_DB_MIN            (-24L)
#define VT_GAIN_DB_MAX            24L

/* Moderate G4 defaults: MPEG-4 / MP3 / MKV, single pass, original size. */
#define VT_DEFAULT_VIDEO_BITRATE  2500L
#define VT_DEFAULT_AUDIO_BITRATE  192L

/* Target picture height. Original keeps the source size. */
typedef enum {
    VT_SCALE_ORIGINAL = 0,
    VT_SCALE_576 = 1,
    VT_SCALE_720 = 2,
    VT_SCALE_1080 = 3,
    VT_SCALE_COUNT = 4
} VTScale;

typedef struct VTProfile {
    char name[VT_NAME_LEN];
    VTContainer container;
    VTVideoCodec video_codec;
    LONG video_bitrate;
    VTScale scale;
    BOOL two_pass;
    VTAudioCodec audio_codec;
    LONG audio_bitrate;
    LONG audio_gain_db;
} VTProfile;

struct VTProfileNode;

typedef struct VTProfileStore {
    struct VTProfileNode **items;
    ULONG count;
    ULONG capacity;
} VTProfileStore;

void VT_ProfileSetDefaults(VTProfile *profile);
void VT_ProfileNormalize(VTProfile *profile);

/* Profile fields only, without the file header. */
BOOL VT_ProfileFormatKeys(const VTProfile *profile, char *text, ULONG text_len);
/* *known is set when key belongs to a profile. FALSE means that value is illegal. */
BOOL VT_ProfileApplyKey(VTProfile *profile, const char *key, const char *value, BOOL *known);
BOOL VT_ProfileNameOK(const char *name);
BOOL VT_ProfileVideoBitrateOK(VTVideoCodec codec, LONG bitrate);
LONG VT_ProfileAudioBitrateMax(VTAudioCodec codec);
BOOL VT_ProfileAudioBitrateOK(VTAudioCodec codec, LONG bitrate);
BOOL VT_ProfileValidate(const VTProfile *profile);
BOOL VT_ProfileSlowOnG4(const VTProfile *profile);
const char *VT_ProfileScaleToken(VTScale scale);
const char *VT_ProfileScaleLabel(VTScale scale);
BOOL VT_ProfileScaleFromToken(const char *token, VTScale *out);

/* Even output size. A scaled frame is square pixels, so the source
   sample aspect is included in the width. Original keeps the source size. */
BOOL VT_ProfileFrameSize(const VTProfile *profile, int src_w, int src_h,
                         int sar_num, int sar_den, int *out_w, int *out_h);

void VT_ProfileStoreInit(VTProfileStore *store);
void VT_ProfileStoreFree(VTProfileStore *store);

/* TRUE when the store holds at least one profile. A missing drawer still
   yields an in-memory Default. FALSE means memory could not be allocated. */
BOOL VT_ProfileStoreLoad(VTProfileStore *store);

LONG VT_ProfileStoreIndex(const VTProfileStore *store, const char *name);
const VTProfile *VT_ProfileStoreAt(const VTProfileStore *store, ULONG index);

LONG VT_ProfileStoreCreate(VTProfileStore *store, const char *name);
LONG VT_ProfileStoreDuplicate(VTProfileStore *store, ULONG index);
BOOL VT_ProfileStoreRename(VTProfileStore *store, ULONG index, const char *name);
BOOL VT_ProfileStoreSave(VTProfileStore *store, ULONG index, const VTProfile *profile);
/* Drops the profile from the store. *file_gone may be NULL.
   *file_gone is TRUE when the profile file is no longer on disk. */
BOOL VT_ProfileStoreDelete(VTProfileStore *store, ULONG index, BOOL *file_gone);

#endif
