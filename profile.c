#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "profile.h"
#include "vt_util.h"

struct VTProfileNode {
    VTProfile profile;
    char path[VT_PATH_LEN];
};

void VT_ProfileSetDefaults(VTProfile *profile)
{
    if (!profile)
        return;
    memset(profile, 0, sizeof *profile);
    VT_StrCopy(profile->name, sizeof profile->name, VT_DEFAULT_PROFILE_NAME);
    profile->container = VT_CONTAINER_MKV;
    profile->video_codec = VT_VCODEC_MPEG4;
    profile->video_bitrate = VT_DEFAULT_VIDEO_BITRATE;
    profile->scale = VT_SCALE_ORIGINAL;
    profile->two_pass = FALSE;
    profile->audio_codec = VT_ACODEC_MP3;
    profile->audio_bitrate = VT_DEFAULT_AUDIO_BITRATE;
    profile->audio_gain_db = 0;
}

void VT_ProfileNormalize(VTProfile *profile)
{
    if (!profile)
        return;
    VT_Trim(profile->name);
    profile->two_pass = FALSE;
}

BOOL VT_ProfileNameOK(const char *name)
{
    const char *cursor;
    BOOL any_visible = FALSE;

    if (!name || !name[0])
        return FALSE;
    if ((ULONG)strlen(name) >= VT_NAME_LEN)
        return FALSE;
    if (VT_StrEq(name, ".") || VT_StrEq(name, ".."))
        return FALSE;
    for (cursor = name; *cursor; cursor++) {
        unsigned char ch = (unsigned char)*cursor;

        if (ch < 32 || ch == ':' || ch == '/' || ch == '\\' || ch == '\"' ||
            ch == '*' || ch == '?' || ch == ';' || ch == '|')
            return FALSE;
        if (ch != ' ' && ch != '\t')
            any_visible = TRUE;
    }
    return any_visible;
}

BOOL VT_ProfileVideoBitrateOK(VTVideoCodec codec, LONG bitrate)
{
    if (codec == VT_VCODEC_COPY)
        return TRUE;
    if ((ULONG)codec >= VT_VCODEC_COUNT)
        return FALSE;
    return (BOOL)(bitrate >= VT_VIDEO_BITRATE_MIN && bitrate <= VT_VIDEO_BITRATE_MAX);
}

LONG VT_ProfileAudioBitrateMax(VTAudioCodec codec)
{
    switch (codec) {
    case VT_ACODEC_MP3:
        return VT_MP3_BITRATE_MAX;
    case VT_ACODEC_AAC:
        return VT_AAC_BITRATE_MAX;
    case VT_ACODEC_AC3:
        return VT_AC3_BITRATE_MAX;
    default:
        return 0;
    }
}

BOOL VT_ProfileAudioBitrateOK(VTAudioCodec codec, LONG bitrate)
{
    LONG max_rate;

    if (codec == VT_ACODEC_COPY)
        return TRUE;
    max_rate = VT_ProfileAudioBitrateMax(codec);
    if (max_rate <= 0)
        return FALSE;
    return (BOOL)(bitrate >= VT_AUDIO_BITRATE_MIN && bitrate <= max_rate);
}

BOOL VT_ProfileValidate(const VTProfile *profile)
{
    if (!profile)
        return FALSE;
    if (!VT_ProfileNameOK(profile->name))
        return FALSE;
    if (!VT_CompatPair(profile->container, profile->video_codec, profile->audio_codec))
        return FALSE;
    if (profile->video_codec == VT_VCODEC_COPY && profile->two_pass)
        return FALSE;
    if ((ULONG)profile->scale >= VT_SCALE_COUNT)
        return FALSE;
    if (!VT_ProfileVideoBitrateOK(profile->video_codec, profile->video_bitrate))
        return FALSE;
    if (!VT_ProfileAudioBitrateOK(profile->audio_codec, profile->audio_bitrate))
        return FALSE;
    if (profile->audio_gain_db < VT_GAIN_DB_MIN || profile->audio_gain_db > VT_GAIN_DB_MAX)
        return FALSE;
    return TRUE;
}

BOOL VT_ProfileSlowOnG4(const VTProfile *profile)
{
    if (!profile)
        return FALSE;
    if (profile->video_codec == VT_VCODEC_H264)
        return TRUE;
    if (profile->scale == VT_SCALE_1080 && profile->video_codec != VT_VCODEC_COPY)
        return TRUE;
    return FALSE;
}

