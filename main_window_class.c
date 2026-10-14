/*
 * Home window. Buttons call the job and profile models, then the
 * batch list redraws. Start queues work; the encode task reports back
 * through MUIM_VT_EncodeNote on the main task.
 */
#include <emul/emulregs.h>
#include <dos/dos.h>
#include <libraries/gadtools.h>
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
    Object *menu_remove;
    Object *menu_log;
    Object *popup_remove;
    Object *popup_log;
    Object *popup_resume;
    const char *cycle_entries[VT_PROFILE_MAX + 1];
    char status_buf[160];
    char add_drawer[VT_PATH_LEN];
    char job_drawer[VT_PATH_LEN];
    char job_file[VT_PATH_LEN];
    ULONG shown_job;
    BOOL quiet;
};

static const char *cycle_placeholder[] = { "", NULL };

static void update_job_menus(struct MainData *data);
static void show_log(struct MainData *data, const VTJob *job, BOOL open_window);

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
    LONG active = (LONG)xget(data->batch, MUIA_List_Active);

    data->quiet = TRUE;
    DoMethod(data->batch, MUIM_VT_BatchSync);
    if (active >= 0)
        nnset(data->batch, MUIA_List_Active, active);
    data->quiet = FALSE;
    refresh_progress(data);
    update_job_menus(data);
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

static BOOL ask_job_file(struct MainData *data, Object *obj, BOOL save_mode, char *out, ULONG out_len)
{
    struct FileRequester *request;
    struct TagItem ask[4];
    char full[VT_PATH_LEN];

    request = MUI_AllocAslRequestTags(ASL_FileRequest,
                                      ASLFR_TitleText, (ULONG)(save_mode ? "Save job list" : "Open job list"),
                                      ASLFR_DoSaveMode, save_mode,
                                      ASLFR_DoPatterns, TRUE,
                                      ASLFR_InitialPattern, (ULONG)"#?.jobs",
                                      ASLFR_RejectIcons, TRUE,
                                      TAG_DONE);
    if (!request) {
        set_status(data, save_mode ? "Could not save the job list." : "Could not open the job list.");
        return FALSE;
    }
    ask[0].ti_Tag = ASLFR_Window;
    ask[0].ti_Data = (IPTR)_window(obj);
    ask[1].ti_Tag = ASLFR_InitialDrawer;
    ask[1].ti_Data = (IPTR)data->job_drawer;
    ask[2].ti_Tag = ASLFR_InitialFile;
    ask[2].ti_Data = (IPTR)data->job_file;
    ask[3].ti_Tag = TAG_END;
    ask[3].ti_Data = 0;
    if (!MUI_AslRequest(request, ask) || !request->fr_Drawer || !request->fr_File || !request->fr_File[0] ||
        !VT_JoinDrawerFile(request->fr_Drawer, request->fr_File, full, sizeof full)) {
        MUI_FreeAslRequest(request);
        return FALSE;
    }
    if (save_mode && !VT_EndsWithI(full, VT_JOBLIST_SUFFIX)) {
        char with_suffix[VT_PATH_LEN];
        int wrote = snprintf(with_suffix, sizeof with_suffix, "%s%s", full, VT_JOBLIST_SUFFIX);

        if (wrote < 0 || (ULONG)wrote >= sizeof with_suffix) {
            MUI_FreeAslRequest(request);
            set_status(data, "Could not save the job list.");
            return FALSE;
        }
        VT_StrCopy(full, sizeof full, with_suffix);
    }
    VT_StrCopy(data->job_drawer, sizeof data->job_drawer, request->fr_Drawer);
    VT_StrCopy(data->job_file, sizeof data->job_file, request->fr_File);
    if (save_mode && !VT_EndsWithI(data->job_file, VT_JOBLIST_SUFFIX)) {
        char with_suffix[VT_PATH_LEN];
        int wrote = snprintf(with_suffix, sizeof with_suffix, "%s%s", data->job_file, VT_JOBLIST_SUFFIX);

        if (wrote >= 0 && (ULONG)wrote < sizeof with_suffix)
            VT_StrCopy(data->job_file, sizeof data->job_file, with_suffix);
    }
    MUI_FreeAslRequest(request);
    return VT_StrCopy(out, out_len, full);
}

