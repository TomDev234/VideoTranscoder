/*
 * Serial queue. The process owns libav. The main task owns jobs and MUI.
 * A Stop bumps the batch id, so files that have not started stay Waiting.
 */
#include <stdio.h>
#include <string.h>

#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/semaphores.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <libavutil/log.h>

#include "encode_task.h"
#include "encoder.h"
#include "probe.h"
#include "vt_util.h"

enum {
    VT_MSG_JOB = 1,
    VT_MSG_QUIT = 2,
    VT_MSG_CLAIM = 3,
    VT_MSG_PROBE = 4,
    VT_MSG_PROGRESS = 5,
    VT_MSG_FINISHED = 6,
    VT_MSG_IDLE = 7
};

struct VTMsg {
    struct Message msg;
    ULONG type;
    ULONG job_id;
    ULONG batch;
    LONG progress;
    LONG result;
    BOOL accepted;
    long long duration_us;
    unsigned long long size_bytes;
    char probe_video[VT_CODEC_LABEL_LEN];
    char probe_audio[VT_CODEC_LABEL_LEN];
    char source[VT_PATH_LEN];
    char output[VT_PATH_LEN];
    char error[VT_ERROR_LEN];
    VTProfile profile;
};

struct VTEngine {
    struct SignalSemaphore lock;
    struct MsgPort *to_main;
    struct MsgPort *to_task;
    struct MsgPort *replies;
    struct Task *main_task;
    struct Process *proc;
    struct VTMsg *spare;
    struct VTMsg *idle_msg;
    struct VTMsg *quit_msg;
    char drawer[VT_PATH_LEN];
    volatile LONG cancel;
    volatile LONG quit;
    volatile LONG running;
    volatile ULONG batch;
    LONG ready_sig;
    LONG done_sig;
    BOOL ready;
};

static struct VTEngine engine;
static BOOL engine_inited;

static struct VTMsg *make_msg(ULONG type, BOOL emergency)
{
    struct VTMsg *msg;

    msg = (struct VTMsg *)AllocVec(sizeof *msg, MEMF_PUBLIC | MEMF_CLEAR);
    if (!msg && emergency && engine.spare) {
        msg = engine.spare;
        engine.spare = NULL;
    }
    if (!msg)
        return NULL;
    msg->msg.mn_Node.ln_Type = NT_MESSAGE;
    msg->msg.mn_Length = sizeof *msg;
    msg->type = type;
    return msg;
}

static void post_main(struct VTMsg *msg)
{
    if (msg && engine.to_main)
        PutMsg(engine.to_main, (struct Message *)msg);
}

static BOOL batch_current(ULONG batch)
{
    BOOL ok;

    ObtainSemaphore(&engine.lock);
    ok = (BOOL)(batch == engine.batch && !engine.quit);
    ReleaseSemaphore(&engine.lock);
    return ok;
}

static void on_progress(LONG percent, APTR user)
{
    struct VTMsg *msg;
    ULONG job_id = *(ULONG *)user;

    msg = make_msg(VT_MSG_PROGRESS, FALSE);
    if (!msg)
        return;
    msg->job_id = job_id;
    msg->progress = percent;
    post_main(msg);
}

static void send_probe(ULONG job_id, const VTProbeInfo *info)
{
    struct VTMsg *msg;

    if (!info)
        return;
    msg = make_msg(VT_MSG_PROBE, FALSE);
    if (!msg)
        return;
    msg->job_id = job_id;
    msg->duration_us = info->duration_us;
    msg->size_bytes = info->size_bytes;
    VT_StrCopyTrunc(msg->probe_video, sizeof msg->probe_video, info->video_name);
    VT_StrCopyTrunc(msg->probe_audio, sizeof msg->probe_audio, info->audio_name);
    post_main(msg);
}

static void send_finished(ULONG job_id, VTEncodeResult result, const char *error)
{
    struct VTMsg *msg = make_msg(VT_MSG_FINISHED, TRUE);

    if (!msg)
        return;
    msg->job_id = job_id;
    msg->result = (LONG)result;
    VT_StrCopyTrunc(msg->error, sizeof msg->error, error ? error : "");
    post_main(msg);
}

