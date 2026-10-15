/*
 * MUI shell shared by the main window, batch list, and profile editor.
 * Views call the model. They do not call libav.
 */
#ifndef VT_UI_H
#define VT_UI_H

#include <exec/types.h>
#include <intuition/classusr.h>
#include <libraries/mui.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>

#ifndef nnset
#define nnset(obj, attr, value) SetAttrs((obj), MUIA_NoNotify, TRUE, (attr), (value), TAG_DONE)
#endif

#define VT_TAGBASE 0x8E560000u

#define MUIA_VT_Jobs          (VT_TAGBASE + 1)
#define MUIA_VT_Profiles      (VT_TAGBASE + 2)
#define MUIA_VT_Prefs         (VT_TAGBASE + 3)
#define MUIA_VT_ProfileWindow (VT_TAGBASE + 4)
#define MUIA_VT_MainWindow    (VT_TAGBASE + 5)
#define MUIA_VT_JobLogWindow  (VT_TAGBASE + 6)

#define VT_VERSION_TEXT "1.1 (16.10.2026)"
#define VT_APP_VERSION  "$VER: VideoTranscoder " VT_VERSION_TEXT

#define MUIM_VT_BatchSync        (VT_TAGBASE + 20)
#define MUIM_VT_AddFiles         (VT_TAGBASE + 21)
#define MUIM_VT_RemoveJob        (VT_TAGBASE + 22)
#define MUIM_VT_ClearJobs        (VT_TAGBASE + 23)
#define MUIM_VT_Start            (VT_TAGBASE + 24)
#define MUIM_VT_Stop             (VT_TAGBASE + 25)
#define MUIM_VT_NewProfile       (VT_TAGBASE + 26)
#define MUIM_VT_EditProfile      (VT_TAGBASE + 27)
#define MUIM_VT_DuplicateProfile (VT_TAGBASE + 28)
#define MUIM_VT_DeleteProfile    (VT_TAGBASE + 29)
#define MUIM_VT_DrawerChanged    (VT_TAGBASE + 30)
#define MUIM_VT_ProfileCycle     (VT_TAGBASE + 31)
#define MUIM_VT_ProfilesChanged  (VT_TAGBASE + 32)
#define MUIM_VT_SavePrefs        (VT_TAGBASE + 33)
#define MUIM_VT_ProfileUse       (VT_TAGBASE + 34)
#define MUIM_VT_ProfileRefresh   (VT_TAGBASE + 35)
#define MUIM_VT_ProfileSave      (VT_TAGBASE + 36)
#define MUIM_VT_ProfileCancel    (VT_TAGBASE + 37)
#define MUIM_VT_Close            (VT_TAGBASE + 38)
#define MUIM_VT_BatchGetActive   (VT_TAGBASE + 39)
#define MUIM_VT_EncodeNote       (VT_TAGBASE + 40)
#define MUIM_VT_Drop             (VT_TAGBASE + 41)
#define MUIM_VT_ShowLog          (VT_TAGBASE + 42)
#define MUIM_VT_ShowAbout        (VT_TAGBASE + 43)
#define MUIM_VT_JobLogUse        (VT_TAGBASE + 44)
#define MUIM_VT_JobSelect        (VT_TAGBASE + 45)
#define MUIM_VT_ResumeJob        (VT_TAGBASE + 46)
#define MUIM_VT_OpenJobs         (VT_TAGBASE + 47)
#define MUIM_VT_SaveJobs         (VT_TAGBASE + 48)

struct MUIP_VT_ProfilesChanged {
    ULONG MethodID;
    const char *name;
    const char *previous;
};

struct MUIP_VT_ProfileUse {
    ULONG MethodID;
    LONG index;
};

struct MUIP_VT_EncodeNote {
    ULONG MethodID;
    const char *status;
};

struct VTJob;

struct MUIP_VT_JobLogUse {
    ULONG MethodID;
    const struct VTJob *job;
    BOOL open_window;
};

struct MUI_CustomClass;

extern struct MUI_CustomClass *VT_BatchListClass;
extern struct MUI_CustomClass *VT_MainWindowClass;
extern struct MUI_CustomClass *VT_ProfileWindowClass;
extern struct MUI_CustomClass *VT_JobLogWindowClass;

BOOL VT_BatchList_Init(void);
void VT_BatchList_Exit(void);
BOOL VT_MainWindow_Init(void);
void VT_MainWindow_Exit(void);
BOOL VT_ProfileWindow_Init(void);
void VT_ProfileWindow_Exit(void);
BOOL VT_JobLogWindow_Init(void);
void VT_JobLogWindow_Exit(void);

static inline Object *VT_SuperNew(struct IClass *cl, Object *obj, const struct TagItem *user,
                                  const struct TagItem *mine, ULONG mine_count)
{
    struct TagItem tags[32];
    struct opSet ops;
    ULONG n = 0;
    ULONG i;

    if (mine_count > 28)
        return NULL;
    for (i = 0; i < mine_count; i++)
        tags[n++] = mine[i];
    if (user) {
        tags[n].ti_Tag = TAG_MORE;
        tags[n].ti_Data = (IPTR)user;
        n++;
    }
    tags[n].ti_Tag = TAG_END;
    tags[n].ti_Data = 0;
    ops.MethodID = OM_NEW;
    ops.ops_AttrList = tags;
    ops.ops_GInfo = NULL;
    return (Object *)DoSuperMethodA(cl, obj, (Msg)&ops);
}

#endif
