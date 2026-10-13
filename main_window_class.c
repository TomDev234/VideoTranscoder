/*
 * Home window. Buttons call the job and profile models, then the
 * batch list redraws. Start queues work; the encode task reports back
 * through MUIM_VT_EncodeNote on the main task.
 */
#include <emul/emulregs.h>
#include <dos/dos.h>
#include <workbench/workbench.h>
#include <libraries/asl.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <string.h>

#include "macros.h"
#include "vt_ui.h"
#include "job.h"
#include "prefs.h"
#include "vt_util.h"
#include "encode_task.h"

struct MUI_CustomClass *VT_MainWindowClass;

struct MainData {
    VTJobList *jobs;
    VTProfileStore *profiles;
    VTPrefs *prefs;
    Object *profile_window;
    Object *job_log;
    Object *about;
    Object *batch;
    Object *drawer_string;
    Object *profile_cycle;
    Object *gauge;
    Object *status;
    const char *cycle_entries[VT_PROFILE_MAX + 1];
    char status_buf[160];
    char add_drawer[VT_PATH_LEN];
    ULONG shown_job;
    BOOL quiet;
};

static const char *cycle_placeholder[] = { "", NULL };

static void set_status(struct MainData *data, const char *text)
{
    if (text != data->status_buf)
        VT_StrCopyTrunc(data->status_buf, sizeof data->status_buf, text ? text : "");
    set(data->status, MUIA_Text_Contents, data->status_buf);
}

static void refresh_progress(struct MainData *data)
{
    ULONG index;
    LONG total = 0;

    if (!data->jobs || data->jobs->count == 0) {
        set(data->gauge, MUIA_Gauge_Current, 0);
        return;
    }
    for (index = 0; index < data->jobs->count; index++)
        total += data->jobs->items[index]->progress;
    set(data->gauge, MUIA_Gauge_Current, total / (LONG)data->jobs->count);
}

static void refresh_jobs(struct MainData *data)
{
    DoMethod(data->batch, MUIM_VT_BatchSync);
    refresh_progress(data);
}

static const VTProfile *active_profile(struct MainData *data)
{
    LONG active;

    if (!data->profiles || data->profiles->count == 0)
        return NULL;
    active = (LONG)xget(data->profile_cycle, MUIA_Cycle_Active);
    if (active < 0 || (ULONG)active >= data->profiles->count)
        active = 0;
    return VT_ProfileStoreAt(data->profiles, (ULONG)active);
}

static void rebuild_cycle(struct MainData *data, const char *select_name)
{
    ULONG index;
    LONG active = 0;
    BOOL found = FALSE;
    const VTProfile *selected;

    if (!data->profiles)
        return;
    if (!select_name || !select_name[0])
        select_name = data->prefs ? data->prefs->active_profile : NULL;
    for (index = 0; index < data->profiles->count && index < VT_PROFILE_MAX; index++) {
        const VTProfile *profile = VT_ProfileStoreAt(data->profiles, index);

        data->cycle_entries[index] = profile ? profile->name : "";
        if (select_name && profile && VT_StrEqI(profile->name, select_name)) {
            active = (LONG)index;
            found = TRUE;
        }
    }
    if (index == 0) {
        data->cycle_entries[0] = "";
        index = 1;
        active = 0;
    }
    data->cycle_entries[index] = NULL;
    if (!found)
        active = 0;
    data->quiet = TRUE;
    set(data->profile_cycle, MUIA_Cycle_Entries, cycle_placeholder);
    set(data->profile_cycle, MUIA_Cycle_Entries, data->cycle_entries);
    nnset(data->profile_cycle, MUIA_Cycle_Active, active);
    data->quiet = FALSE;
    selected = VT_ProfileStoreAt(data->profiles, (ULONG)active);
    if (data->prefs && selected)
        VT_StrCopy(data->prefs->active_profile, sizeof data->prefs->active_profile, selected->name);
}

