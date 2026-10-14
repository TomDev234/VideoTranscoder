/*
 * Freeware notice and credits for the statically linked libraries.
 * No links and no update check.
 */
#include <emul/emulregs.h>
#include <clib/alib_protos.h>

#include <stdio.h>

#include "macros.h"
#include "vt_ui.h"

struct MUI_CustomClass *VT_AboutWindowClass;

struct AboutData {
    char text[1024];
};

static Object *about_new(struct IClass *cl, Object *obj, struct opSet *ops)
{
    struct AboutData *data;
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
    set(close_button, MUIA_Weight, 0);
    root = VGroup,
        Child, body,
        Child, RectangleObject,
            MUIA_Rectangle_HBar, TRUE,
            MUIA_FixHeight, 2,
            MUIA_Weight, 0,
        End,
        Child, HGroup,
            Child, RectangleObject, MUIA_Weight, 100, End,
            Child, close_button,
            Child, RectangleObject, MUIA_Weight, 100, End,
        End,
    End;
    if (!root)
        return NULL;

    extra[extra_count].ti_Tag = MUIA_Window_Title;
    extra[extra_count++].ti_Data = (IPTR)"About VideoTranscoder";
    extra[extra_count].ti_Tag = MUIA_Window_RootObject;
    extra[extra_count++].ti_Data = (IPTR)root;
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
    snprintf(data->text, sizeof data->text,
             "VideoTranscoder %s\n"
             "\n"
             "Convert videos like a pro.\n"
             "Freeware. This program stays offline. It does not phone home and it does not check for updates.\n"
             "The VideoTranscoder binary is free software under the GNU General Public License because it is linked with libx264.\n"
             "\n"
             "Credits for the statically linked libraries:\n"
             "FFmpeg\n"
             "LAME\n"
             "libx264 (GPL)\n",
             VT_VERSION_TEXT);
    set(body, MUIA_Text_Contents, data->text);
    DoMethod(close_button, MUIM_Notify, MUIA_Pressed, FALSE,
             obj, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(obj, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             obj, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    return obj;
}

DISPATCHER(AboutWindow)
{
    switch (msg->MethodID) {
    case OM_NEW:
        return (ULONG)about_new(cl, obj, (struct opSet *)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}
DISPATCHER_END

BOOL VT_AboutWindow_Init(void)
{
    VT_AboutWindowClass = MUI_CreateCustomClass(NULL, MUIC_Window, NULL,
                                                sizeof(struct AboutData),
                                                DISPATCHER_REF(AboutWindow));
    return (BOOL)(VT_AboutWindowClass != NULL);
}

void VT_AboutWindow_Exit(void)
{
    if (!VT_AboutWindowClass)
        return;
    MUI_DeleteCustomClass(VT_AboutWindowClass);
    VT_AboutWindowClass = NULL;
}
