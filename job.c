#include <exec/memory.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

#include "job.h"
#include "vt_util.h"

void VT_JobListInit(VTJobList *list)
{
    if (!list)
        return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->next_id = 1;
}

static void free_jobs(VTJobList *list)
{
    ULONG index;

    if (!list->items) {
        list->count = 0;
        list->capacity = 0;
        return;
    }
    for (index = 0; index < list->count; index++)
        FreeVec(list->items[index]);
    FreeVec(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void VT_JobListFree(VTJobList *list)
{
    if (!list)
        return;
    free_jobs(list);
    list->next_id = 1;
}

static ULONG take_id(VTJobList *list)
{
    ULONG id = list->next_id;

    if (id == 0)
        id = 1;
    list->next_id = id + 1;
    if (list->next_id == 0)
        list->next_id = 1;
    return id;
}

static void slide_out(VTJobList *list, ULONG index)
{
    ULONG cursor;

    FreeVec(list->items[index]);
    for (cursor = index; cursor + 1 < list->count; cursor++)
        list->items[cursor] = list->items[cursor + 1];
    list->count--;
    list->items[list->count] = NULL;
}

VTJob *VT_JobListAdd(VTJobList *list, const char *source_path, const VTProfile *profile)
{
    VTJob *job;
    VTProfile snapshot;
    char source_name[VT_PATH_LEN];

    if (!list || !source_path || !source_path[0] || !profile)
        return NULL;
    if (!VT_BaseName(source_path, source_name, sizeof source_name))
        return NULL;
    snapshot = *profile;
    VT_ProfileNormalize(&snapshot);
    if (!VT_ProfileValidate(&snapshot))
        return NULL;
    if (list->count >= VT_JOB_MAX)
        return NULL;
    if (list->count == list->capacity) {
        if (!VT_ArrayGrow((APTR *)&list->items, &list->capacity, list->count,
                          sizeof(VTJob *), VT_JOB_MAX))
            return NULL;
    }

    job = (VTJob *)AllocVec(sizeof(VTJob), MEMF_PUBLIC | MEMF_CLEAR);
    if (!job)
        return NULL;
    if (!VT_StrCopy(job->source_path, sizeof job->source_path, source_path) ||
        !VT_StrCopy(job->source_name, sizeof job->source_name, source_name)) {
        FreeVec(job);
        return NULL;
    }
    job->id = take_id(list);
    job->profile = snapshot;
    job->status = VT_JOB_WAITING;
    list->items[list->count++] = job;
    return job;
}

BOOL VT_JobListRemove(VTJobList *list, ULONG index)
{
    if (!list || index >= list->count)
        return FALSE;
    if (list->items[index]->status == VT_JOB_RUNNING)
        return FALSE;
    slide_out(list, index);
    return TRUE;
}

ULONG VT_JobListClear(VTJobList *list)
{
    ULONG index = 0;
    ULONG removed = 0;

    if (!list)
        return 0;
    while (index < list->count) {
        if (list->items[index]->status == VT_JOB_RUNNING) {
            index++;
            continue;
        }
        slide_out(list, index);
        removed++;
    }
    return removed;
}

VTJob *VT_JobListAt(VTJobList *list, ULONG index)
{
    if (!list || index >= list->count)
        return NULL;
    return list->items[index];
}

VTJob *VT_JobListFind(VTJobList *list, ULONG id)
{
    ULONG index;

    if (!list || id == 0)
        return NULL;
    for (index = 0; index < list->count; index++) {
        if (list->items[index]->id == id)
            return list->items[index];
    }
    return NULL;
}

LONG VT_JobListFindStatus(const VTJobList *list, VTJobStatus status)
{
    ULONG index;

    if (!list)
        return -1;
    for (index = 0; index < list->count; index++) {
        if (list->items[index]->status == status)
            return (LONG)index;
    }
    return -1;
}

const char *VT_JobStatusLabel(VTJobStatus status)
{
    switch (status) {
    case VT_JOB_WAITING:
        return "Waiting";
    case VT_JOB_RUNNING:
        return "Running";
    case VT_JOB_DONE:
        return "Done";
    case VT_JOB_FAILED:
        return "Failed";
    case VT_JOB_CANCELLED:
        return "Cancelled";
    default:
        return "";
    }
}

void VT_JobSetStatus(VTJob *job, VTJobStatus status)
{
    if (!job)
        return;
    if (status < VT_JOB_WAITING || status > VT_JOB_CANCELLED)
        return;
    if (job->status != status) {
        if (status == VT_JOB_WAITING || status == VT_JOB_RUNNING) {
            job->progress = 0;
            job->error[0] = '\0';
        }
        if (status == VT_JOB_DONE)
            job->progress = 100;
    }
    job->status = status;
}

void VT_JobSetProgress(VTJob *job, LONG progress)
{
    if (!job)
        return;
    if (progress < 0)
        progress = 0;
    if (progress > 100)
        progress = 100;
    job->progress = progress;
}

void VT_JobSetError(VTJob *job, const char *message)
{
    if (!job)
        return;
    VT_StrCopyTrunc(job->error, sizeof job->error, message ? message : "");
}

void VT_JobSetProbe(VTJob *job, long long duration_us, const char *video_name,
                    const char *audio_name, unsigned long long size_bytes)
{
    if (!job)
        return;
    job->duration_us = duration_us < 0 ? 0 : duration_us;
    VT_StrCopyTrunc(job->probe_video, sizeof job->probe_video, video_name ? video_name : "");
    VT_StrCopyTrunc(job->probe_audio, sizeof job->probe_audio, audio_name ? audio_name : "");
    job->size_bytes = size_bytes;
    job->has_probe = TRUE;
}

static BOOL output_name(const char *stem, ULONG number, const char *extension, char *out, ULONG out_len)
{
    int wrote;

    if (number == 0)
        wrote = snprintf(out, out_len, "%s%s", stem, extension);
    else
        wrote = snprintf(out, out_len, "%s-%lu%s", stem, (unsigned long)number, extension);
    if (wrote < 0 || (ULONG)wrote >= out_len) {
        if (out_len > 0)
            out[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

BOOL VT_JobAssignOutput(VTJob *job, const char *output_drawer)
{
    char stem[VT_PATH_LEN];
    char file_name[VT_PATH_LEN];
    char candidate[VT_PATH_LEN];
    const char *extension;
    ULONG number;

    if (!job || job->status != VT_JOB_WAITING)
        return FALSE;
    if (!output_drawer || !output_drawer[0])
        return FALSE;
    if (!VT_StripExtension(job->source_name, stem, sizeof stem) || stem[0] == '\0')
        return FALSE;
    if (VT_StrEq(stem, ".") || VT_StrEq(stem, ".."))
        return FALSE;
    extension = VT_ContainerExtension(job->profile.container);
    if (!extension)
        return FALSE;

    for (number = 0; number <= 999; number++) {
        if (number == 1)
            continue;
        if (!output_name(stem, number, extension, file_name, sizeof file_name))
            return FALSE;
        if (!VT_JoinDrawerFile(output_drawer, file_name, candidate, sizeof candidate))
            return FALSE;
        if (VT_StrEqI(candidate, job->source_path))
            continue;
        if (VT_FileExists(candidate))
            continue;
        return VT_StrCopy(job->output_path, sizeof job->output_path, candidate);
    }
    return FALSE;
}
