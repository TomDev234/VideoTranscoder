/*
 * Profile editor. Illegal container/codec pairs are removed from the
 * cycles. Save writes through the profile model.
 */
#include <emul/emulregs.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>

#include <stdio.h>

#include "macros.h"
#include "vt_ui.h"
#include "profile.h"
#include "vt_util.h"

struct MUI_CustomClass *VT_ProfileWindowClass;

struct ProfileData {
    VTProfileStore *profiles;
    Object *main_window;
    Object *name;
    Object *container;
    Object *video;
    Object *scale;
    Object *audio;
    Object *video_bitrate;
    Object *audio_bitrate;
    Object *gain;
    Object *slow_note;
    Object *hint;
    Object *error;
    const char *video_entries[VT_VCODEC_COUNT + 1];
    const char *audio_entries[VT_ACODEC_COUNT + 1];
    UBYTE video_map[VT_VCODEC_COUNT];
    UBYTE audio_map[VT_ACODEC_COUNT];
    ULONG video_count;
    ULONG audio_count;
    LONG edit_index;
    BOOL updating;
    char hint_text[160];
    char error_text[160];
};

static const char *container_entries[VT_CONTAINER_COUNT + 1];
static const char *scale_entries[VT_SCALE_COUNT + 1];
static const char *cycle_reset[] = { "", NULL };
static BOOL container_ready;
static BOOL scale_ready;

static void prepare_container_entries(void)
{
    ULONG index;

    if (container_ready)
        return;
    for (index = 0; index < VT_CONTAINER_COUNT; index++)
        container_entries[index] = VT_ContainerLabel((VTContainer)index);
    container_entries[index] = NULL;
    container_ready = TRUE;
}

static void prepare_scale_entries(void)
{
    ULONG index;

    if (scale_ready)
        return;
    for (index = 0; index < VT_SCALE_COUNT; index++)
        scale_entries[index] = VT_ProfileScaleLabel((VTScale)index);
    scale_entries[index] = NULL;
    scale_ready = TRUE;
}

static BOOL allow_video(VTContainer container, ULONG codec)
{
    return VT_CompatVideo(container, (VTVideoCodec)codec);
}

static BOOL allow_audio(VTContainer container, ULONG codec)
{
    return VT_CompatAudio(container, (VTAudioCodec)codec);
}

static const char *label_video(ULONG codec)
{
    return VT_VideoCodecLabel((VTVideoCodec)codec);
}

static const char *label_audio(ULONG codec)
{
    return VT_AudioCodecLabel((VTAudioCodec)codec);
}

static void fill_cycle(Object *cycle, const char **entries, UBYTE *map, ULONG *count,
                       ULONG enum_count, ULONG wanted, VTContainer container,
                       BOOL (*allowed)(VTContainer container, ULONG codec),
                       const char *(*label)(ULONG codec))
{
    ULONG codec;
    ULONG n = 0;
    ULONG select = 0;
    BOOL found = FALSE;

    for (codec = 0; codec < enum_count; codec++) {
        if (!allowed(container, codec))
            continue;
        entries[n] = label(codec);
        map[n] = (UBYTE)codec;
        if (codec == wanted) {
            select = n;
            found = TRUE;
        }
        n++;
    }
    if (n == 0) {
        entries[0] = "";
        map[0] = 0;
        n = 1;
    }
    entries[n] = NULL;
    *count = n;
    if (!found)
        select = 0;
    /* The same array pointer would not refresh the labels. */
    set(cycle, MUIA_Cycle_Entries, cycle_reset);
    set(cycle, MUIA_Cycle_Entries, entries);
    nnset(cycle, MUIA_Cycle_Active, select);
}

static void show_error(struct ProfileData *data, const char *text)
{
    VT_StrCopyTrunc(data->error_text, sizeof data->error_text, text ? text : "");
    set(data->error, MUIA_Text_Contents, data->error_text);
}

static void write_long(Object *string, LONG value)
{
    char buf[16];

    snprintf(buf, sizeof buf, "%ld", (long)value);
    nnset(string, MUIA_String_Contents, buf);
}