static void store_prefs(struct MainData *data, Object *obj)
{
    const char *drawer;

    if (!data->prefs)
        return;
    drawer = (const char *)xget(data->drawer_string, MUIA_String_Contents);
    VT_StrCopy(data->prefs->output_drawer, sizeof data->prefs->output_drawer, drawer ? drawer : "");
    data->prefs->window_left = (LONG)xget(obj, MUIA_Window_LeftEdge);
    data->prefs->window_top = (LONG)xget(obj, MUIA_Window_TopEdge);
    data->prefs->window_width = (LONG)xget(obj, MUIA_Window_Width);
    data->prefs->window_height = (LONG)xget(obj, MUIA_Window_Height);
    data->prefs->window_valid = (BOOL)(data->prefs->window_width > 80 && data->prefs->window_height > 60);
    VT_PrefsSave(data->prefs);
}

static int path_kind(const char *path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int kind = 0;

    if (!path || !path[0])
        return 0;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock)
        return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) {
        if (fib->fib_DirEntryType < 0)
            kind = 1;
        else if (fib->fib_DirEntryType > 0)
            kind = 2;
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return kind;
}

static BOOL join_arg(const struct WBArg *arg, char *out, ULONG out_len)
{
    char drawer[VT_PATH_LEN];

    if (!arg || !arg->wa_Name || !arg->wa_Name[0])
        return FALSE;
    if (strchr(arg->wa_Name, ':'))
        return VT_StrCopy(out, out_len, arg->wa_Name);
    if (!arg->wa_Lock || !NameFromLock(arg->wa_Lock, drawer, (LONG)sizeof drawer))
        return FALSE;
    return VT_JoinDrawerFile(drawer, arg->wa_Name, out, out_len);
}

static void queue_path(struct MainData *data, const char *path, ULONG *added)
{
    const VTProfile *profile;

    if (!path || VT_EndsWithI(path, ".info"))
        return;
    if (path_kind(path) != 1)
        return;
    profile = active_profile(data);
    if (!profile)
        return;
    if (VT_JobListAdd(data->jobs, path, profile))
        (*added)++;
}

static void queue_drawer(struct MainData *data, const char *drawer, ULONG *added)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    char path[VT_PATH_LEN];

    if (!drawer || !drawer[0])
        return;
    lock = Lock((STRPTR)drawer, ACCESS_READ);
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (lock && fib && Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            if (fib->fib_DirEntryType > 0)
                continue;
            if (!VT_JoinDrawerFile(drawer, fib->fib_FileName, path, sizeof path))
                continue;
            queue_path(data, path, added);
        }
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (lock)
        UnLock(lock);
}

static void report_added(struct MainData *data, ULONG added)
{
    refresh_jobs(data);
    if (added == 0)
        set_status(data, "No files were added.");
    else if (added == 1)
        set_status(data, "Added 1 file.");
    else {
        snprintf(data->status_buf, sizeof data->status_buf, "Added %lu files.", (unsigned long)added);
        set_status(data, data->status_buf);
    }
}