static ULONG main_open_jobs(struct MainData *data, Object *obj)
{
    char path[VT_PATH_LEN];
    LONG choice;

    if (!data->jobs) {
        set_status(data, "The queue is not available.");
        return 0;
    }
    if (VT_Encode_IsRunning()) {
        set_status(data, "Stop the queue before opening a job list.");
        return 0;
    }
    if (!ask_job_file(data, obj, FALSE, path, sizeof path))
        return 0;
    if (data->jobs->count > 0) {
        choice = MUI_RequestA(_app(obj), obj, 0, "Open job list", "Cancel|Replace",
                              "Replace the current job list?", NULL);
        if (choice != 2)
            return 0;
    }
    if (!VT_JobListLoad(data->jobs, path)) {
        set_status(data, "Could not read the job list.");
        return 0;
    }
    data->shown_job = 0;
    refresh_jobs(data);
    data->quiet = TRUE;
    nnset(data->batch, MUIA_List_Active, MUIV_List_Active_Off);
    data->quiet = FALSE;
    update_job_menus(data);
    if (data->job_log && xget(data->job_log, MUIA_Window_Open))
        show_log(data, NULL, FALSE);
    if (data->jobs->count == 0)
        set_status(data, "Opened an empty job list.");
    else if (data->jobs->count == 1)
        set_status(data, "Opened 1 job.");
    else {
        snprintf(data->status_buf, sizeof data->status_buf, "Opened %lu jobs.",
                 (unsigned long)data->jobs->count);
        set_status(data, data->status_buf);
    }
    return 0;
}

static ULONG main_save_jobs(struct MainData *data, Object *obj)
{
    char path[VT_PATH_LEN];
    LONG choice;

    if (!data->jobs) {
        set_status(data, "The queue is not available.");
        return 0;
    }
    if (!ask_job_file(data, obj, TRUE, path, sizeof path))
        return 0;
    if (VT_FileExists(path)) {
        choice = MUI_RequestA(_app(obj), obj, 0, "Save job list", "Cancel|Replace",
                              "Replace the existing file?", NULL);
        if (choice != 2)
            return 0;
    }
    if (!VT_JobListSave(data->jobs, path)) {
        set_status(data, "Could not save the job list.");
        return 0;
    }
    set_status(data, "Job list saved.");
    return 0;
}

