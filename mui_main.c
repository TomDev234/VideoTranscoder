/*
 * VideoTranscoder application object.
 * The encode process is started with the program. This loop drains its
 * messages; MUI is only used on this task.
 */
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>

#include "macros.h"
#include "vt_ui.h"
#include "profile.h"
#include "job.h"
#include "prefs.h"
#include "encode_task.h"

int main(void)
{
    VTProfileStore profiles;
    VTJobList jobs;
    VTPrefs prefs;
    Object *app = NULL;
    Object *main_win = NULL;
    Object *profile_win = NULL;
    ULONG signals = 0;
    ULONG encode_sig = 0;
    BOOL ui = FALSE;
    BOOL encoder = FALSE;
    int rc = RETURN_FAIL;

    VT_ProfileStoreInit(&profiles);
    VT_JobListInit(&jobs);
    VT_PrefsSetDefaults(&prefs);
    VT_PrefsLoad(&prefs);

    if (!VT_BatchList_Init() || !VT_ProfileWindow_Init() || !VT_MainWindow_Init()) {
        PutStr("VideoTranscoder: could not create the interface.\n");
        goto cleanup;
    }
    ui = TRUE;
    if (!VT_ProfileStoreLoad(&profiles)) {
        PutStr("VideoTranscoder: not enough memory for profiles.\n");
        goto cleanup;
    }

    profile_win = NewObject(VT_ProfileWindowClass->mcc_Class, NULL,
                            MUIA_VT_Profiles, (ULONG)&profiles,
                            TAG_DONE);
    main_win = NewObject(VT_MainWindowClass->mcc_Class, NULL,
                         MUIA_VT_Jobs, (ULONG)&jobs,
                         MUIA_VT_Profiles, (ULONG)&profiles,
                         MUIA_VT_Prefs, (ULONG)&prefs,
                         MUIA_VT_ProfileWindow, (ULONG)profile_win,
                         TAG_DONE);
    if (!profile_win || !main_win) {
        PutStr("VideoTranscoder: could not build the windows.\n");
        goto cleanup;
    }
    set(profile_win, MUIA_VT_MainWindow, (ULONG)main_win);

    app = ApplicationObject,
        MUIA_Application_Title, "VideoTranscoder",
        MUIA_Application_Version, "$VER: VideoTranscoder 1.0 (13.10.2026)",
        MUIA_Application_Description, "Batch video transcoder",
        MUIA_Application_Base, "VIDEOTRANSCODER",
        MUIA_Application_SingleTask, TRUE,
        MUIA_Application_Window, main_win,
        SubWindow, profile_win,
    End;
    if (!app) {
        PutStr("VideoTranscoder: could not open MUI.\n");
        goto cleanup;
    }

    set(main_win, MUIA_Window_Open, TRUE);
    if (!xget(main_win, MUIA_Window_Open)) {
        PutStr("VideoTranscoder: could not open the window.\n");
        goto cleanup;
    }

    encoder = VT_Encode_Init();
    if (!encoder)
        PutStr("VideoTranscoder: the encoder did not start.\n");
    encode_sig = VT_Encode_Signal();

    rc = RETURN_OK;
    for (;;) {
        char status[160];
        struct MUIP_VT_EncodeNote note;
        ULONG id = DoMethod(app, MUIM_Application_NewInput, &signals);

        status[0] = '\0';
        if (VT_Encode_Drain(&jobs, status, sizeof status)) {
            note.MethodID = MUIM_VT_EncodeNote;
            note.status = status;
            DoMethodA(main_win, (Msg)&note);
        }
        if (id == MUIV_Application_ReturnID_Quit)
            break;
        if (signals)
            Wait(signals | encode_sig);
    }
    if (encoder)
        VT_Encode_Shutdown(&jobs);
    encoder = FALSE;
    DoMethod(main_win, MUIM_VT_SavePrefs);

cleanup:
    if (app)
        MUI_DisposeObject(app);
    else {
        if (main_win)
            MUI_DisposeObject(main_win);
        if (profile_win)
            MUI_DisposeObject(profile_win);
    }
    VT_JobListFree(&jobs);
    VT_ProfileStoreFree(&profiles);
    if (ui) {
        VT_MainWindow_Exit();
        VT_ProfileWindow_Exit();
        VT_BatchList_Exit();
    }
    return rc;
}