const char *VT_ProfileScaleToken(VTScale scale)
{
    switch (scale) {
    case VT_SCALE_ORIGINAL:
        return "original";
    case VT_SCALE_576:
        return "576";
    case VT_SCALE_720:
        return "720";
    case VT_SCALE_1080:
        return "1080";
    default:
        return NULL;
    }
}

const char *VT_ProfileScaleLabel(VTScale scale)
{
    switch (scale) {
    case VT_SCALE_ORIGINAL:
        return "Original";
    case VT_SCALE_576:
        return "576p";
    case VT_SCALE_720:
        return "720p";
    case VT_SCALE_1080:
        return "1080p";
    default:
        return NULL;
    }
}

BOOL VT_ProfileScaleFromToken(const char *token, VTScale *out)
{
    ULONG scale;

    if (!token || !out)
        return FALSE;
    for (scale = 0; scale < VT_SCALE_COUNT; scale++) {
        if (VT_StrEq(token, VT_ProfileScaleToken((VTScale)scale))) {
            *out = (VTScale)scale;
            return TRUE;
        }
    }
    return FALSE;
}

static int scale_height(VTScale scale)
{
    switch (scale) {
    case VT_SCALE_576:
        return 576;
    case VT_SCALE_720:
        return 720;
    case VT_SCALE_1080:
        return 1080;
    default:
        return 0;
    }
}

BOOL VT_ProfileFrameSize(const VTProfile *profile, int src_w, int src_h,
                         int sar_num, int sar_den, int *out_w, int *out_h)
{
    int target_h;
    long long wide;
    long long den;
    long long num;

    if (!profile || !out_w || !out_h)
        return FALSE;
    src_w &= ~1;
    src_h &= ~1;
    if (src_w < 2 || src_h < 2)
        return FALSE;
    target_h = scale_height(profile->scale);
    if (profile->video_codec == VT_VCODEC_COPY || target_h <= 0) {
        *out_w = src_w;
        *out_h = src_h;
        return TRUE;
    }
    if (sar_num <= 0 || sar_den <= 0) {
        sar_num = 1;
        sar_den = 1;
    }
    den = (long long)src_h * (long long)sar_den;
    num = (long long)target_h * (long long)src_w * (long long)sar_num;
    if (den <= 0)
        return FALSE;
    wide = (num + den / 2) / den;
    if (wide < 2)
        wide = 2;
    wide = (wide + 1) & ~1LL;
    if (wide > 8192 || target_h > 8192)
        return FALSE;
    *out_w = (int)wide;
    *out_h = target_h;
    return TRUE;
}

void VT_ProfileStoreInit(VTProfileStore *store)
{
    if (!store)
        return;
    store->items = NULL;
    store->count = 0;
    store->capacity = 0;
}

static void free_profiles(VTProfileStore *store)
{
    ULONG index;

    if (!store->items) {
        store->count = 0;
        store->capacity = 0;
        return;
    }
    for (index = 0; index < store->count; index++)
        FreeVec(store->items[index]);
    FreeVec(store->items);
    store->items = NULL;
    store->count = 0;
    store->capacity = 0;
}

void VT_ProfileStoreFree(VTProfileStore *store)
{
    if (!store)
        return;
    free_profiles(store);
}

LONG VT_ProfileStoreIndex(const VTProfileStore *store, const char *name)
{
    ULONG index;

    if (!store || !name)
        return -1;
    for (index = 0; index < store->count; index++) {
        if (VT_StrEqI(store->items[index]->profile.name, name))
            return (LONG)index;
    }
    return -1;
}

const VTProfile *VT_ProfileStoreAt(const VTProfileStore *store, ULONG index)
{
    if (!store || index >= store->count)
        return NULL;
    return &store->items[index]->profile;
}

static BOOL profile_path(const VTProfile *profile, char *out, ULONG out_len)
{
    char file_name[VT_NAME_LEN + 16];
    int wrote;

    if (!profile)
        return FALSE;
    wrote = snprintf(file_name, sizeof file_name, "%s%s", profile->name, VT_PROFILE_SUFFIX);
    if (wrote < 0 || (ULONG)wrote >= sizeof file_name)
        return FALSE;
    return VT_JoinDrawerFile(VT_PROFILES_DIR, file_name, out, out_len);
}

