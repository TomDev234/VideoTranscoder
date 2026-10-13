/*
 * Batch queue model. Each job keeps its own profile snapshot, so a later
 * edit to the profile drawer does not change work already queued.
 *
 * Jobs are allocated one by one. A VTJob pointer stays valid until that
 * job is removed or the list is freed. The running job cannot be removed.
 * Call these functions from the main task. Phase 3 posts progress to the
 * main task, which then calls the setters. The id is what a message carries.
 */
#ifndef VT_JOB_H
#define VT_JOB_H

#include "profile.h"

#define VT_JOB_MAX 4096

typedef enum {
    VT_JOB_WAITING = 0,
    VT_JOB_RUNNING = 1,
    VT_JOB_DONE = 2,
    VT_JOB_FAILED = 3,
    VT_JOB_CANCELLED = 4
} VTJobStatus;

typedef struct VTJob {
    ULONG id;
    char source_path[VT_PATH_LEN];
    char source_name[VT_PATH_LEN];
    char output_path[VT_PATH_LEN];
    VTProfile profile;
    VTJobStatus status;
    LONG progress;
    char error[VT_ERROR_LEN];
    BOOL has_probe;
    long long duration_us;
    char probe_video[VT_CODEC_LABEL_LEN];
    char probe_audio[VT_CODEC_LABEL_LEN];
    unsigned long long size_bytes;
} VTJob;

typedef struct VTJobList {
    VTJob **items;
    ULONG count;
    ULONG capacity;
    ULONG next_id;
} VTJobList;

void VT_JobListInit(VTJobList *list);
void VT_JobListFree(VTJobList *list);

VTJob *VT_JobListAdd(VTJobList *list, const char *source_path, const VTProfile *profile);
BOOL VT_JobListRemove(VTJobList *list, ULONG index);

/* Drops every job that is not running. Returns how many were dropped. */
ULONG VT_JobListClear(VTJobList *list);

VTJob *VT_JobListAt(VTJobList *list, ULONG index);
VTJob *VT_JobListFind(VTJobList *list, ULONG id);
LONG VT_JobListFindStatus(const VTJobList *list, VTJobStatus status);

const char *VT_JobStatusLabel(VTJobStatus status);
void VT_JobSetStatus(VTJob *job, VTJobStatus status);
void VT_JobSetProgress(VTJob *job, LONG progress);
void VT_JobSetError(VTJob *job, const char *message);
void VT_JobSetProbe(VTJob *job, long long duration_us, const char *video_name,
                    const char *audio_name, unsigned long long size_bytes);

/* Waiting jobs only. Never overwrites an existing file: name, name-2, name-3, ... */
BOOL VT_JobAssignOutput(VTJob *job, const char *output_drawer);

#endif
