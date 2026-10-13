/*
 * Session prefs in ENVARC:VideoTranscoder.prefs.
 * No network, no licence, no statistics. Window size is stored with
 * the position so the main window can reopen where the user left it.
 *
 *   VTPREFS 1
 *   output_drawer, active_profile, window_valid, window_left, window_top,
 *   window_width, window_height
 */
#ifndef VT_PREFS_H
#define VT_PREFS_H

#include "vt_types.h"

#define VT_PREFS_FORMAT_VERSION 1L
#define VT_PREFS_MAGIC          "VTPREFS"
#define VT_PREFS_PATH           "ENVARC:VideoTranscoder.prefs"

typedef struct VTPrefs {
    char output_drawer[VT_PATH_LEN];
    char active_profile[VT_NAME_LEN];
    BOOL window_valid;
    LONG window_left;
    LONG window_top;
    LONG window_width;
    LONG window_height;
} VTPrefs;

void VT_PrefsSetDefaults(VTPrefs *prefs);

/* *prefs is always filled. TRUE means a version-1 file was read. */
BOOL VT_PrefsLoad(VTPrefs *prefs);
BOOL VT_PrefsSave(const VTPrefs *prefs);

#endif