static void show_log(struct MainData *data, const VTJob *job, BOOL open_window)
{
    struct MUIP_VT_JobLogUse msg;

    if (!data->job_log)
        return;
    if (!job && open_window)
        return;
    data->shown_job = job ? job->id : 0;
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

static ULONG main_resume(struct MainData *data)
{
    LONG index = selected_job_index(data);
    VTJob *job;

    if (index < 0 || !data->jobs) {
        set_status(data, "Select a job to resume.");
        return 0;
    }
    job = data->jobs->items[index];
    if (job->status != VT_JOB_CANCELLED) {
        set_status(data, "Only a cancelled job can be resumed.");
        return 0;
    }
    job->output_path[0] = '\0';
    VT_JobSetStatus(job, VT_JOB_WAITING);
    refresh_jobs(data);
    refresh_log(data);
    set_status(data, "Job is waiting again.");
    return 0;
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

static ULONG main_job_select(struct MainData *data)
{
    LONG index;
    LONG profile_index;
    const VTJob *job;

    if (data->quiet)
        return 0;
    index = selected_job_index(data);
    if (index < 0 || !data->jobs) {
        update_job_menus(data);
        return 0;
    }
    job = data->jobs->items[index];
    profile_index = VT_ProfileStoreIndex(data->profiles, job->profile.name);
    if (profile_index < 0) {
        update_job_menus(data);
        return 0;
    }
    data->quiet = TRUE;
    nnset(data->profile_cycle, MUIA_Cycle_Active, profile_index);
    data->quiet = FALSE;
    update_job_menus(data);
    return 0;
}

static void update_job_menus(struct MainData *data)
{
    LONG index = selected_job_index(data);
    BOOL has_job = (BOOL)(index >= 0);
    BOOL cancelled = FALSE;

    if (has_job && data->jobs && data->jobs->items[index]->status == VT_JOB_CANCELLED)
        cancelled = TRUE;
    if (data->menu_remove)
        set(data->menu_remove, MUIA_Menuitem_Enabled, has_job);
    if (data->menu_log)
        set(data->menu_log, MUIA_Menuitem_Enabled, has_job);
    if (data->popup_remove)
        set(data->popup_remove, MUIA_Menuitem_Enabled, has_job);
    if (data->popup_log)
        set(data->popup_log, MUIA_Menuitem_Enabled, has_job);
    if (data->popup_resume)
        set(data->popup_resume, MUIA_Menuitem_Enabled, cancelled);
}

static Object *menu_command(const char *title, const char *shortcut, BOOL enabled)
{
    if (shortcut && shortcut[0]) {
        return MenuitemObject,
            MUIA_Menuitem_Title, (IPTR)title,
            MUIA_Menuitem_Shortcut, (IPTR)shortcut,
            MUIA_Menuitem_Enabled, enabled,
        End;
    }
    return MenuitemObject,
        MUIA_Menuitem_Title, (IPTR)title,
        MUIA_Menuitem_Enabled, enabled,
    End;
}

static Object *menu_bar(void)
{
    return (Object *)MUI_MakeObject(MUIO_Menuitem, NM_BARLABEL, 0, 0, 0);
}

static void dispose_objects(Object **items, ULONG count)
{
    ULONG index;

    for (index = 0; index < count; index++) {
        if (items[index])
            MUI_DisposeObject(items[index]);
    }
}

static void bind_item(Object *item, Object *window, ULONG method)
{
    if (item)
        DoMethod(item, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime, window, 1, method);
}

struct WindowMenuItems {
    Object *open;
    Object *save;
    Object *add;
    Object *remove;
    Object *clear;
    Object *start;
    Object *stop;
    Object *about;
    Object *quit;
    Object *new_item;
    Object *edit_item;
    Object *duplicate_item;
    Object *delete_item;
    Object *log;
};

struct PopupMenuItems {
    Object *add;
    Object *remove;
    Object *clear;
    Object *log;
    Object *resume;
    Object *start;
    Object *stop;
};

static Object *build_menustrip(struct WindowMenuItems *items)
{
    Object *bar_list;
    Object *bar_jobs;
    Object *bar_run;
    Object *bar_quit;
    Object *strip;
    Object *owned[20];
    ULONG count = 0;

    memset(items, 0, sizeof *items);
    items->open = menu_command("Open...", "O", TRUE);
    items->save = menu_command("Save...", NULL, TRUE);
    bar_list = menu_bar();
    items->add = menu_command("Add...", "A", TRUE);
    items->remove = menu_command("Remove", "R", FALSE);
    items->clear = menu_command("Clear", "C", TRUE);
    bar_jobs = menu_bar();
    items->start = menu_command("Start", "S", TRUE);
    items->stop = menu_command("Stop", "T", TRUE);
    bar_run = menu_bar();
    items->about = menu_command("About", "?", TRUE);
    bar_quit = menu_bar();
    items->quit = menu_command("Quit", "Q", TRUE);
    items->new_item = menu_command("New", "N", TRUE);
    items->edit_item = menu_command("Edit", "E", TRUE);
    items->duplicate_item = menu_command("Duplicate", "U", TRUE);
    items->delete_item = menu_command("Delete", "D", TRUE);
    items->log = menu_command("Show log", "L", FALSE);
    owned[count++] = items->open;
    owned[count++] = items->save;
    owned[count++] = bar_list;
    owned[count++] = items->add;
    owned[count++] = items->remove;
    owned[count++] = items->clear;
    owned[count++] = bar_jobs;
    owned[count++] = items->start;
    owned[count++] = items->stop;
    owned[count++] = bar_run;
    owned[count++] = items->about;
    owned[count++] = bar_quit;
    owned[count++] = items->quit;
    owned[count++] = items->new_item;
    owned[count++] = items->edit_item;
    owned[count++] = items->duplicate_item;
    owned[count++] = items->delete_item;
    owned[count++] = items->log;
    if (!items->open || !items->save || !bar_list || !items->add || !items->remove ||
        !items->clear || !bar_jobs || !items->start ||
        !items->stop || !bar_run || !items->about || !bar_quit || !items->quit ||
        !items->new_item || !items->edit_item || !items->duplicate_item ||
        !items->delete_item || !items->log) {
        dispose_objects(owned, count);
        memset(items, 0, sizeof *items);
        return NULL;
    }
    strip = MenustripObject,
        Child, MenuObject,
            MUIA_Menu_Title, "Project",
            Child, items->open,
            Child, items->save,
            Child, bar_list,
            Child, items->add,
            Child, items->remove,
            Child, items->clear,
            Child, bar_jobs,
            Child, items->start,
            Child, items->stop,
            Child, bar_run,
            Child, items->about,
            Child, bar_quit,
            Child, items->quit,
        End,
        Child, MenuObject,
            MUIA_Menu_Title, "Profile",
            Child, items->new_item,
            Child, items->edit_item,
            Child, items->duplicate_item,
            Child, items->delete_item,
        End,
        Child, MenuObject,
            MUIA_Menu_Title, "Job",
            Child, items->log,
        End,
    End;
    if (!strip) {
        dispose_objects(owned, count);
        memset(items, 0, sizeof *items);
        return NULL;
    }
    return strip;
}

static Object *build_job_popup(struct PopupMenuItems *items)
{
    Object *bar_log;
    Object *bar_run;
    Object *strip;
    Object *owned[9];
    ULONG count = 0;

    memset(items, 0, sizeof *items);
    items->add = menu_command("Add...", NULL, TRUE);
    items->remove = menu_command("Remove", NULL, FALSE);
    items->clear = menu_command("Clear", NULL, TRUE);
    bar_log = menu_bar();
    items->log = menu_command("Show log", NULL, FALSE);
    items->resume = menu_command("Resume", NULL, FALSE);
    bar_run = menu_bar();
    items->start = menu_command("Start", NULL, TRUE);
    items->stop = menu_command("Stop", NULL, TRUE);
    owned[count++] = items->add;
    owned[count++] = items->remove;
    owned[count++] = items->clear;
    owned[count++] = bar_log;
    owned[count++] = items->log;
    owned[count++] = items->resume;
    owned[count++] = bar_run;
    owned[count++] = items->start;
    owned[count++] = items->stop;
    if (!items->add || !items->remove || !items->clear || !bar_log || !items->log ||
        !items->resume || !bar_run || !items->start || !items->stop) {
        dispose_objects(owned, count);
        memset(items, 0, sizeof *items);
        return NULL;
    }
    strip = MenustripObject,
        Child, MenuObject,
            MUIA_Menu_Title, "Job",
            Child, items->add,
            Child, items->remove,
            Child, items->clear,
            Child, bar_log,
            Child, items->log,
            Child, items->resume,
            Child, bar_run,
            Child, items->start,
            Child, items->stop,
        End,
    End;
    if (!strip) {
        dispose_objects(owned, count);
        memset(items, 0, sizeof *items);
        return NULL;
    }
    return strip;
}

static ULONG main_cycle(struct MainData *data)
{
    const VTProfile *profile;
    LONG index;
    VTJob *job;

    if (data->quiet)
        return 0;
    profile = active_profile(data);
    if (data->prefs && profile) {
        VT_StrCopy(data->prefs->active_profile, sizeof data->prefs->active_profile, profile->name);
        VT_PrefsSave(data->prefs);
    }
    index = selected_job_index(data);
    if (index < 0 || !profile || !data->jobs)
        return 0;
    job = data->jobs->items[index];
    if (job->status != VT_JOB_WAITING || VT_Encode_IsRunning()) {
        LONG profile_index = VT_ProfileStoreIndex(data->profiles, job->profile.name);

        if (profile_index >= 0) {
            data->quiet = TRUE;
            nnset(data->profile_cycle, MUIA_Cycle_Active, profile_index);
            data->quiet = FALSE;
            profile = VT_ProfileStoreAt(data->profiles, (ULONG)profile_index);
            if (data->prefs && profile)
                VT_StrCopy(data->prefs->active_profile, sizeof data->prefs->active_profile,
                           profile->name);
        }
        if (VT_Encode_IsRunning())
            set_status(data, "Stop the queue before changing a profile.");
        else
            set_status(data, "Only a waiting job can change profile.");
        return 0;
    }
    if (!VT_JobSetProfile(job, profile)) {
        set_status(data, "That profile cannot be used for this job.");
        return 0;
    }
    refresh_jobs(data);
    set_status(data, "Profile updated.");
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
    Object *menustrip;
    Object *popup;
    struct WindowMenuItems window_items;
    struct PopupMenuItems popup_items;
    struct TagItem extra[8];
    ULONG extra_count = 0;

    memset(&window_items, 0, sizeof window_items);
    memset(&popup_items, 0, sizeof popup_items);
    menustrip = build_menustrip(&window_items);
    popup = build_job_popup(&popup_items);
    prefs = (VTPrefs *)GetTagData(MUIA_VT_Prefs, 0, ops->ops_AttrList);
    batch = NewObject(VT_BatchListClass->mcc_Class, NULL,
                      MUIA_VT_Jobs, GetTagData(MUIA_VT_Jobs, 0, ops->ops_AttrList),
                      TAG_DONE);
    if (!batch) {
        if (menustrip)
            MUI_DisposeObject(menustrip);
        if (popup)
            MUI_DisposeObject(popup);
        return NULL;
    }

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
    add_button = SimpleButton("Add");
    remove_button = SimpleButton("Remove");
    clear_button = SimpleButton("Clear");
    start_button = SimpleButton("Start");
    stop_button = SimpleButton("Stop");
    new_button = SimpleButton("New");
    edit_button = SimpleButton("Edit");
    duplicate_button = SimpleButton("Duplicate");
    delete_button = SimpleButton("Delete");

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
            Child, RectangleObject, MUIA_Weight, 100, End,
            Child, start_button,
            Child, stop_button,
        End,
        Child, gauge,
        Child, status,
    End;
    if (!root) {
        MUI_DisposeObject(batch);
        if (menustrip)
            MUI_DisposeObject(menustrip);
        if (popup)
            MUI_DisposeObject(popup);
        return NULL;
    }
    if (popup)
        set(batch, MUIA_ContextMenu, popup);

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
    if (menustrip) {
        extra[extra_count].ti_Tag = MUIA_Window_Menustrip;
        extra[extra_count++].ti_Data = (IPTR)menustrip;
    }

    obj = VT_SuperNew(cl, obj, ops->ops_AttrList, extra, extra_count);
    if (!obj) {
        MUI_DisposeObject(root);
        if (menustrip)
            MUI_DisposeObject(menustrip);
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
    data->menu_remove = window_items.remove;
    data->menu_log = window_items.log;
    data->popup_remove = popup_items.remove;
    data->popup_log = popup_items.log;
    data->popup_resume = popup_items.resume;
    data->quiet = FALSE;
    data->add_drawer[0] = '\0';
    data->job_drawer[0] = '\0';
    VT_StrCopy(data->job_file, sizeof data->job_file, "Jobs.jobs");
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
    DoMethod(profile_cycle, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             obj, 1, MUIM_VT_ProfileCycle);
    DoMethod(batch, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             obj, 1, MUIM_VT_JobSelect);
    DoMethod(drawer_string, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime,
             obj, 1, MUIM_VT_DrawerChanged);
    DoMethod(obj, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, obj, 1, MUIM_VT_Close);
    DoMethod(obj, MUIM_Notify, MUIA_AppMessage, MUIV_EveryTime,
             obj, 2, MUIM_VT_Drop, MUIV_TriggerValue);
    bind_item(window_items.open, obj, MUIM_VT_OpenJobs);
    bind_item(window_items.save, obj, MUIM_VT_SaveJobs);
    bind_item(window_items.add, obj, MUIM_VT_AddFiles);
    bind_item(window_items.remove, obj, MUIM_VT_RemoveJob);
    bind_item(window_items.clear, obj, MUIM_VT_ClearJobs);
    bind_item(window_items.start, obj, MUIM_VT_Start);
    bind_item(window_items.stop, obj, MUIM_VT_Stop);
    bind_item(window_items.about, obj, MUIM_VT_ShowAbout);
    bind_item(window_items.quit, obj, MUIM_VT_Close);
    bind_item(window_items.new_item, obj, MUIM_VT_NewProfile);
    bind_item(window_items.edit_item, obj, MUIM_VT_EditProfile);
    bind_item(window_items.duplicate_item, obj, MUIM_VT_DuplicateProfile);
    bind_item(window_items.delete_item, obj, MUIM_VT_DeleteProfile);
    bind_item(window_items.log, obj, MUIM_VT_ShowLog);
    bind_item(popup_items.add, obj, MUIM_VT_AddFiles);
    bind_item(popup_items.remove, obj, MUIM_VT_RemoveJob);
    bind_item(popup_items.clear, obj, MUIM_VT_ClearJobs);
    bind_item(popup_items.log, obj, MUIM_VT_ShowLog);
    bind_item(popup_items.resume, obj, MUIM_VT_ResumeJob);
    bind_item(popup_items.start, obj, MUIM_VT_Start);
    bind_item(popup_items.stop, obj, MUIM_VT_Stop);
    update_job_menus(data);
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
    case MUIM_VT_OpenJobs:
        return main_open_jobs(data, obj);
    case MUIM_VT_SaveJobs:
        return main_save_jobs(data, obj);
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
    case MUIM_VT_ResumeJob:
        return main_resume(data);
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
    case MUIM_VT_JobSelect:
        return main_job_select(data);
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
