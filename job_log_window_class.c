/*
 * Read-only probe and error view for one job. The main task copies
 * the fields; this class does not call libav or keep the job pointer.
 */
#include <emul/emulregs.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>

#include <stdio.h>

#include "macros.h"
#include "vt_ui.h"
#include "job.h"

struct MUI_CustomClass *VT_JobLogWindowClass;

struct JobLogData {
    Object *body;
    char text[4096];
};

static void format_duration(const VTJob *job, char *out, ULONG out_len)
{
    long long seconds;
    long hours;
    long minutes;
    long secs;

    if (!job->has_probe) {
        snprintf(out, out_len, "not probed yet");
        return;
    }
    seconds = job->duration_us / 1000000LL;
    if (seconds < 0)
        seconds = 0;
    hours = (long)(seconds / 3600);
    minutes = (long)((seconds / 60) % 60);
    secs = (long)(seconds % 60);
    snprintf(out, out_len, "%ld:%02ld:%02ld", hours, minutes, secs);
}

static void format_size(const VTJob *job, char *out, ULONG out_len)
{
    unsigned long long bytes;

    if (!job->has_probe) {
        snprintf(out, out_len, "not probed yet");
        return;
    }
    bytes = job->size_bytes;
    if (bytes >= 1024ULL * 1024ULL)
        snprintf(out, out_len, "%lu MB", (unsigned long)(bytes / (1024ULL * 1024ULL)));
    else if (bytes >= 1024ULL)
        snprintf(out, out_len, "%lu KB", (unsigned long)(bytes / 1024ULL));
    else
        snprintf(out, out_len, "%lu bytes", (unsigned long)bytes);
}

static void fill_log(struct JobLogData *data, const VTJob *job)
{
    char duration[32];
    char size[32];
    const char *video;
    const char *audio;
    const char *output;
    const char *error;

    if (!job) {
        snprintf(data->text, sizeof data->text, "No job selected.");
        set(data->body, MUIA_Text_Contents, data->text);
        return;
    }
    format_duration(job, duration, sizeof duration);
    format_size(job, size, sizeof size);
    video = job->has_probe ? (job->probe_video[0] ? job->probe_video : "(none)") : "not probed yet";
    audio = job->has_probe ? (job->probe_audio[0] ? job->probe_audio : "(none)") : "not probed yet";
    output = job->output_path[0] ? job->output_path : "(not chosen yet)";
    error = job->error[0] ? job->error : "(none)";
    snprintf(data->text, sizeof data->text,
             "Source: %s\n"
             "Path: %s\n"
             "Profile: %s\n"
             "Status: %s\n"
             "Progress: %ld%%\n"
             "Output: %s\n"
             "\n"
             "Duration: %s\n"
             "Video: %s\n"
             "Audio: %s\n"
             "Size: %s\n"
             "\n"
             "Error: %s\n",
             job->source_name,
             job->source_path,
             job->profile.name,
             VT_JobStatusLabel(job->status),
             (long)job->progress,
             output,
             duration,
             video,
             audio,
             size,
             error);
    set(data->body, MUIA_Text_Contents, data->text);
}

static Object *log_new(struct IClass *cl, Object *obj, struct opSet *ops)
{
    struct JobLogData *data;
    Object *root;
    Object *body;
    Object *close_button;
    struct TagItem extra[6];
    ULONG extra_count = 0;

    body = TextObject,
        MUIA_Text_Contents, "",
        MUIA_Text_PreParse, "\33l",
    End;
    close_button = SimpleButton("_Close");
    root = VGroup,
        Child, ScrollgroupObject,
            MUIA_Weight, 100,
            MUIA_Scrollgroup_Contents, VirtgroupObject,
                MUIA_Frame, MUIV_Frame_None,
                Child, body,
            End,
        End,
        Child, close_button,
    End;
    if (!root)
        return NULL;

    extra[extra_count].ti_Tag = MUIA_Window_Title;
    extra[extra_count++].ti_Data = (IPTR)"Job log";
    extra[extra_count].ti_Tag = MUIA_Window_RootObject;
    extra[extra_count++].ti_Data = (IPTR)root;
    extra[extra_count].ti_Tag = MUIA_Window_Width;
    extra[extra_count++].ti_Data = 520;
    extra[extra_count].ti_Tag = MUIA_Window_Height;
    extra[extra_count++].ti_Data = 320;
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
    data->body = body;
    data->text[0] = '\0';
    DoMethod(close_button, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(obj, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             obj, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    return obj;
}

DISPATCHER(JobLogWindow)
{
    struct JobLogData *data = NULL;

    if (msg->MethodID != OM_NEW)
        data = INST_DATA(cl, obj);
    switch (msg->MethodID) {
    case OM_NEW:
        return (ULONG)log_new(cl, obj, (struct opSet *)msg);
    case MUIM_VT_JobLogUse: {
        struct MUIP_VT_JobLogUse *use = (struct MUIP_VT_JobLogUse *)msg;

        fill_log(data, (const VTJob *)use->job);
        if (use->open_window)
            set(obj, MUIA_Window_Open, TRUE);
        return 0;
    }
    }
    return DoSuperMethodA(cl, obj, msg);
}
DISPATCHER_END

BOOL VT_JobLogWindow_Init(void)
{
    VT_JobLogWindowClass = MUI_CreateCustomClass(NULL, MUIC_Window, NULL,
                                                 sizeof(struct JobLogData),
                                                 DISPATCHER_REF(JobLogWindow));
    return (BOOL)(VT_JobLogWindowClass != NULL);
}

void VT_JobLogWindow_Exit(void)
{
    if (!VT_JobLogWindowClass)
        return;
    MUI_DeleteCustomClass(VT_JobLogWindowClass);
    VT_JobLogWindowClass = NULL;
}