BOOL VT_ProfileFormatKeys(const VTProfile *profile, char *text, ULONG text_len)
{
    const char *container;
    const char *video;
    const char *audio;
    const char *scale;
    int wrote;

    if (!profile || !text || text_len == 0)
        return FALSE;
    container = VT_ContainerToken(profile->container);
    video = VT_VideoCodecToken(profile->video_codec);
    audio = VT_AudioCodecToken(profile->audio_codec);
    scale = VT_ProfileScaleToken(profile->scale);
    if (!container || !video || !audio || !scale)
        return FALSE;
    wrote = snprintf(text, text_len,
                     "name=%s\n"
                     "container=%s\n"
                     "video=%s\n"
                     "video_bitrate=%ld\n"
                     "scale=%s\n"
                     "two_pass=%ld\n"
                     "audio=%s\n"
                     "audio_bitrate=%ld\n"
                     "gain_db=%ld\n",
                     profile->name,
                     container,
                     video,
                     (long)profile->video_bitrate,
                     scale,
                     profile->two_pass ? 1L : 0L,
                     audio,
                     (long)profile->audio_bitrate,
                     (long)profile->audio_gain_db);
    if (wrote < 0 || (ULONG)wrote >= text_len)
        return FALSE;
    return TRUE;
}

BOOL VT_ProfileApplyKey(VTProfile *profile, const char *key, const char *value, BOOL *known)
{
    LONG number;

    if (known)
        *known = FALSE;
    if (!profile || !key || !value)
        return FALSE;
    if (VT_StrEq(key, "name")) {
        if (known)
            *known = TRUE;
        return VT_StrCopy(profile->name, sizeof profile->name, value);
    }
    if (VT_StrEq(key, "container")) {
        if (known)
            *known = TRUE;
        return VT_ContainerFromToken(value, &profile->container);
    }
    if (VT_StrEq(key, "video")) {
        if (known)
            *known = TRUE;
        return VT_VideoCodecFromToken(value, &profile->video_codec);
    }
    if (VT_StrEq(key, "audio")) {
        if (known)
            *known = TRUE;
        return VT_AudioCodecFromToken(value, &profile->audio_codec);
    }
    if (VT_StrEq(key, "scale")) {
        if (known)
            *known = TRUE;
        return VT_ProfileScaleFromToken(value, &profile->scale);
    }
    if (!VT_StrEq(key, "video_bitrate") && !VT_StrEq(key, "audio_bitrate") &&
        !VT_StrEq(key, "gain_db") && !VT_StrEq(key, "two_pass"))
        return TRUE;
    if (known)
        *known = TRUE;
    if (!VT_ParseLong(value, &number))
        return FALSE;
    if (VT_StrEq(key, "video_bitrate"))
        profile->video_bitrate = number;
    else if (VT_StrEq(key, "audio_bitrate"))
        profile->audio_bitrate = number;
    else if (VT_StrEq(key, "gain_db"))
        profile->audio_gain_db = number;
    else if (number == 0 || number == 1)
        profile->two_pass = number ? TRUE : FALSE;
    else
        return FALSE;
    return TRUE;
}

static BOOL format_profile(const VTProfile *profile, char *text, ULONG text_len)
{
    char keys[512];
    int wrote;

    if (!VT_ProfileFormatKeys(profile, keys, sizeof keys))
        return FALSE;
    wrote = snprintf(text, text_len, "%s %ld\n%s",
                     VT_PROFILE_MAGIC, (long)VT_PROFILE_FORMAT_VERSION, keys);
    if (wrote < 0 || (ULONG)wrote >= text_len)
        return FALSE;
    return TRUE;
}

static BOOL write_profile_file(const VTProfile *profile)
{
    char path[VT_PATH_LEN];
    char text[1024];

    if (!VT_EnsureDir(VT_PROFILES_DIR))
        return FALSE;
    if (!profile_path(profile, path, sizeof path))
        return FALSE;
    if (!format_profile(profile, text, sizeof text))
        return FALSE;
    return VT_WriteTextFileAtomic(path, text);
}