static void read_profile(struct ProfileData *data, VTProfile *out)
{
    LONG container;
    LONG video_pos;
    LONG audio_pos;
    LONG scale;
    const char *name;

    VT_ProfileSetDefaults(out);
    name = (const char *)xget(data->name, MUIA_String_Contents);
    VT_StrCopy(out->name, sizeof out->name, name ? name : "");
    VT_Trim(out->name);
    container = (LONG)xget(data->container, MUIA_Cycle_Active);
    if (container >= 0 && container < VT_CONTAINER_COUNT)
        out->container = (VTContainer)container;
    video_pos = (LONG)xget(data->video, MUIA_Cycle_Active);
    if (video_pos >= 0 && (ULONG)video_pos < data->video_count)
        out->video_codec = (VTVideoCodec)data->video_map[video_pos];
    audio_pos = (LONG)xget(data->audio, MUIA_Cycle_Active);
    if (audio_pos >= 0 && (ULONG)audio_pos < data->audio_count)
        out->audio_codec = (VTAudioCodec)data->audio_map[audio_pos];
    out->video_bitrate = (LONG)xget(data->video_bitrate, MUIA_String_Integer);
    scale = (LONG)xget(data->scale, MUIA_Cycle_Active);
    if (scale >= 0 && scale < VT_SCALE_COUNT)
        out->scale = (VTScale)scale;
    out->audio_bitrate = (LONG)xget(data->audio_bitrate, MUIA_String_Integer);
    {
        const char *gain_text = (const char *)xget(data->gain, MUIA_String_Contents);
        LONG gain = 0;

        if (gain_text && gain_text[0] && !VT_ParseLong(gain_text, &gain))
            gain = VT_GAIN_DB_MAX + 1;
        out->audio_gain_db = gain;
    }
    VT_ProfileNormalize(out);
}

static void apply_flags(struct ProfileData *data, const VTProfile *profile)
{
    BOOL video_copy = (BOOL)(profile->video_codec == VT_VCODEC_COPY);
    BOOL audio_copy = (BOOL)(profile->audio_codec == VT_ACODEC_COPY);
    LONG audio_max = VT_ProfileAudioBitrateMax(profile->audio_codec);

    set(data->video_bitrate, MUIA_Disabled, video_copy);
    set(data->scale, MUIA_Disabled, video_copy);
    set(data->audio_bitrate, MUIA_Disabled, audio_copy);
    set(data->gain, MUIA_Disabled, audio_copy);

    if (video_copy && audio_copy) {
        snprintf(data->hint_text, sizeof data->hint_text,
                 "Streams are copied. Size, bitrate, and gain are not applied.");
    } else if (video_copy) {
        snprintf(data->hint_text, sizeof data->hint_text,
                 "Video is copied at its original size. Audio %ld-%ld kbit/s. Gain %ld to %ld dB.",
                 (long)VT_AUDIO_BITRATE_MIN, (long)audio_max,
                 (long)VT_GAIN_DB_MIN, (long)VT_GAIN_DB_MAX);
    } else if (audio_copy) {
        snprintf(data->hint_text, sizeof data->hint_text,
                 "Video %ld-%ld kbit/s. Audio is copied.",
                 (long)VT_VIDEO_BITRATE_MIN, (long)VT_VIDEO_BITRATE_MAX);
    } else {
        snprintf(data->hint_text, sizeof data->hint_text,
                 "Video %ld-%ld kbit/s. Audio %ld-%ld. Gain %ld to %ld dB.",
                 (long)VT_VIDEO_BITRATE_MIN, (long)VT_VIDEO_BITRATE_MAX,
                 (long)VT_AUDIO_BITRATE_MIN, (long)audio_max,
                 (long)VT_GAIN_DB_MIN, (long)VT_GAIN_DB_MAX);
    }
    set(data->hint, MUIA_Text_Contents, data->hint_text);
    set(data->slow_note, MUIA_ShowMe, VT_ProfileSlowOnG4(profile));
}

static void profile_refresh(struct ProfileData *data)
{
    VTProfile draft;

    if (data->updating)
        return;
    data->updating = TRUE;
    read_profile(data, &draft);
    fill_cycle(data->video, data->video_entries, data->video_map, &data->video_count,
               VT_VCODEC_COUNT, (ULONG)draft.video_codec, draft.container,
               allow_video, label_video);
    fill_cycle(data->audio, data->audio_entries, data->audio_map, &data->audio_count,
               VT_ACODEC_COUNT, (ULONG)draft.audio_codec, draft.container,
               allow_audio, label_audio);
    read_profile(data, &draft);
    apply_flags(data, &draft);
    show_error(data, "");
    data->updating = FALSE;
}