static ULONG main_add(struct MainData *data, Object *obj)
{
    struct FileRequester *request;
    struct TagItem ask[3];
    ULONG added = 0;
    char full[VT_PATH_LEN];

    if (!data->jobs) {
        set_status(data, "The queue is not available.");
        return 0;
    }
    if (!active_profile(data)) {
        set_status(data, "Choose a profile.");
        return 0;
    }
    request = MUI_AllocAslRequestTags(ASL_FileRequest,
                                      ASLFR_TitleText, (ULONG)"Add videos",
                                      ASLFR_DoMultiSelect, TRUE,
                                      ASLFR_RejectIcons, TRUE,
                                      ASLFR_DoPatterns, TRUE,
                                      TAG_DONE);
    if (!request) {
        set_status(data, "Could not open the file requester.");
        return 0;
    }
    ask[0].ti_Tag = ASLFR_Window;
    ask[0].ti_Data = (IPTR)_window(obj);
    ask[1].ti_Tag = ASLFR_InitialDrawer;
    ask[1].ti_Data = (IPTR)data->add_drawer;
    ask[2].ti_Tag = TAG_END;
    ask[2].ti_Data = 0;
    if (!MUI_AslRequest(request, ask)) {
        MUI_FreeAslRequest(request);
        return 0;
    }
    if (request->fr_Drawer)
        VT_StrCopy(data->add_drawer, sizeof data->add_drawer, request->fr_Drawer);

    if (request->fr_NumArgs > 0 && request->fr_ArgList) {
        LONG index;

        for (index = 0; index < request->fr_NumArgs; index++) {
            if (!join_arg(&request->fr_ArgList[index], full, sizeof full))
                continue;
            queue_path(data, full, &added);
        }
    } else if (request->fr_File && request->fr_File[0] &&
               VT_JoinDrawerFile(request->fr_Drawer, request->fr_File, full, sizeof full)) {
        queue_path(data, full, &added);
    }
    MUI_FreeAslRequest(request);
    report_added(data, added);
    return 0;
}

struct MUIP_VT_Drop {
    ULONG MethodID;
    struct AppMessage *message;
};

static ULONG main_drop(struct MainData *data, struct AppMessage *message)
{
    ULONG added = 0;
    LONG index;
    char full[VT_PATH_LEN];
    char drawer[VT_PATH_LEN];

    if (!data->jobs) {
        set_status(data, "The queue is not available.");
        return 0;
    }
    if (!active_profile(data)) {
        set_status(data, "Choose a profile.");
        return 0;
    }
    if (!message || message->am_NumArgs <= 0 || !message->am_ArgList) {
        set_status(data, "No files were added.");
        return 0;
    }
    for (index = 0; index < message->am_NumArgs; index++) {
        const struct WBArg *arg = &message->am_ArgList[index];

        if (arg->wa_Name && arg->wa_Name[0]) {
            if (!join_arg(arg, full, sizeof full))
                continue;
            if (path_kind(full) == 2)
                queue_drawer(data, full, &added);
            else
                queue_path(data, full, &added);
        } else if (arg->wa_Lock && NameFromLock(arg->wa_Lock, drawer, (LONG)sizeof drawer)) {
            queue_drawer(data, drawer, &added);
        }
    }
    report_added(data, added);
    return 0;
}

static void show_log(struct MainData *data, const VTJob *job, BOOL open_window)
{
    struct MUIP_VT_JobLogUse msg;

    if (!data->job_log || !job)
        return;
    data->shown_job = job->id;
    msg.MethodID = MUIM_VT_JobLogUse;
    msg.job = (const struct VTJob *)job;
    msg.open_window = open_window;
    DoMethodA(data->job_log, (Msg)&msg);
}

static void refresh_log(struct MainData *data)
{
    VTJob *job;

    if (!data->job_log || !data->shown_job || !data->jobs)
        return;
    if (!xget(data->job_log, MUIA_Window_Open))
        return;
    job = VT_JobListFind(data->jobs, data->shown_job);
    if (job)
        show_log(data, job, FALSE);
}

static LONG selected_job_index(struct MainData *data);

static ULONG main_show_log(struct MainData *data)
{
    LONG index;

    if (!data->job_log) {
        set_status(data, "The job log is not available.");
        return 0;
    }
    index = selected_job_index(data);
    if (index < 0 || !data->jobs) {
        set_status(data, "Select a job first.");
        return 0;
    }
    show_log(data, data->jobs->items[index], TRUE);
    return 0;
}

static ULONG main_show_about(struct MainData *data)
{
    if (!data->about) {
        set_status(data, "The about window is not available.");
        return 0;
    }
    set(data->about, MUIA_Window_Open, TRUE);
    return 0;
}

static LONG selected_job_index(struct MainData *data)
{
    VTJob *job;
    ULONG index;

    job = (VTJob *)DoMethod(data->batch, MUIM_VT_BatchGetActive);
    if (!job || !data->jobs)
        return -1;
    for (index = 0; index < data->jobs->count; index++) {
        if (data->jobs->items[index] == job)
            return (LONG)index;
    }
    return -1;
}

