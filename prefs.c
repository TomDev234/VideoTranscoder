#include <stdio.h>
#include <string.h>

#include "prefs.h"
#include "vt_util.h"

void VT_PrefsSetDefaults(VTPrefs *prefs)
{
    if (!prefs)
        return;
    memset(prefs, 0, sizeof *prefs);
    VT_StrCopy(prefs->active_profile, sizeof prefs->active_profile, VT_DEFAULT_PROFILE_NAME);
}

typedef struct PrefsParse {
    VTPrefs *prefs;
    BOOL saw_header;
} PrefsParse;

static BOOL apply_prefs_line(const char *line, APTR user)
{
    PrefsParse *state = (PrefsParse *)user;
    char key[32];
    char value[VT_PATH_LEN];
    LONG number;

    if (!state->saw_header) {
        if (!VT_IsFormatHeader(line, VT_PREFS_MAGIC, VT_PREFS_FORMAT_VERSION))
            return FALSE;
        state->saw_header = TRUE;
        return TRUE;
    }
    if (!VT_ParseKeyValue(line, key, sizeof key, value, sizeof value))
        return FALSE;

    if (VT_StrEq(key, "output_drawer"))
        return VT_StrCopy(state->prefs->output_drawer, sizeof state->prefs->output_drawer, value);
    if (VT_StrEq(key, "active_profile"))
        return VT_StrCopy(state->prefs->active_profile, sizeof state->prefs->active_profile, value);
    if (VT_StrEq(key, "window_valid")) {
        if (!VT_ParseLong(value, &number) || (number != 0 && number != 1))
            return FALSE;
        state->prefs->window_valid = number ? TRUE : FALSE;
        return TRUE;
    }
    if (VT_StrEq(key, "window_left") || VT_StrEq(key, "window_top") ||
        VT_StrEq(key, "window_width") || VT_StrEq(key, "window_height")) {
        if (!VT_ParseLong(value, &number))
            return FALSE;
        if (VT_StrEq(key, "window_left"))
            state->prefs->window_left = number;
        else if (VT_StrEq(key, "window_top"))
            state->prefs->window_top = number;
        else if (VT_StrEq(key, "window_width"))
            state->prefs->window_width = number;
        else
            state->prefs->window_height = number;
        return TRUE;
    }
    return TRUE;
}

BOOL VT_PrefsLoad(VTPrefs *prefs)
{
    char *text = NULL;
    PrefsParse state;

    if (!prefs)
        return FALSE;
    VT_PrefsSetDefaults(prefs);
    if (!VT_ReadTextFile(VT_PREFS_PATH, &text, 4096))
        return FALSE;

    state.prefs = prefs;
    state.saw_header = FALSE;
    if (!VT_ForEachLine(text, apply_prefs_line, &state) || !state.saw_header) {
        VT_FreeText(text);
        VT_PrefsSetDefaults(prefs);
        return FALSE;
    }
    VT_FreeText(text);
    if (prefs->active_profile[0] == '\0')
        VT_StrCopy(prefs->active_profile, sizeof prefs->active_profile, VT_DEFAULT_PROFILE_NAME);
    return TRUE;
}

BOOL VT_PrefsSave(const VTPrefs *prefs)
{
    char text[VT_PATH_LEN + 256];
    int wrote;

    if (!prefs)
        return FALSE;
    if (!VT_IsSingleLine(prefs->output_drawer) || !VT_IsSingleLine(prefs->active_profile))
        return FALSE;
    wrote = snprintf(text, sizeof text,
                     "%s %ld\n"
                     "output_drawer=%s\n"
                     "active_profile=%s\n"
                     "window_valid=%ld\n"
                     "window_left=%ld\n"
                     "window_top=%ld\n"
                     "window_width=%ld\n"
                     "window_height=%ld\n",
                     VT_PREFS_MAGIC,
                     (long)VT_PREFS_FORMAT_VERSION,
                     prefs->output_drawer,
                     prefs->active_profile,
                     prefs->window_valid ? 1L : 0L,
                     (long)prefs->window_left,
                     (long)prefs->window_top,
                     (long)prefs->window_width,
                     (long)prefs->window_height);
    if (wrote < 0 || (ULONG)wrote >= sizeof text)
        return FALSE;
    return VT_WriteTextFileAtomic(VT_PREFS_PATH, text);
}
