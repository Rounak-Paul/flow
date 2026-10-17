#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Recommended buffer size for any path produced by this module. */
#define FL_PATH_MAX 4096u

/* Name of the marker file that switches Flow into portable mode. */
#define FL_PORTABLE_MARKER "flow.portable"

typedef enum Fl_PathKind {
    FL_PATH_CONFIG = 0,   /* user settings                         */
    FL_PATH_DATA,         /* durable app data                      */
    FL_PATH_CACHE,        /* disposable caches (shaders, ffmpeg)   */
    FL_PATH_LOGS,         /* log files                             */
    FL_PATH_CRASHES,      /* crash reports and minidumps           */
    FL_PATH_KIND_COUNT
} Fl_PathKind;

/*
 * Directory containing the running executable.
 *
 * Returns  false when the OS cannot report it or it does not fit in cap.
 */
bool fl_paths_executable_dir(char *out, size_t cap);

/*
 * Report whether FL_PORTABLE_MARKER sits next to the executable. In portable
 * mode every user directory lives under <executable dir>/flow-data/.
 */
bool fl_paths_is_portable(void);

/*
 * Resolve a per-user directory for kind and create it if missing.
 *
 *   macOS    ~/Library/Application Support/Flow/{config,data}, ~/Library/Caches/Flow,
 *            ~/Library/Logs/Flow, ~/Library/Logs/Flow/crashes
 *   Linux    $XDG_CONFIG_HOME/flow, $XDG_DATA_HOME/flow, $XDG_CACHE_HOME/flow,
 *            $XDG_STATE_HOME/flow/logs, .../logs/crashes
 *   Windows  %APPDATA%\Flow\config, %LOCALAPPDATA%\Flow\{data,cache,logs,crashes}
 *
 * Returns  false when the location cannot be determined, does not fit, or
 *          cannot be created.
 */
bool fl_paths_user_dir(Fl_PathKind kind, char *out, size_t cap);