static BOOL suggest_name(const VTProfileStore *store, char *out, ULONG out_len)
{
    ULONG number;

    if (VT_ProfileStoreIndex(store, "Profile") < 0)
        return VT_StrCopy(out, out_len, "Profile");
    for (number = 2; number <= 999; number++) {
        char candidate[VT_NAME_LEN];
        int wrote = snprintf(candidate, sizeof candidate, "Profile %lu", (unsigned long)number);

        if (wrote < 0 || (ULONG)wrote >= sizeof candidate)
            return FALSE;
        if (VT_ProfileStoreIndex(store, candidate) >= 0)
            continue;
        return VT_StrCopy(out, out_len, candidate);
    }
    return FALSE;
}

static void fill_from_profile(struct ProfileData *data, const VTProfile *profile)
{
    data->updating = TRUE;
    nnset(data->name, MUIA_String_Contents, profile->name);
    nnset(data->container, MUIA_Cycle_Active, profile->container);
    fill_cycle(data->video, data->video_entries, data->video_map, &data->video_count,
               VT_VCODEC_COUNT, (ULONG)profile->video_codec, profile->container,
               allow_video, label_video);
    fill_cycle(data->audio, data->audio_entries, data->audio_map, &data->audio_count,
               VT_ACODEC_COUNT, (ULONG)profile->audio_codec, profile->container,
               allow_audio, label_audio);
    write_long(data->video_bitrate, profile->video_bitrate);
    nnset(data->scale, MUIA_Cycle_Active, profile->scale);
    write_long(data->audio_bitrate, profile->audio_bitrate);
    write_long(data->gain, profile->audio_gain_db);
    apply_flags(data, profile);
    show_error(data, "");
    data->updating = FALSE;
}

static ULONG profile_use(struct ProfileData *data, Object *obj, struct MUIP_VT_ProfileUse *msg)
{
    VTProfile draft;
    const VTProfile *source = NULL;

    if (msg->index >= 0)
        source = VT_ProfileStoreAt(data->profiles, (ULONG)msg->index);
    if (!source) {
        VT_ProfileSetDefaults(&draft);
        if (!suggest_name(data->profiles, draft.name, sizeof draft.name))
            VT_StrCopy(draft.name, sizeof draft.name, VT_DEFAULT_PROFILE_NAME);
        source = &draft;
        data->edit_index = -1;
        set(obj, MUIA_Window_Title, "New profile");
    } else {
        data->edit_index = msg->index;
        set(obj, MUIA_Window_Title, "Edit profile");
    }
    fill_from_profile(data, source);
    set(obj, MUIA_Window_Open, TRUE);
    set(obj, MUIA_Window_ActiveObject, data->name);
    return 0;
}

static ULONG profile_save(struct ProfileData *data, Object *obj)
{
    VTProfile edited;
    LONG index;
    struct MUIP_VT_ProfilesChanged changed;

    if (!data->profiles)
        return 0;
    read_profile(data, &edited);
    if (!VT_ProfileNameOK(edited.name)) {
        show_error(data, "Use a name without : / or other path characters.");
        return 0;
    }
    if (!VT_ProfileVideoBitrateOK(edited.video_codec, edited.video_bitrate)) {
        snprintf(data->error_text, sizeof data->error_text,
                 "Video bitrate must be %ld-%ld kbit/s.",
                 (long)VT_VIDEO_BITRATE_MIN, (long)VT_VIDEO_BITRATE_MAX);
        set(data->error, MUIA_Text_Contents, data->error_text);
        return 0;
    }
    if (!VT_ProfileAudioBitrateOK(edited.audio_codec, edited.audio_bitrate)) {
        snprintf(data->error_text, sizeof data->error_text,
                 "Audio bitrate must be %ld-%ld kbit/s.",
                 (long)VT_AUDIO_BITRATE_MIN,
                 (long)VT_ProfileAudioBitrateMax(edited.audio_codec));
        set(data->error, MUIA_Text_Contents, data->error_text);
        return 0;
    }
    if (edited.audio_gain_db < VT_GAIN_DB_MIN || edited.audio_gain_db > VT_GAIN_DB_MAX) {
        snprintf(data->error_text, sizeof data->error_text,
                 "Gain must be %ld to %ld dB.",
                 (long)VT_GAIN_DB_MIN, (long)VT_GAIN_DB_MAX);
        set(data->error, MUIA_Text_Contents, data->error_text);
        return 0;
    }
    if (!VT_ProfileValidate(&edited)) {
        show_error(data, "That codec pair is not valid for this container.");
        return 0;
    }

    index = VT_ProfileStoreIndex(data->profiles, edited.name);
    if (data->edit_index < 0) {
        LONG created;

        if (index >= 0) {
            show_error(data, "A profile with that name already exists.");
            return 0;
        }
        created = VT_ProfileStoreCreate(data->profiles, edited.name);
        if (created < 0) {
            show_error(data, "Could not create the profile.");
            return 0;
        }
        if (!VT_ProfileStoreSave(data->profiles, (ULONG)created, &edited)) {
            VT_ProfileStoreDelete(data->profiles, (ULONG)created);
            show_error(data, "Could not save the profile.");
            return 0;
        }
    } else {
        if (index >= 0 && index != data->edit_index) {
            show_error(data, "A profile with that name already exists.");
            return 0;
        }
        if (!VT_ProfileStoreSave(data->profiles, (ULONG)data->edit_index, &edited)) {
            show_error(data, "Could not save the profile.");
            return 0;
        }
    }

    if (data->main_window) {
        changed.MethodID = MUIM_VT_ProfilesChanged;
        changed.name = edited.name;
        DoMethodA(data->main_window, (Msg)&changed);
    }
    set(obj, MUIA_Window_Open, FALSE);
    return 0;
}