static int claim_job(ULONG job_id, char *output, ULONG output_len)
{
    struct VTMsg *msg = make_msg(VT_MSG_CLAIM, FALSE);
    BOOL accepted;

    if (!msg)
        return -1;
    msg->job_id = job_id;
    msg->msg.mn_ReplyPort = engine.replies;
    post_main(msg);
    for (;;) {
        WaitPort(engine.replies);
        if (GetMsg(engine.replies))
            break;
    }
    accepted = msg->accepted;
    if (accepted)
        VT_StrCopy(output, output_len, msg->output);
    else if (output && output_len)
        output[0] = '\0';
    FreeVec(msg);
    if (!accepted)
        return 0;
    return output && output[0] ? 1 : 0;
}

static void process_job(struct VTMsg *msg)
{
    VTProbeInfo info;
    VTProbeResult probed;
    char output[VT_PATH_LEN];
    char error[VT_ERROR_LEN];
    ULONG job_id;
    int claimed;

    error[0] = '\0';
    output[0] = '\0';
    if (!batch_current(msg->batch))
        goto done;
    job_id = msg->job_id;
    probed = VT_ProbeFile(msg->source, &info, &engine.cancel, error, sizeof error);
    if (probed == VT_PROBE_CANCELLED || !batch_current(msg->batch))
        goto done;
    if (probed != VT_PROBE_OK) {
        send_finished(job_id, VT_ENCODE_FAILED, error);
        goto done;
    }
    send_probe(job_id, &info);
    if (!batch_current(msg->batch))
        goto done;
    if (!VT_EncodeSupported(&msg->profile)) {
        send_finished(job_id, VT_ENCODE_FAILED, VT_EncodeUnsupportedReason());
        goto done;
    }
    claimed = claim_job(job_id, output, sizeof output);
    if (claimed < 0) {
        send_finished(job_id, VT_ENCODE_FAILED, "Out of memory.");
        goto done;
    }
    if (claimed == 0)
        goto done;
    if (!batch_current(msg->batch) || engine.cancel) {
        VT_DeleteIfExists(output);
        send_finished(job_id, VT_ENCODE_CANCELLED, "");
        goto done;
    }
    {
        VTEncodeResult encoded = VT_EncodeFile(msg->source, output, &msg->profile, &engine.cancel,
                                               on_progress, &job_id, error, sizeof error);

        send_finished(job_id, encoded, error);
    }
done:
    FreeVec(msg);
}

static void flush_work(void)
{
    struct Message *msg;

    if (!engine.to_task)
        return;
    while ((msg = GetMsg(engine.to_task)))
        FreeVec(msg);
}

static void send_idle(void)
{
    struct VTMsg *msg = engine.idle_msg;

    if (!msg || !engine.replies)
        return;
    msg->type = VT_MSG_IDLE;
    msg->msg.mn_ReplyPort = engine.replies;
    post_main(msg);
    for (;;) {
        WaitPort(engine.replies);
        if (GetMsg(engine.replies))
            break;
    }
}

static void encode_loop(void)
{
    for (;;) {
        struct VTMsg *msg;
        BOOL saw_quit = FALSE;

        /* Leave the port alive until the quit message arrives, so the
           main task can still PutMsg after it sets the cancel flag. */
        WaitPort(engine.to_task);
        while ((msg = (struct VTMsg *)GetMsg(engine.to_task))) {
            if (msg->type == VT_MSG_QUIT) {
                FreeVec(msg);
                saw_quit = TRUE;
                break;
            }
            if (engine.quit || msg->type != VT_MSG_JOB) {
                FreeVec(msg);
                continue;
            }
            process_job(msg);
        }
        if (saw_quit) {
            flush_work();
            return;
        }
        if (!engine.quit)
            send_idle();
    }
}