static ULONG main_remove(struct MainData *data)
{
    LONG index = selected_job_index(data);

    if (index < 0) {
        set_status(data, "Select a job to remove.");
        return 0;
    }
    if (!VT_JobListRemove(data->jobs, (ULONG)index)) {
        set_status(data, "A running job stays in the queue.");
        return 0;
    }
    refresh_jobs(data);
    set_status(data, "Removed the selected job.");
    return 0;
}

static ULONG main_clear(struct MainData *data)
{
    ULONG removed = VT_JobListClear(data->jobs);

    refresh_jobs(data);
    if (data->jobs->count > 0)
        set_status(data, "Cleared the idle jobs. The running job stayed.");
    else if (removed == 0)
        set_status(data, "The queue is already empty.");
    else
        set_status(data, "Queue cleared.");
    return 0;
}

static ULONG main_start(struct MainData *data)
{
    const char *drawer;

    drawer = (const char *)xget(data->drawer_string, MUIA_String_Contents);
    if (!drawer || !drawer[0]) {
        set_status(data, "Choose an output drawer first.");
        return 0;
    }
    if (!data->jobs || data->jobs->count == 0 ||
        VT_JobListFindStatus(data->jobs, VT_JOB_WAITING) < 0) {
        set_status(data, "The queue has no waiting files.");
        return 0;
    }
    if (!active_profile(data)) {
        set_status(data, "Choose a profile.");
        return 0;
    }
    if (!VT_Encode_IsReady()) {
        set_status(data, "The encoder did not start.");
        return 0;
    }
    if (VT_Encode_IsRunning()) {
        set_status(data, "Encoding is already running.");
        return 0;
    }
    if (data->prefs)
        VT_StrCopy(data->prefs->output_drawer, sizeof data->prefs->output_drawer, drawer);
    if (!VT_Encode_Submit(data->jobs, drawer)) {
        set_status(data, "Could not queue the files.");
        return 0;
    }
    set_status(data, "Encoding.");
    return 0;
}

static ULONG main_stop(struct MainData *data)
{
    if (!VT_Encode_Stop()) {
        set_status(data, "Nothing is running.");
        return 0;
    }
    set_status(data, "Stopping.");
    return 0;
}

static ULONG main_profile_use(struct MainData *data, LONG index)
{
    struct MUIP_VT_ProfileUse msg;

    if (!data->profile_window) {
        set_status(data, "The profile editor is not available.");
        return 0;
    }
    msg.MethodID = MUIM_VT_ProfileUse;
    msg.index = index;
    DoMethodA(data->profile_window, (Msg)&msg);
    return 0;
}

static ULONG main_delete_profile(struct MainData *data, Object *obj)
{
    LONG active;
    const VTProfile *profile;
    IPTR args[1];
    LONG choice;

    active = (LONG)xget(data->profile_cycle, MUIA_Cycle_Active);
    profile = (active >= 0) ? VT_ProfileStoreAt(data->profiles, (ULONG)active) : NULL;
    if (!profile) {
        set_status(data, "Choose a profile.");
        return 0;
    }
    args[0] = (IPTR)profile->name;
    choice = MUI_RequestA(_app(obj), obj, 0, "Delete profile", "Cancel|Delete",
                          "Delete profile \"%s\"?", (APTR)args);
    if (choice != 2)
        return 0;
    if (!VT_ProfileStoreDelete(data->profiles, (ULONG)active)) {
        set_status(data, "Could not delete the profile.");
        return 0;
    }
    rebuild_cycle(data, NULL);
    VT_PrefsSave(data->prefs);
    set_status(data, "Profile deleted.");
    return 0;
}

