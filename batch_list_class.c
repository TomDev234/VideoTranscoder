/*
 * Queue list. Rows point at VTJob objects owned by the model.
 * Clearing the list frees only the row wrappers.
 */
#include <emul/emulregs.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>

#include <stdio.h>

#include "macros.h"
#include "vt_ui.h"
#include "job.h"
#include "vt_util.h"

struct MUI_CustomClass *VT_BatchListClass;

struct BatchRow {
    VTJob *job;
    char progress[16];
    char output[128];
};

struct BatchData {
    VTJobList *jobs;
};

static ULONG batch_display(void);
static ULONG batch_construct(void);
static ULONG batch_destruct(void);

static struct Hook display_hook;
static struct Hook construct_hook;
static struct Hook destruct_hook;
static struct EmulLibEntry display_gate = { TRAP_LIB, 0, (void (*)(void))batch_display };
static struct EmulLibEntry construct_gate = { TRAP_LIB, 0, (void (*)(void))batch_construct };
static struct EmulLibEntry destruct_gate = { TRAP_LIB, 0, (void (*)(void))batch_destruct };
static BOOL hooks_ready;

static ULONG batch_display(void)
{
    char **array = (char **)REG_A2;
    struct BatchRow *row = (struct BatchRow *)REG_A1;

    if (!row) {
        array[0] = "Source";
        array[1] = "Profile";
        array[2] = "Status";
        array[3] = "Progress";
        array[4] = "Output";
        return 0;
    }
    snprintf(row->progress, sizeof row->progress, "%ld%%", (long)row->job->progress);
    if (row->job->output_path[0] == '\0' ||
        !VT_BaseName(row->job->output_path, row->output, sizeof row->output))
        row->output[0] = '\0';
    array[0] = row->job->source_name;
    array[1] = row->job->profile.name;
    array[2] = (char *)VT_JobStatusLabel(row->job->status);
    array[3] = row->progress;
    array[4] = row->output;
    return 0;
}

static ULONG batch_construct(void)
{
    APTR pool = (APTR)REG_A2;
    VTJob *job = (VTJob *)REG_A1;
    struct BatchRow *row;

    if (!pool || !job)
        return 0;
    row = (struct BatchRow *)AllocPooled(pool, sizeof *row);
    if (!row)
        return 0;
    row->job = job;
    row->progress[0] = '\0';
    row->output[0] = '\0';
    return (ULONG)row;
}

static ULONG batch_destruct(void)
{
    APTR pool = (APTR)REG_A2;
    struct BatchRow *row = (struct BatchRow *)REG_A1;

    if (pool && row)
        FreePooled(pool, row, sizeof *row);
    return 0;
}

static void prepare_hooks(void)
{
    if (hooks_ready)
        return;
    display_hook.h_Entry = (HOOKFUNC)&display_gate;
    construct_hook.h_Entry = (HOOKFUNC)&construct_gate;
    destruct_hook.h_Entry = (HOOKFUNC)&destruct_gate;
    hooks_ready = TRUE;
}

static ULONG batch_get_active(Object *obj)
{
    LONG active;
    struct BatchRow *row = NULL;

    active = (LONG)xget(obj, MUIA_List_Active);
    if (active < 0)
        return 0;
    DoMethod(obj, MUIM_List_GetEntry, active, &row);
    if (!row || !row->job)
        return 0;
    return (ULONG)row->job;
}

static ULONG batch_sync(struct BatchData *data, Object *obj)
{
    ULONG index;

    set(obj, MUIA_List_Quiet, TRUE);
    DoMethod(obj, MUIM_List_Clear);
    if (data->jobs) {
        for (index = 0; index < data->jobs->count; index++) {
            DoMethod(obj, MUIM_List_InsertSingle, data->jobs->items[index],
                     MUIV_List_Insert_Bottom);
        }
    }
    set(obj, MUIA_List_Quiet, FALSE);
    return 0;
}

static Object *batch_new(struct IClass *cl, Object *obj, struct opSet *ops)
{
    struct BatchData *data;
    struct TagItem extra[] = {
        { MUIA_List_Format, (IPTR)"BAR,BAR,BAR,BAR,BAR" },
        { MUIA_List_Title, TRUE },
        { MUIA_List_DisplayHook, (IPTR)&display_hook },
        { MUIA_List_ConstructHook, (IPTR)&construct_hook },
        { MUIA_List_DestructHook, (IPTR)&destruct_hook },
        { MUIA_List_AutoVisible, TRUE }
    };

    prepare_hooks();
    obj = VT_SuperNew(cl, obj, ops->ops_AttrList, extra, sizeof extra / sizeof extra[0]);
    if (!obj)
        return NULL;
    data = INST_DATA(cl, obj);
    data->jobs = (VTJobList *)GetTagData(MUIA_VT_Jobs, 0, ops->ops_AttrList);
    return obj;
}

DISPATCHER(BatchList)
{
    struct BatchData *data = NULL;

    if (msg->MethodID != OM_NEW)
        data = INST_DATA(cl, obj);
    switch (msg->MethodID) {
    case OM_NEW:
        return (ULONG)batch_new(cl, obj, (struct opSet *)msg);
    case MUIM_VT_BatchSync:
        return batch_sync(data, obj);
    case MUIM_VT_BatchGetActive:
        return batch_get_active(obj);
    }
    return DoSuperMethodA(cl, obj, msg);
}
DISPATCHER_END

BOOL VT_BatchList_Init(void)
{
    VT_BatchListClass = MUI_CreateCustomClass(NULL, MUIC_List, NULL,
                                              sizeof(struct BatchData),
                                              DISPATCHER_REF(BatchList));
    return (BOOL)(VT_BatchListClass != NULL);
}

void VT_BatchList_Exit(void)
{
    if (!VT_BatchListClass)
        return;
    MUI_DeleteCustomClass(VT_BatchListClass);
    VT_BatchListClass = NULL;
}