static LONG append_profile(VTProfileStore *store, const VTProfile *src, const char *loaded_path, BOOL write)
{
    VTProfile edited;
    struct VTProfileNode *slot;
    char canonical[VT_PATH_LEN];
    ULONG index;

    if (!store || !src)
        return -1;
    edited = *src;
    VT_ProfileNormalize(&edited);
    if (!VT_ProfileValidate(&edited))
        return -1;
    if (VT_ProfileStoreIndex(store, edited.name) >= 0)
        return -1;
    if (!profile_path(&edited, canonical, sizeof canonical))
        return -1;
    if (store->count >= VT_PROFILE_MAX)
        return -1;
    if (store->count == store->capacity) {
        if (!VT_ArrayGrow((APTR *)&store->items, &store->capacity, store->count,
                          sizeof(struct VTProfileNode *), VT_PROFILE_MAX))
            return -1;
    }

    slot = (struct VTProfileNode *)AllocVec(sizeof(struct VTProfileNode), MEMF_PUBLIC | MEMF_CLEAR);
    if (!slot)
        return -1;
    slot->profile = edited;
    if (loaded_path && loaded_path[0]) {
        if (!VT_StrCopy(slot->path, sizeof slot->path, loaded_path)) {
            FreeVec(slot);
            return -1;
        }
    } else if (!VT_StrCopy(slot->path, sizeof slot->path, canonical)) {
        FreeVec(slot);
        return -1;
    }
    index = store->count;
    store->items[index] = slot;
    store->count++;
    if (write && !write_profile_file(&slot->profile)) {
        store->count--;
        store->items[index] = NULL;
        FreeVec(slot);
        return -1;
    }
    if (write)
        VT_StrCopy(slot->path, sizeof slot->path, canonical);
    return (LONG)index;
}

LONG VT_ProfileStoreCreate(VTProfileStore *store, const char *name)
{
    VTProfile profile;
    char trimmed[VT_NAME_LEN];

    if (!store || !name)
        return -1;
    if (!VT_StrCopy(trimmed, sizeof trimmed, name))
        return -1;
    VT_Trim(trimmed);
    VT_ProfileSetDefaults(&profile);
    if (!VT_StrCopy(profile.name, sizeof profile.name, trimmed))
        return -1;
    return append_profile(store, &profile, NULL, TRUE);
}

static BOOL make_unique_name(const VTProfileStore *store, const char *base, char *out, ULONG out_len)
{
    ULONG number;

    for (number = 2; number <= 999; number++) {
        char candidate[VT_NAME_LEN];
        int wrote;

        wrote = snprintf(candidate, sizeof candidate, "%s %lu", base, (unsigned long)number);
        if (wrote < 0 || (ULONG)wrote >= sizeof candidate)
            return FALSE;
        if (!VT_ProfileNameOK(candidate))
            continue;
        if (VT_ProfileStoreIndex(store, candidate) >= 0)
            continue;
        return VT_StrCopy(out, out_len, candidate);
    }
    return FALSE;
}

LONG VT_ProfileStoreDuplicate(VTProfileStore *store, ULONG index)
{
    const VTProfile *src;
    VTProfile copy;
    char name[VT_NAME_LEN];

    src = VT_ProfileStoreAt(store, index);
    if (!src)
        return -1;
    copy = *src;
    if (!make_unique_name(store, src->name, name, sizeof name))
        return -1;
    if (!VT_StrCopy(copy.name, sizeof copy.name, name))
        return -1;
    return append_profile(store, &copy, NULL, TRUE);
}

BOOL VT_ProfileStoreSave(VTProfileStore *store, ULONG index, const VTProfile *profile)
{
    struct VTProfileNode *node;
    VTProfile edited;
    VTProfile previous;
    char old_path[VT_PATH_LEN];
    char new_path[VT_PATH_LEN];
    LONG clash;

    if (!store || !profile || index >= store->count)
        return FALSE;
    node = store->items[index];
    edited = *profile;
    VT_ProfileNormalize(&edited);
    if (!VT_ProfileValidate(&edited))
        return FALSE;
    clash = VT_ProfileStoreIndex(store, edited.name);
    if (clash >= 0 && (ULONG)clash != index)
        return FALSE;
    if (!VT_StrCopy(old_path, sizeof old_path, node->path))
        return FALSE;
    if (!profile_path(&edited, new_path, sizeof new_path))
        return FALSE;

    previous = node->profile;
    node->profile = edited;
    if (!write_profile_file(&node->profile)) {
        node->profile = previous;
        return FALSE;
    }
    if (!VT_StrEqI(old_path, new_path) && !VT_DeleteIfExists(old_path)) {
        node->profile = previous;
        write_profile_file(&node->profile);
        VT_DeleteIfExists(new_path);
        return FALSE;
    }
    VT_StrCopy(node->path, sizeof node->path, new_path);
    return TRUE;
}

BOOL VT_ProfileStoreRename(VTProfileStore *store, ULONG index, const char *name)
{
    VTProfile edited;
    const VTProfile *current;

    current = VT_ProfileStoreAt(store, index);
    if (!current || !name)
        return FALSE;
    edited = *current;
    if (!VT_StrCopy(edited.name, sizeof edited.name, name))
        return FALSE;
    return VT_ProfileStoreSave(store, index, &edited);
}