static ULONG main_duplicate_profile(struct MainData *data)
{
    LONG active;
    LONG created;
    const VTProfile *profile;

    active = (LONG)xget(data->profile_cycle, MUIA_Cycle_Active);
    if (active < 0) {
        set_status(data, "Choose a profile.");
        return 0;
    }
    created = VT_ProfileStoreDuplicate(data->profiles, (ULONG)active);
    if (created < 0) {
        set_status(data, "Could not duplicate the profile.");
        return 0;
    }
    profile = VT_ProfileStoreAt(data->profiles, (ULONG)created);
    rebuild_cycle(data, profile ? profile->name : NULL);
    VT_PrefsSave(data->prefs);
    set_status(data, "Profile duplicated.");
    return 0;
}

static ULONG main_profiles_changed(struct MainData *data, struct MUIP_VT_ProfilesChanged *msg)
{
    rebuild_cycle(data, msg->name);
    VT_PrefsSave(data->prefs);
    set_status(data, "Profile saved.");
    return 0;
}

static ULONG main_cycle(struct MainData *data)
{
    const VTProfile *profile;

    if (data->quiet)
        return 0;
    profile = active_profile(data);
    if (data->prefs && profile) {
        VT_StrCopy(data->prefs->active_profile, sizeof data->prefs->active_profile, profile->name);
        VT_PrefsSave(data->prefs);
    }
    return 0;
}

