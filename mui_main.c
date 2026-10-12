#include <exec/types.h>
#include <libraries/mui.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/muimaster.h>

#include <clib/alib_protos.h>

#include "macros.h"

#define ID_ABOUT 1

LONG __stack = 8192;

struct NewMenu Menu[] =
{
	{ NM_TITLE, "Project"  , 0 ,0,0,(APTR)0            },
	{ NM_ITEM , "About..." ,"?",0,0,(APTR)ID_ABOUT     },
	{ NM_ITEM , NM_BARLABEL, 0 ,0,0,(APTR)0            },
	{ NM_ITEM , "Quit"     ,"Q",0,0,(APTR)MUIV_Application_ReturnID_Quit },
	{ NM_END  , NULL       , 0 ,0,0,(APTR)0            },
};

int main(void)
{
    Object *app;
    static Object *window;
    static Object *okButton;
    static Object *cancelButton;

    /*
     * Create the MUI application and its window.
     */
    app = ApplicationObject,

    MUIA_Application_Title,       (IPTR)"VideoTranscoder",
    MUIA_Application_Version,     (IPTR)"$VER: VideoTranscoder 1.0",
    MUIA_Application_Author,      (IPTR)"Master Tom",
    MUIA_Application_Description, (IPTR)"An easy to use video compressor",
    MUIA_Application_Base,        (IPTR)"VIDEOTRANSCODER",
		MUIA_Application_Menustrip     , MUI_MakeObject(MUIO_MenustripNM,Menu,0),

    SubWindow, window = WindowObject,

               MUIA_Window_Title, (IPTR)"Video Transcoder",
               MUIA_Window_ID,    MAKE_ID('M', 'A', 'I', 'N'),

               WindowContents, VGroup,

               /*
                * Static text.
                */
               Child, TextObject,
               MUIA_Text_Contents,
               (IPTR)"Hello from Video Transcoder!\n\n"
          "This is a simple TextObject containing\n"
          "some sample text.",
               MUIA_Text_PreParse, (IPTR)"\33c",
               End,

               /*
                * Some vertical space between the text and buttons.
                */
               Child, VSpace(8),

               /*
                * Buttons.
                */
               Child, HGroup,

               MUIA_Group_SameSize, TRUE,

               Child, okButton = SimpleButton("_OK"),

                      Child, cancelButton = SimpleButton("_Cancel"),

                             End,

                             End,

                             End,

                             End;


    if (app == NULL)
    {
        return 20;
    }


    /*
     * Pressing the window close gadget terminates the application.
     */
    DoMethod(
        window,
        MUIM_Notify,
        MUIA_Window_CloseRequest,
        TRUE,
        (IPTR)app,
        2,
        MUIM_Application_ReturnID,
        MUIV_Application_ReturnID_Quit
    );


    /*
     * OK button -> quit.
     */
    DoMethod(
        okButton,
        MUIM_Notify,
        MUIA_Pressed,
        FALSE,
        (IPTR)app,
        2,
        MUIM_Application_ReturnID,
        MUIV_Application_ReturnID_Quit
    );


    /*
     * Cancel button -> quit.
     */
    DoMethod(
        cancelButton,
        MUIM_Notify,
        MUIA_Pressed,
        FALSE,
        (IPTR)app,
        2,
        MUIM_Application_ReturnID,
        MUIV_Application_ReturnID_Quit
    );


    /*
     * Open the window.
     */
    set(window, MUIA_Window_Open, TRUE);

    /*
     * Main MUI event loop.
     */
    ULONG signal;
    BOOL quit = false;

    do
    {
        switch (DoMethod(app,MUIM_Application_Input,&signal))
        {
        case MUIV_Application_ReturnID_Quit:
            quit = true;
            break;

            case ID_ABOUT:
            MUI_Request(app, window, 0, NULL, "OK", "VideoTranscoder\n© 2026 Master Tom");
            break;
        }
        if (!quit && signal != 0) Wait(signal);
    }
    while(!quit);


    /*
     * Close the window.
     */
    //set(window, MUIA_Window_Open, FALSE);


    /*
     * Dispose the complete MUI object hierarchy.
     */
    MUI_DisposeObject(app);


    return 0;
}