static void encode_entry(void)
{
    struct MsgPort *work;
    struct MsgPort *replies;

    work = CreateMsgPort();
    replies = CreateMsgPort();
    engine.spare = make_msg(VT_MSG_FINISHED, FALSE);
    engine.idle_msg = make_msg(VT_MSG_IDLE, FALSE);
    if (!work || !replies || !engine.spare || !engine.idle_msg) {
        if (engine.spare)
            FreeVec(engine.spare);
        if (engine.idle_msg)
            FreeVec(engine.idle_msg);
        engine.spare = NULL;
        engine.idle_msg = NULL;
        if (work)
            DeleteMsgPort(work);
        if (replies)
            DeleteMsgPort(replies);
        Signal(engine.main_task, 1UL << engine.ready_sig);
        return;
    }
    engine.replies = replies;
    engine.to_task = work;
    Signal(engine.main_task, 1UL << engine.ready_sig);
    av_log_set_level(AV_LOG_ERROR);
    encode_loop();
    flush_work();
    if (engine.spare) {
        FreeVec(engine.spare);
        engine.spare = NULL;
    }
    if (engine.idle_msg) {
        FreeVec(engine.idle_msg);
        engine.idle_msg = NULL;
    }
    engine.to_task = NULL;
    engine.replies = NULL;
    DeleteMsgPort(work);
    DeleteMsgPort(replies);
    Signal(engine.main_task, 1UL << engine.done_sig);
}

static void release_signals(void)
{
    if (engine.ready_sig >= 0)
        FreeSignal(engine.ready_sig);
    if (engine.done_sig >= 0)
        FreeSignal(engine.done_sig);
    engine.ready_sig = -1;
    engine.done_sig = -1;
}

BOOL VT_Encode_Init(void)
{
    struct TagItem tags[] = {
        { NP_Entry, (IPTR)encode_entry },
        { NP_Name, (IPTR)"VideoTranscoder encode" },
        { NP_Priority, (IPTR)(LONG)-1 },
        { NP_StackSize, 1024UL * 1024UL },
        { NP_WindowPtr, (IPTR)(LONG)-1 },
        { NP_Cli, FALSE },
        { NP_Input, 0 },
        { NP_Output, 0 },
        { NP_Error, 0 },
        { NP_CloseInput, FALSE },
        { NP_CloseOutput, FALSE },
        { NP_CloseError, FALSE },
        { TAG_END, 0 }
    };

    if (engine.ready)
        return TRUE;
    memset(&engine, 0, sizeof engine);
    InitSemaphore(&engine.lock);
    engine_inited = TRUE;
    engine.ready_sig = -1;
    engine.done_sig = -1;
    engine.main_task = FindTask(NULL);
    engine.ready_sig = AllocSignal(-1);
    engine.done_sig = AllocSignal(-1);
    engine.to_main = CreateMsgPort();
    engine.quit_msg = make_msg(VT_MSG_QUIT, FALSE);
    if (engine.ready_sig < 0 || engine.done_sig < 0 || !engine.to_main || !engine.quit_msg) {
        if (engine.quit_msg)
            FreeVec(engine.quit_msg);
        engine.quit_msg = NULL;
        if (engine.to_main)
            DeleteMsgPort(engine.to_main);
        engine.to_main = NULL;
        release_signals();
        return FALSE;
    }
    engine.proc = CreateNewProc(tags);
    if (!engine.proc) {
        FreeVec(engine.quit_msg);
        engine.quit_msg = NULL;
        DeleteMsgPort(engine.to_main);
        engine.to_main = NULL;
        release_signals();
        return FALSE;
    }
    Wait(1UL << engine.ready_sig);
    if (!engine.to_task) {
        engine.proc = NULL;
        FreeVec(engine.quit_msg);
        engine.quit_msg = NULL;
        DeleteMsgPort(engine.to_main);
        engine.to_main = NULL;
        release_signals();
        return FALSE;
    }
    engine.ready = TRUE;
    return TRUE;
}

static void note(char *status, ULONG status_len, const char *text)
{
    if (status && status_len && text)
        VT_StrCopyTrunc(status, status_len, text);
}

static void note_job(char *status, ULONG status_len, const char *prefix, const VTJob *job)
{
    char line[160];
    const char *name = (job && job->source_name[0]) ? job->source_name : "file";

    snprintf(line, sizeof line, "%s %s.", prefix, name);
    note(status, status_len, line);
}