static Object *main_new(struct IClass *cl, Object *obj, struct opSet *ops)
{
    struct MainData *data;
    VTPrefs *prefs;
    Object *batch;
    Object *drawer_string;
    Object *drawer_pop;
    Object *profile_cycle;
    Object *gauge;
    Object *status;
    Object *root;
    Object *add_button;
    Object *remove_button;
    Object *clear_button;
    Object *start_button;
    Object *stop_button;
    Object *new_button;
    Object *edit_button;
    Object *duplicate_button;
    Object *delete_button;
    Object *log_button;
    Object *about_button;
    struct TagItem extra[8];
    ULONG extra_count = 0;

    prefs = (VTPrefs *)GetTagData(MUIA_VT_Prefs, 0, ops->ops_AttrList);
    batch = NewObject(VT_BatchListClass->mcc_Class, NULL,
                      MUIA_VT_Jobs, GetTagData(MUIA_VT_Jobs, 0, ops->ops_AttrList),
                      TAG_DONE);
    if (!batch)
        return NULL;

    drawer_string = StringObject,
        StringFrame,
        MUIA_String_MaxLen, VT_PATH_LEN,
        MUIA_String_Contents, prefs ? prefs->output_drawer : "",
        MUIA_CycleChain, TRUE,
    End;
    drawer_pop = PopaslObject,
        MUIA_Popstring_String, drawer_string,
        MUIA_Popstring_Button, PopButton(MUII_PopDrawer),
        MUIA_Popasl_Type, ASL_FileRequest,
        ASLFR_DrawersOnly, TRUE,
        ASLFR_TitleText, "Output drawer",
    End;
    profile_cycle = CycleObject,
        MUIA_Cycle_Entries, cycle_placeholder,
        MUIA_CycleChain, TRUE,
    End;
    gauge = GaugeObject,
        GaugeFrame,
        MUIA_Gauge_Horiz, TRUE,
        MUIA_Gauge_Max, 100,
        MUIA_Gauge_Current, 0,
        MUIA_Gauge_InfoText, "%ld %%",
    End;
    status = TextObject,
        MUIA_Text_Contents, "Add files, choose a profile and an output drawer.",
        MUIA_Text_PreParse, "\33l",
    End;
    add_button = SimpleButton("_Add");
    remove_button = SimpleButton("_Remove");
    clear_button = SimpleButton("_Clear");
    start_button = SimpleButton("_Start");
    stop_button = SimpleButton("S_top");
    new_button = SimpleButton("_New");
    edit_button = SimpleButton("_Edit");
    duplicate_button = SimpleButton("D_uplicate");
    delete_button = SimpleButton("De_lete");
    log_button = SimpleButton("_Log");
    about_button = SimpleButton("A_bout");

    root = VGroup,
        Child, HGroup,
            Child, TextObject,
                MUIA_Text_Contents, "Output",
                MUIA_Text_PreParse, "\33r",
                MUIA_Weight, 0,
                MUIA_Frame, MUIV_Frame_None,
            End,
            Child, drawer_pop,
        End,
        Child, HGroup,
            Child, TextObject,
                MUIA_Text_Contents, "Profile",
                MUIA_Text_PreParse, "\33r",
                MUIA_Weight, 0,
                MUIA_Frame, MUIV_Frame_None,
            End,
            Child, profile_cycle,
            Child, new_button,
            Child, edit_button,
            Child, duplicate_button,
            Child, delete_button,
        End,
        Child, ListviewObject,
            MUIA_Listview_List, batch,
            MUIA_Weight, 100,
        End,
        Child, HGroup,
            Child, add_button,
            Child, remove_button,
            Child, clear_button,
            Child, log_button,
            Child, about_button,
            Child, RectangleObject, MUIA_Weight, 100, End,
            Child, start_button,
            Child, stop_button,
        End,
        Child, gauge,
        Child, status,
    End;
    if (!root) {
        MUI_DisposeObject(batch);
        return NULL;
    }

    extra[extra_count].ti_Tag = MUIA_Window_Title;
    extra[extra_count++].ti_Data = (IPTR)"VideoTranscoder";
    extra[extra_count].ti_Tag = MUIA_Window_RootObject;
    extra[extra_count++].ti_Data = (IPTR)root;
    extra[extra_count].ti_Tag = MUIA_Window_Width;
    extra[extra_count++].ti_Data = 720;
    extra[extra_count].ti_Tag = MUIA_Window_Height;
    extra[extra_count++].ti_Data = 480;
    extra[extra_count].ti_Tag = MUIA_Window_AppWindow;
    extra[extra_count++].ti_Data = TRUE;

    obj = VT_SuperNew(cl, obj, ops->ops_AttrList, extra, extra_count);
    if (!obj) {
        MUI_DisposeObject(root);
        return NULL;
    }

    data = INST_DATA(cl, obj);
    data->jobs = (VTJobList *)GetTagData(MUIA_VT_Jobs, 0, ops->ops_AttrList);
    data->profiles = (VTProfileStore *)GetTagData(MUIA_VT_Profiles, 0, ops->ops_AttrList);
    data->prefs = prefs;
    data->profile_window = (Object *)GetTagData(MUIA_VT_ProfileWindow, 0, ops->ops_AttrList);
    data->job_log = (Object *)GetTagData(MUIA_VT_JobLogWindow, 0, ops->ops_AttrList);
    data->about = (Object *)GetTagData(MUIA_VT_AboutWindow, 0, ops->ops_AttrList);
    data->batch = batch;
    data->drawer_string = drawer_string;
    data->profile_cycle = profile_cycle;
    data->gauge = gauge;
    data->status = status;
    data->quiet = FALSE;
    data->add_drawer[0] = '\0';
    data->shown_job = 0;
    VT_StrCopyTrunc(data->status_buf, sizeof data->status_buf,
                    "Add files, choose a profile and an output drawer.");
    if (prefs && prefs->window_valid && prefs->window_width > 80 && prefs->window_height > 60) {
        set(obj, MUIA_Window_LeftEdge, prefs->window_left);
        set(obj, MUIA_Window_TopEdge, prefs->window_top);
        set(obj, MUIA_Window_Width, prefs->window_width);
        set(obj, MUIA_Window_Height, prefs->window_height);
    }
    rebuild_cycle(data, prefs ? prefs->active_profile : NULL);

    DoMethod(add_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_AddFiles);
    DoMethod(remove_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_RemoveJob);
    DoMethod(clear_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_ClearJobs);
    DoMethod(start_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_Start);
    DoMethod(stop_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_Stop);
    DoMethod(new_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_NewProfile);
    DoMethod(edit_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_EditProfile);
    DoMethod(duplicate_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_DuplicateProfile);
    DoMethod(delete_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_DeleteProfile);
    DoMethod(log_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_ShowLog);
    DoMethod(about_button, MUIM_Notify, MUIA_Pressed, FALSE, obj, 1, MUIM_VT_ShowAbout);
    DoMethod(profile_cycle, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_VT_ProfileCycle);
    DoMethod(drawer_string, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime,
             obj, 1, MUIM_VT_DrawerChanged);
    DoMethod(obj, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, obj, 1, MUIM_VT_Close);
    DoMethod(obj, MUIM_Notify, MUIA_AppMessage, MUIV_EveryTime,
             obj, 2, MUIM_VT_Drop, MUIV_TriggerValue);
    return obj;
}