static Object *field_label(const char *text)
{
    return TextObject,
        MUIA_Text_Contents, text,
        MUIA_Text_PreParse, "\33r",
        MUIA_Weight, 0,
        MUIA_Frame, MUIV_Frame_None,
    End;
}

static Object *profile_new(struct IClass *cl, Object *obj, struct opSet *ops)
{
    struct ProfileData *data;
    Object *root;
    Object *name;
    Object *container;
    Object *video;
    Object *scale;
    Object *audio;
    Object *video_bitrate;
    Object *audio_bitrate;
    Object *gain;
    Object *slow_note;
    Object *hint;
    Object *error;
    Object *save;
    Object *cancel;
    VTProfile defaults;
    struct TagItem extra[8];
    ULONG extra_count = 0;

    prepare_container_entries();
    prepare_scale_entries();
    VT_ProfileSetDefaults(&defaults);

    name = StringObject,
        StringFrame,
        MUIA_String_MaxLen, VT_NAME_LEN,
        MUIA_String_Contents, defaults.name,
        MUIA_CycleChain, TRUE,
    End;
    container = CycleObject,
        MUIA_Cycle_Entries, container_entries,
        MUIA_CycleChain, TRUE,
    End;
    video = CycleObject,
        MUIA_Cycle_Entries, cycle_reset,
        MUIA_CycleChain, TRUE,
    End;
    scale = CycleObject,
        MUIA_Cycle_Entries, scale_entries,
        MUIA_CycleChain, TRUE,
    End;
    audio = CycleObject,
        MUIA_Cycle_Entries, cycle_reset,
        MUIA_CycleChain, TRUE,
    End;
    video_bitrate = StringObject,
        StringFrame,
        MUIA_String_MaxLen, 8,
        MUIA_String_Integer, TRUE,
        MUIA_String_Accept, "0123456789",
        MUIA_CycleChain, TRUE,
    End;
    audio_bitrate = StringObject,
        StringFrame,
        MUIA_String_MaxLen, 8,
        MUIA_String_Integer, TRUE,
        MUIA_String_Accept, "0123456789",
        MUIA_CycleChain, TRUE,
    End;
    gain = StringObject,
        StringFrame,
        MUIA_String_MaxLen, 8,
        MUIA_String_Accept, "0123456789-",
        MUIA_CycleChain, TRUE,
    End;
    slow_note = TextObject,
        MUIA_Text_Contents, "H.264 and 1080p are much slower on a G4.",
        MUIA_Text_PreParse, "\33c",
        MUIA_ShowMe, FALSE,
    End;
    hint = TextObject,
        MUIA_Text_Contents, "",
        MUIA_Text_PreParse, "\33l",
    End;
    error = TextObject,
        MUIA_Text_Contents, "",
        MUIA_Text_PreParse, "\33c",
    End;
    save = SimpleButton("_Save");
    cancel = SimpleButton("_Cancel");

    root = VGroup,
        Child, ColGroup(2),
            Child, field_label("Name"),
            Child, name,
            Child, field_label("Container"),
            Child, container,
            Child, field_label("Video"),
            Child, video,
            Child, field_label("Size"),
            Child, scale,
            Child, field_label("Video kbit/s"),
            Child, video_bitrate,
            Child, field_label("Audio"),
            Child, audio,
            Child, field_label("Audio kbit/s"),
            Child, audio_bitrate,
            Child, field_label("Gain dB"),
            Child, gain,
        End,
        Child, slow_note,
        Child, hint,
        Child, error,
        Child, HGroup,
            Child, save,
            Child, cancel,
        End,
    End;
    if (!root)
        return NULL;

    extra[extra_count].ti_Tag = MUIA_Window_Title;
    extra[extra_count++].ti_Data = (IPTR)"Profile";
    extra[extra_count].ti_Tag = MUIA_Window_RootObject;
    extra[extra_count++].ti_Data = (IPTR)root;
    extra[extra_count].ti_Tag = MUIA_Window_LeftEdge;
    extra[extra_count++].ti_Data = MUIV_Window_LeftEdge_Centered;
    extra[extra_count].ti_Tag = MUIA_Window_TopEdge;
    extra[extra_count++].ti_Data = MUIV_Window_TopEdge_Centered;

    obj = VT_SuperNew(cl, obj, ops->ops_AttrList, extra, extra_count);
    if (!obj) {
        MUI_DisposeObject(root);
        return NULL;
    }

    data = INST_DATA(cl, obj);
    data->profiles = (VTProfileStore *)GetTagData(MUIA_VT_Profiles, 0, ops->ops_AttrList);
    data->main_window = NULL;
    data->name = name;
    data->container = container;
    data->video = video;
    data->scale = scale;
    data->audio = audio;
    data->video_bitrate = video_bitrate;
    data->audio_bitrate = audio_bitrate;
    data->gain = gain;
    data->slow_note = slow_note;
    data->hint = hint;
    data->error = error;
    data->edit_index = -1;
    data->updating = TRUE;
    fill_from_profile(data, &defaults);
    data->updating = FALSE;

    DoMethod(container, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_VT_ProfileRefresh);
    DoMethod(video, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_VT_ProfileRefresh);
    DoMethod(scale, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_VT_ProfileRefresh);
    DoMethod(audio, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_VT_ProfileRefresh);
    DoMethod(save, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_VT_ProfileSave);
    DoMethod(cancel, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 1, MUIM_VT_ProfileCancel);
    DoMethod(obj, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             obj, 1, MUIM_VT_ProfileCancel);
    return obj;
}