BOOL VT_ProfileStoreDelete(VTProfileStore *store, ULONG index)
{
    char path[VT_PATH_LEN];
    ULONG cursor;

    if (!store || index >= store->count)
        return FALSE;
    if (!VT_StrCopy(path, sizeof path, store->items[index]->path))
        return FALSE;
    if (!VT_DeleteIfExists(path))
        return FALSE;

    FreeVec(store->items[index]);
    for (cursor = index; cursor + 1 < store->count; cursor++)
        store->items[cursor] = store->items[cursor + 1];
    store->count--;
    store->items[store->count] = NULL;

    if (store->count == 0) {
        VTProfile fallback;
        LONG created;

        VT_ProfileSetDefaults(&fallback);
        created = append_profile(store, &fallback, NULL, FALSE);
        if (created < 0)
            return FALSE;
        write_profile_file(&store->items[created]->profile);
    }
    return TRUE;
}

typedef struct ProfileParse {
    VTProfile *profile;
    BOOL saw_header;
    BOOL saw_name;
} ProfileParse;

static BOOL apply_profile_line(const char *line, APTR user)
{
    ProfileParse *state = (ProfileParse *)user;
    char key[32];
    char value[VT_NAME_LEN];
    BOOL known = FALSE;

    if (!state->saw_header) {
        if (!VT_IsFormatHeader(line, VT_PROFILE_MAGIC, VT_PROFILE_FORMAT_VERSION))
            return FALSE;
        state->saw_header = TRUE;
        return TRUE;
    }
    if (!VT_ParseKeyValue(line, key, sizeof key, value, sizeof value))
        return FALSE;
    if (!VT_ProfileApplyKey(state->profile, key, value, &known))
        return FALSE;
    if (known && VT_StrEq(key, "name"))
        state->saw_name = TRUE;
    return TRUE;
}

static BOOL read_profile_file(const char *path, VTProfile *out)
{
    char *text = NULL;
    ProfileParse state;
    VTProfile parsed;

    VT_ProfileSetDefaults(&parsed);
    if (!VT_ReadTextFile(path, &text, 4096))
        return FALSE;
    state.profile = &parsed;
    state.saw_header = FALSE;
    state.saw_name = FALSE;
    if (!VT_ForEachLine(text, apply_profile_line, &state) || !state.saw_header || !state.saw_name) {
        VT_FreeText(text);
        return FALSE;
    }
    VT_FreeText(text);
    VT_ProfileNormalize(&parsed);
    if (!VT_ProfileValidate(&parsed))
        return FALSE;
    *out = parsed;
    return TRUE;
}

static void consider_entry(VTProfileStore *store, const char *file_name, BOOL *out_of_memory)
{
    char path[VT_PATH_LEN];
    VTProfile loaded;

    if (*out_of_memory)
        return;
    if (!file_name || !VT_EndsWithI(file_name, VT_PROFILE_SUFFIX))
        return;
    if (!VT_JoinDrawerFile(VT_PROFILES_DIR, file_name, path, sizeof path))
        return;
    if (!read_profile_file(path, &loaded))
        return;
    if (VT_ProfileStoreIndex(store, loaded.name) >= 0)
        return;
    if (append_profile(store, &loaded, path, FALSE) < 0)
        *out_of_memory = TRUE;
}

BOOL VT_ProfileStoreLoad(VTProfileStore *store)
{
    BPTR lock = 0;
    struct FileInfoBlock *fib = NULL;
    BOOL listed = FALSE;
    BOOL out_of_memory = FALSE;

    if (!store)
        return FALSE;
    free_profiles(store);

    if (VT_EnsureDir(VT_PROFILES_DIR)) {
        lock = Lock((STRPTR)VT_PROFILES_DIR, ACCESS_READ);
        fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
        if (lock && fib && Examine(lock, fib)) {
            listed = TRUE;
            while (ExNext(lock, fib)) {
                if (fib->fib_DirEntryType > 0)
                    continue;
                consider_entry(store, fib->fib_FileName, &out_of_memory);
                if (out_of_memory)
                    break;
            }
        }
    }

    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (lock)
        UnLock(lock);

    if (out_of_memory)
        return (BOOL)(store->count > 0);

    if (store->count == 0) {
        VTProfile fallback;
        LONG index;

        VT_ProfileSetDefaults(&fallback);
        index = append_profile(store, &fallback, NULL, FALSE);
        if (index < 0)
            return FALSE;
        if (listed)
            write_profile_file(&store->items[index]->profile);
    }
    return TRUE;
}
