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

BOOL VT_JobSetProfile(VTJob *job, const VTProfile *profile)
{
    VTProfile snapshot;

    if (!job || !profile || job->status != VT_JOB_WAITING)
        return FALSE;
    snapshot = *profile;
    VT_ProfileNormalize(&snapshot);
    if (!VT_ProfileValidate(&snapshot))
        return FALSE;
    job->profile = snapshot;
    return TRUE;
}

BOOL VT_JobReplaceProfile(VTJob *job, const VTProfile *profile)
{
    VTProfile snapshot;

    if (!job || !profile)
        return FALSE;
    snapshot = *profile;
    VT_ProfileNormalize(&snapshot);
    if (!VT_ProfileValidate(&snapshot))
        return FALSE;
    job->profile = snapshot;
    return TRUE;
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

static const char *status_token(VTJobStatus status)
{
    switch (status) {
    case VT_JOB_WAITING:
        return "waiting";
    case VT_JOB_RUNNING:
        return "running";
    case VT_JOB_DONE:
        return "done";
    case VT_JOB_FAILED:
        return "failed";
    case VT_JOB_CANCELLED:
        return "cancelled";
    default:
        return NULL;
    }
}

static BOOL status_from_token(const char *token, VTJobStatus *out)
{
    VTJobStatus status;

    if (!token || !out)
        return FALSE;
    for (status = VT_JOB_WAITING; status <= VT_JOB_CANCELLED; status++) {
        const char *name = status_token(status);

        if (name && VT_StrEq(token, name)) {
            *out = status;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL parse_u64(const char *text, unsigned long long *out)
{
    unsigned long long value = 0;
    BOOL any = FALSE;

    if (!text || !out)
        return FALSE;
    while (*text >= '0' && *text <= '9') {
        unsigned long long digit = (unsigned long long)(*text - '0');

        if (value > (~0ULL - digit) / 10ULL)
            return FALSE;
        value = value * 10ULL + digit;
        any = TRUE;
        text++;
    }
    if (!any || *text != '\0')
        return FALSE;
    *out = value;
    return TRUE;
}

static void format_u64(char *out, ULONG out_len, unsigned long long value)
{
    char digits[32];
    ULONG count = 0;
    ULONG index;

    if (!out || out_len == 0)
        return;
    if (value == 0) {
        VT_StrCopy(out, out_len, "0");
        return;
    }
    while (value > 0 && count < sizeof digits) {
        digits[count++] = (char)('0' + (int)(value % 10ULL));
        value /= 10ULL;
    }
    if (count + 1 > out_len) {
        out[0] = '\0';
        return;
    }
    for (index = 0; index < count; index++)
        out[index] = digits[count - 1 - index];
    out[count] = '\0';
}

static BOOL add_piece(char *text, ULONG cap, ULONG *used, const char *piece)
{
    ULONG len;

    if (!text || !used || !piece)
        return FALSE;
    len = (ULONG)strlen(piece);
    if (*used + len + 1 > cap)
        return FALSE;
    memcpy(text + *used, piece, len + 1);
    *used += len;
    return TRUE;
}

static BOOL add_job_text(char *text, ULONG cap, ULONG *used, const VTJob *job)
{
    char line[VT_PATH_LEN + 64];
    char keys[512];
    char duration[32];
    char bytes[32];
    const char *token;
    int wrote;

    token = status_token(job->status);
    if (!token || !VT_ProfileFormatKeys(&job->profile, keys, sizeof keys))
        return FALSE;
    wrote = snprintf(line, sizeof line, "job\nsource=%s\nstatus=%s\nprogress=%ld\n",
                     job->source_path, token, (long)job->progress);
    if (wrote < 0 || (ULONG)wrote >= sizeof line || !add_piece(text, cap, used, line))
        return FALSE;
    wrote = snprintf(line, sizeof line, "output=%s\n", job->output_path);
    if (wrote < 0 || (ULONG)wrote >= sizeof line || !add_piece(text, cap, used, line))
        return FALSE;
    wrote = snprintf(line, sizeof line, "error=%s\n", job->error);
    if (wrote < 0 || (ULONG)wrote >= sizeof line || !add_piece(text, cap, used, line))
        return FALSE;
    format_u64(duration, sizeof duration,
               job->duration_us < 0 ? 0ULL : (unsigned long long)job->duration_us);
    format_u64(bytes, sizeof bytes, job->size_bytes);
    wrote = snprintf(line, sizeof line,
                     "duration_us=%s\nprobe_video=%s\nprobe_audio=%s\nsize_bytes=%s\nhas_probe=%ld\n",
                     duration, job->probe_video, job->probe_audio, bytes,
                     job->has_probe ? 1L : 0L);
    if (wrote < 0 || (ULONG)wrote >= sizeof line || !add_piece(text, cap, used, line))
        return FALSE;
    return add_piece(text, cap, used, keys);
}

BOOL VT_JobListSave(const VTJobList *list, const char *path)
{
    char *text;
    ULONG cap;
    ULONG used = 0;
    ULONG index;
    BOOL ok;
    int wrote;

    if (!list || !path || !path[0])
        return FALSE;
    cap = 32;
    for (index = 0; index < list->count; index++) {
        const VTJob *job = list->items[index];

        if (!job || !VT_IsSingleLine(job->source_path) || !VT_IsSingleLine(job->output_path) ||
            !VT_IsSingleLine(job->error) || !VT_IsSingleLine(job->probe_video) ||
            !VT_IsSingleLine(job->probe_audio))
            return FALSE;
        cap += (ULONG)strlen(job->source_path) + (ULONG)strlen(job->output_path) +
               (ULONG)strlen(job->error) + 800;
    }
    if (cap > (ULONG)VT_TEXT_FILE_MAX)
        return FALSE;
    text = (char *)AllocVec(cap, MEMF_PUBLIC | MEMF_CLEAR);
    if (!text)
        return FALSE;
    wrote = snprintf(text, cap, "%s %ld\n", VT_JOBLIST_MAGIC, (long)VT_JOBLIST_FORMAT_VERSION);
    if (wrote < 0 || (ULONG)wrote >= cap) {
        FreeVec(text);
        return FALSE;
    }
    used = (ULONG)wrote;
    for (index = 0; index < list->count; index++) {
        if (!add_job_text(text, cap, &used, list->items[index])) {
            FreeVec(text);
            return FALSE;
        }
    }
    ok = VT_WriteTextFileAtomic(path, text);
    FreeVec(text);
    return ok;
}

static BOOL commit_draft(VTJobList *list, VTJob *draft)
{
    VTJob *job;

    if (!draft->source_path[0] || !VT_BaseName(draft->source_path, draft->source_name,
                                               sizeof draft->source_name))
        return FALSE;
    VT_ProfileNormalize(&draft->profile);
    if (!VT_ProfileValidate(&draft->profile))
        return FALSE;
    if (draft->progress < 0 || draft->progress > 100)
        return FALSE;
    if (draft->status == VT_JOB_RUNNING) {
        draft->status = VT_JOB_WAITING;
        draft->progress = 0;
        draft->output_path[0] = '\0';
        draft->error[0] = '\0';
    }
    if (list->count >= VT_JOB_MAX)
        return FALSE;
    if (list->count == list->capacity) {
        if (!VT_ArrayGrow((APTR *)&list->items, &list->capacity, list->count,
                          sizeof(VTJob *), VT_JOB_MAX))
            return FALSE;
    }
    job = (VTJob *)AllocVec(sizeof *job, MEMF_PUBLIC | MEMF_CLEAR);
    if (!job)
        return FALSE;
    *job = *draft;
    job->id = take_id(list);
    list->items[list->count++] = job;
    return TRUE;
}

typedef struct JobParse {
    VTJobList *list;
    VTJob *draft;
    BOOL saw_header;
    BOOL in_job;
} JobParse;

static void reset_draft(VTJob *draft)
{
    memset(draft, 0, sizeof *draft);
    VT_ProfileSetDefaults(&draft->profile);
    draft->status = VT_JOB_WAITING;
}

static BOOL apply_job_line(const char *line, APTR user)
{
    JobParse *state = (JobParse *)user;
    char key[32];
    char value[VT_PATH_LEN];
    LONG number;
    unsigned long long big;

    if (!state->saw_header) {
        if (!VT_IsFormatHeader(line, VT_JOBLIST_MAGIC, VT_JOBLIST_FORMAT_VERSION))
            return FALSE;
        state->saw_header = TRUE;
        return TRUE;
    }
    if (VT_StrEq(line, "job")) {
        if (state->in_job && !commit_draft(state->list, state->draft))
            return FALSE;
        reset_draft(state->draft);
        state->in_job = TRUE;
        return TRUE;
    }
    if (!state->in_job || !VT_ParseKeyValue(line, key, sizeof key, value, sizeof value))
        return FALSE;
    if (VT_StrEq(key, "source"))
        return VT_StrCopy(state->draft->source_path, sizeof state->draft->source_path, value);
    if (VT_StrEq(key, "output"))
        return VT_StrCopy(state->draft->output_path, sizeof state->draft->output_path, value);
    if (VT_StrEq(key, "error"))
        return VT_StrCopy(state->draft->error, sizeof state->draft->error, value);
    if (VT_StrEq(key, "probe_video"))
        return VT_StrCopy(state->draft->probe_video, sizeof state->draft->probe_video, value);
    if (VT_StrEq(key, "probe_audio"))
        return VT_StrCopy(state->draft->probe_audio, sizeof state->draft->probe_audio, value);
    if (VT_StrEq(key, "status"))
        return status_from_token(value, &state->draft->status);
    if (VT_StrEq(key, "progress")) {
        if (!VT_ParseLong(value, &number))
            return FALSE;
        state->draft->progress = number;
        return TRUE;
    }
    if (VT_StrEq(key, "has_probe")) {
        if (!VT_ParseLong(value, &number) || (number != 0 && number != 1))
            return FALSE;
        state->draft->has_probe = number ? TRUE : FALSE;
        return TRUE;
    }
    if (VT_StrEq(key, "duration_us")) {
        if (!parse_u64(value, &big) || big > 9223372036854775807ULL)
            return FALSE;
        state->draft->duration_us = (long long)big;
        return TRUE;
    }
    if (VT_StrEq(key, "size_bytes")) {
        if (!parse_u64(value, &big))
            return FALSE;
        state->draft->size_bytes = big;
        return TRUE;
    }
    return VT_ProfileApplyKey(&state->draft->profile, key, value, NULL);
}

BOOL VT_JobListLoad(VTJobList *list, const char *path)
{
    char *text = NULL;
    VTJobList loaded;
    VTJob *draft;
    JobParse state;

    if (!list || !path || !path[0])
        return FALSE;
    draft = (VTJob *)AllocVec(sizeof *draft, MEMF_PUBLIC | MEMF_CLEAR);
    if (!draft)
        return FALSE;
    VT_JobListInit(&loaded);
    if (!VT_ReadTextFile(path, &text, VT_TEXT_FILE_MAX)) {
        FreeVec(draft);
        return FALSE;
    }
    state.list = &loaded;
    state.draft = draft;
    state.saw_header = FALSE;
    state.in_job = FALSE;
    if (!VT_ForEachLine(text, apply_job_line, &state) || !state.saw_header ||
        (state.in_job && !commit_draft(&loaded, draft))) {
        VT_FreeText(text);
        FreeVec(draft);
        VT_JobListFree(&loaded);
        return FALSE;
    }
    VT_FreeText(text);
    FreeVec(draft);
    VT_JobListFree(list);
    *list = loaded;
    return TRUE;
}