DISPATCHER(ProfileWindow)
{
    struct ProfileData *data = NULL;

    if (msg->MethodID != OM_NEW)
        data = INST_DATA(cl, obj);
    switch (msg->MethodID) {
    case OM_NEW:
        return (ULONG)profile_new(cl, obj, (struct opSet *)msg);
    case OM_SET: {
        struct TagItem *tag = FindTagItem(MUIA_VT_MainWindow, ((struct opSet *)msg)->ops_AttrList);

        if (tag)
            data->main_window = (Object *)tag->ti_Data;
        return DoSuperMethodA(cl, obj, msg);
    }
    case MUIM_VT_ProfileUse:
        return profile_use(data, obj, (struct MUIP_VT_ProfileUse *)msg);
    case MUIM_VT_ProfileRefresh:
        profile_refresh(data);
        return 0;
    case MUIM_VT_ProfileSave:
        return profile_save(data, obj);
    case MUIM_VT_ProfileCancel:
        set(obj, MUIA_Window_Open, FALSE);
        return 0;
    }
    return DoSuperMethodA(cl, obj, msg);
}
DISPATCHER_END

BOOL VT_ProfileWindow_Init(void)
{
    VT_ProfileWindowClass = MUI_CreateCustomClass(NULL, MUIC_Window, NULL,
                                                  sizeof(struct ProfileData),
                                                  DISPATCHER_REF(ProfileWindow));
    return (BOOL)(VT_ProfileWindowClass != NULL);
}

void VT_ProfileWindow_Exit(void)
{
    if (!VT_ProfileWindowClass)
        return;
    MUI_DeleteCustomClass(VT_ProfileWindowClass);
    VT_ProfileWindowClass = NULL;
}