static void apply_claim(VTJobList *jobs, struct VTMsg *msg, char *status, ULONG status_len)
{
    char drawer[VT_PATH_LEN];
    VTJob *job;

    msg->accepted = FALSE;
    msg->output[0] = '\0';
    ObtainSemaphore(&engine.lock);
    VT_StrCopy(drawer, sizeof drawer, engine.drawer);
    ReleaseSemaphore(&engine.lock);
    job = jobs ? VT_JobListFind(jobs, msg->job_id) : NULL;
    if (!job || job->status != VT_JOB_WAITING)
        return;
    if (!VT_JobAssignOutput(job, drawer)) {
        VT_JobSetError(job, "Could not choose an output name.");
        VT_JobSetStatus(job, VT_JOB_FAILED);
        note_job(status, status_len, "Could not name", job);
        return;
    }
    if (!VT_StrCopy(msg->output, sizeof msg->output, job->output_path)) {
        VT_JobSetError(job, "Could not choose an output name.");
        VT_JobSetStatus(job, VT_JOB_FAILED);
        return;
    }
    VT_JobSetStatus(job, VT_JOB_RUNNING);
    msg->accepted = TRUE;
    note_job(status, status_len, "Encoding", job);
}

static void apply_finished(VTJobList *jobs, struct VTMsg *msg, char *status, ULONG status_len)
{
    VTJob *job = jobs ? VT_JobListFind(jobs, msg->job_id) : NULL;
    char line[160];

    if (!job)
        return;
    if (msg->result == VT_ENCODE_OK) {
        VT_JobSetStatus(job, VT_JOB_DONE);
        note_job(status, status_len, "Finished", job);
        return;
    }
    if (msg->result == VT_ENCODE_CANCELLED) {
        VT_JobSetStatus(job, VT_JOB_CANCELLED);
        note_job(status, status_len, "Cancelled", job);
        return;
    }
    VT_JobSetError(job, msg->error[0] ? msg->error : "Encode failed.");
    VT_JobSetStatus(job, VT_JOB_FAILED);
    if (msg->error[0])
        note(status, status_len, msg->error);
    else {
        snprintf(line, sizeof line, "Failed %s.", job->source_name);
        note(status, status_len, line);
    }
}

BOOL VT_Encode_Drain(VTJobList *jobs, char *status, ULONG status_len)
{
    struct VTMsg *msg;
    BOOL changed = FALSE;

    if (!engine.to_main)
        return FALSE;
    while ((msg = (struct VTMsg *)GetMsg(engine.to_main))) {
        changed = TRUE;
        switch (msg->type) {
        case VT_MSG_CLAIM:
            apply_claim(jobs, msg, status, status_len);
            ReplyMsg((struct Message *)msg);
            break;
        case VT_MSG_PROBE: {
            VTJob *job = jobs ? VT_JobListFind(jobs, msg->job_id) : NULL;

            if (job) {
                VT_JobSetProbe(job, msg->duration_us, msg->probe_video, msg->probe_audio,
                               msg->size_bytes);
                note_job(status, status_len, "Probing", job);
            }
            FreeVec(msg);
            break;
        }
        case VT_MSG_PROGRESS: {
            VTJob *job = jobs ? VT_JobListFind(jobs, msg->job_id) : NULL;

            if (job && job->status == VT_JOB_RUNNING)
                VT_JobSetProgress(job, msg->progress);
            FreeVec(msg);
            break;
        }
        case VT_MSG_FINISHED:
            apply_finished(jobs, msg, status, status_len);
            FreeVec(msg);
            break;
        case VT_MSG_IDLE: {
            BOOL stopped;

            ObtainSemaphore(&engine.lock);
            engine.running = FALSE;
            stopped = (BOOL)engine.cancel;
            ReleaseSemaphore(&engine.lock);
            note(status, status_len, stopped ? "Encoding stopped." : "Queue finished.");
            ReplyMsg((struct Message *)msg);
            break;
        }
        default:
            FreeVec(msg);
            break;
        }
    }
    return changed;
}