DISPATCHER(MainWindow)
{
    struct MainData *data = NULL;

    if (msg->MethodID != OM_NEW)
        data = INST_DATA(cl, obj);
    switch (msg->MethodID) {
    case OM_NEW:
        return (ULONG)main_new(cl, obj, (struct opSet *)msg);
    case MUIM_VT_AddFiles:
        return main_add(data, obj);
    case MUIM_VT_Drop:
        return main_drop(data, ((struct MUIP_VT_Drop *)msg)->message);
    case MUIM_VT_ShowLog:
        return main_show_log(data);
    case MUIM_VT_ShowAbout:
        return main_show_about(data);
    case MUIM_VT_RemoveJob:
        return main_remove(data);
    case MUIM_VT_ClearJobs:
        return main_clear(data);
    case MUIM_VT_Start:
        return main_start(data);
    case MUIM_VT_Stop:
        return main_stop(data);
    case MUIM_VT_EncodeNote: {
        struct MUIP_VT_EncodeNote *note = (struct MUIP_VT_EncodeNote *)msg;

        if (note->status && note->status[0])
            set_status(data, note->status);
        refresh_jobs(data);
        refresh_log(data);
        return 0;
    }
    case MUIM_VT_NewProfile:
        return main_profile_use(data, -1);
    case MUIM_VT_EditProfile: {
        LONG active = (LONG)xget(data->profile_cycle, MUIA_Cycle_Active);

        if (active < 0) {
            set_status(data, "Choose a profile.");
            return 0;
        }
        return main_profile_use(data, active);
    }
    case MUIM_VT_DuplicateProfile:
        return main_duplicate_profile(data);
    case MUIM_VT_DeleteProfile:
        return main_delete_profile(data, obj);
    case MUIM_VT_DrawerChanged: {
        const char *drawer = (const char *)xget(data->drawer_string, MUIA_String_Contents);

        if (data->prefs)
            VT_StrCopy(data->prefs->output_drawer, sizeof data->prefs->output_drawer,
                       drawer ? drawer : "");
        return 0;
    }
    case MUIM_VT_ProfileCycle:
        return main_cycle(data);
    case MUIM_VT_ProfilesChanged:
        return main_profiles_changed(data, (struct MUIP_VT_ProfilesChanged *)msg);
    case MUIM_VT_SavePrefs:
        store_prefs(data, obj);
        return 0;
    case MUIM_VT_Close:
        store_prefs(data, obj);
        if (_app(obj))
            DoMethod(_app(obj), MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
        return 0;
    }
    return DoSuperMethodA(cl, obj, msg);
}
DISPATCHER_END

BOOL VT_MainWindow_Init(void)
{
    VT_MainWindowClass = MUI_CreateCustomClass(NULL, MUIC_Window, NULL,
                                               sizeof(struct MainData),
                                               DISPATCHER_REF(MainWindow));
    return (BOOL)(VT_MainWindowClass != NULL);
}

void VT_MainWindow_Exit(void)
{
    if (!VT_MainWindowClass)
        return;
    MUI_DeleteCustomClass(VT_MainWindowClass);
    VT_MainWindowClass = NULL;
}