void VT_Encode_Shutdown(VTJobList *jobs)
{
    struct VTMsg *quit;
    ULONG port_mask;
    ULONG done_mask;

    if (!engine_inited)
        return;
    if (engine.ready && engine.to_task) {
        ObtainSemaphore(&engine.lock);
        engine.quit = TRUE;
        engine.cancel = TRUE;
        engine.batch++;
        ReleaseSemaphore(&engine.lock);
        quit = engine.quit_msg;
        engine.quit_msg = NULL;
        if (quit && engine.to_task)
            PutMsg(engine.to_task, (struct Message *)quit);
        else if (quit)
            FreeVec(quit);
        /* Reply to anything already queued before waiting, or a task
           blocked in send_idle never sees the quit message. */
        VT_Encode_Drain(jobs, NULL, 0);
        port_mask = engine.to_main ? (1UL << engine.to_main->mp_SigBit) : 0;
        done_mask = engine.done_sig >= 0 ? (1UL << engine.done_sig) : 0;
        if (done_mask) {
            for (;;) {
                ULONG got = Wait(port_mask | done_mask);

                VT_Encode_Drain(jobs, NULL, 0);
                if (got & done_mask)
                    break;
            }
            VT_Encode_Drain(jobs, NULL, 0);
        }
    }
    if (engine.to_main) {
        struct Message *msg;

        while ((msg = GetMsg(engine.to_main))) {
            if (((struct VTMsg *)msg)->type == VT_MSG_CLAIM)
                ReplyMsg(msg);
            else
                FreeVec(msg);
        }
        DeleteMsgPort(engine.to_main);
        engine.to_main = NULL;
    }
    release_signals();
    if (engine.quit_msg) {
        FreeVec(engine.quit_msg);
        engine.quit_msg = NULL;
    }
    engine.proc = NULL;
    engine.ready = FALSE;
    engine.running = FALSE;
}

ULONG VT_Encode_Signal(void)
{
    if (!engine.ready || !engine.to_main)
        return 0;
    return 1UL << engine.to_main->mp_SigBit;
}

BOOL VT_Encode_IsReady(void)
{
    return engine.ready;
}

BOOL VT_Encode_IsRunning(void)
{
    return (BOOL)engine.running;
}

BOOL VT_Encode_Stop(void)
{
    if (!engine.ready || !engine.running)
        return FALSE;
    ObtainSemaphore(&engine.lock);
    if (!engine.running) {
        ReleaseSemaphore(&engine.lock);
        return FALSE;
    }
    engine.cancel = TRUE;
    engine.batch++;
    ReleaseSemaphore(&engine.lock);
    return TRUE;
}

BOOL VT_Encode_Submit(VTJobList *jobs, const char *output_drawer)
{
    ULONG index;
    ULONG queued = 0;
    ULONG my_batch;
    struct MsgPort *port;

    if (!jobs || !output_drawer || !output_drawer[0])
        return FALSE;
    ObtainSemaphore(&engine.lock);
    if (!engine.ready || !engine.to_task || engine.running || engine.quit) {
        ReleaseSemaphore(&engine.lock);
        return FALSE;
    }
    if (!VT_StrCopy(engine.drawer, sizeof engine.drawer, output_drawer)) {
        ReleaseSemaphore(&engine.lock);
        return FALSE;
    }
    engine.batch++;
    engine.cancel = FALSE;
    engine.running = TRUE;
    my_batch = engine.batch;
    port = engine.to_task;
    ReleaseSemaphore(&engine.lock);

    for (index = 0; index < jobs->count; index++) {
        VTJob *job = jobs->items[index];
        struct VTMsg *msg;

        if (!job || job->status != VT_JOB_WAITING)
            continue;
        msg = make_msg(VT_MSG_JOB, FALSE);
        if (!msg) {
            ObtainSemaphore(&engine.lock);
            if (queued == 0)
                engine.running = FALSE;
            else {
                engine.cancel = TRUE;
                engine.batch++;
            }
            ReleaseSemaphore(&engine.lock);
            return FALSE;
        }
        msg->job_id = job->id;
        msg->batch = my_batch;
        msg->profile = job->profile;
        if (!VT_StrCopy(msg->source, sizeof msg->source, job->source_path)) {
            FreeVec(msg);
            continue;
        }
        PutMsg(port, (struct Message *)msg);
        queued++;
    }
    if (queued == 0) {
        ObtainSemaphore(&engine.lock);
        engine.running = FALSE;
        ReleaseSemaphore(&engine.lock);
        return FALSE;
    }
    return TRUE;
}
