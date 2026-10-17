#include "fl_paths.h"

#include "fl_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_portable_subdirs[FL_PATH_KIND_COUNT] = {
    "config", "data", "cache", "logs", "logs/crashes",
};

/* Write a formatted path into out; false (and empty out) when it does not fit. */
static bool format_path(char *out, size_t cap, const char *fmt, const char *a, const char *b)
{
    int n = snprintf(out, cap, fmt, a, b);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) out[0] = '\0';
        return false;
    }
    return true;
}

bool fl_paths_is_portable(void)
{
    char dir[FL_PATH_MAX], marker[FL_PATH_MAX];
    return fl_paths_executable_dir(dir, sizeof(dir)) &&
           fl_fs_join(marker, sizeof(marker), dir, FL_PORTABLE_MARKER) &&
           fl_fs_exists(marker);
}

static bool os_user_dir(Fl_PathKind kind, char *out, size_t cap);

bool fl_paths_user_dir(Fl_PathKind kind, char *out, size_t cap)
{
    if (!out || !cap || (unsigned)kind >= (unsigned)FL_PATH_KIND_COUNT) return false;
    out[0] = '\0';

    bool resolved;
    if (fl_paths_is_portable()) {
        char dir[FL_PATH_MAX];
        resolved = fl_paths_executable_dir(dir, sizeof(dir)) &&
                   format_path(out, cap, "%s/flow-data/%s", dir, k_portable_subdirs[kind]);
    } else {
        resolved = os_user_dir(kind, out, cap);
    }
    if (!resolved || !fl_fs_mkdir_p(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

#if defined(_WIN32)

#include <windows.h>

static bool narrow(const wchar_t *wide, char *out, size_t cap)
{
    if (cap > (size_t)INT32_MAX) cap = (size_t)INT32_MAX;
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) > 0;
}

bool fl_paths_executable_dir(char *out, size_t cap)
{
    if (!out || !cap) return false;
    DWORD wide_cap = MAX_PATH;
    wchar_t *wide = NULL;
    for (;;) {
        wchar_t *grown = realloc(wide, (size_t)wide_cap * sizeof(wchar_t));
        if (!grown) {
            free(wide);
            return false;
        }
        wide = grown;
        DWORD len = GetModuleFileNameW(NULL, wide, wide_cap);
        if (len == 0) {
            free(wide);
            return false;
        }
        if (len < wide_cap) break;
        if (wide_cap > 32768u) {
            free(wide);
            return false;
        }
        wide_cap *= 2u;
    }
    wchar_t *slash = wcsrchr(wide, L'\\');
    if (slash) *slash = L'\0';
    bool ok = narrow(wide, out, cap);
    free(wide);
    return ok;
}

static bool env_dir(const wchar_t *name, char *out, size_t cap)
{
    wchar_t wide[FL_PATH_MAX];
    DWORD len = GetEnvironmentVariableW(name, wide, FL_PATH_MAX);
    return len > 0 && len < FL_PATH_MAX && narrow(wide, out, cap);
}

static bool os_user_dir(Fl_PathKind kind, char *out, size_t cap)
{
    static const char *const sub[FL_PATH_KIND_COUNT] = {
        "Flow\\config", "Flow\\data", "Flow\\cache", "Flow\\logs", "Flow\\crashes",
    };
    char base[FL_PATH_MAX];
    if (!env_dir(kind == FL_PATH_CONFIG ? L"APPDATA" : L"LOCALAPPDATA", base, sizeof(base)))
        return false;
    return format_path(out, cap, "%s\\%s", base, sub[kind]);
}

#else

#include <pwd.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

bool fl_paths_executable_dir(char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    char raw[FL_PATH_MAX];
#if defined(__APPLE__)
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) return false;
#else
    ssize_t len = readlink("/proc/self/exe", raw, sizeof(raw) - 1u);
    if (len <= 0) return false;
    raw[len] = '\0';
#endif
    char resolved[FL_PATH_MAX];
    const char *path = realpath(raw, resolved) ? resolved : raw;
    const char *slash = strrchr(path, '/');
    size_t len_dir = slash ? (size_t)(slash - path) : 0u;
    if (len_dir == 0 && slash) len_dir = 1u;
    if (len_dir >= cap) return false;
    memcpy(out, path, len_dir);
    out[len_dir] = '\0';
    return len_dir > 0;
}

static bool home_dir(char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd pw, *result = NULL;
        char buffer[4096];
        if (getpwuid_r(getuid(), &pw, buffer, sizeof(buffer), &result) != 0 || !result)
            return false;
        home = result->pw_dir;
    }
    return format_path(out, cap, "%s%s", home, "");
}

#if defined(__APPLE__)

static bool os_user_dir(Fl_PathKind kind, char *out, size_t cap)
{
    static const char *const sub[FL_PATH_KIND_COUNT] = {
        "Library/Application Support/Flow/config",
        "Library/Application Support/Flow/data",
        "Library/Caches/Flow",
        "Library/Logs/Flow",
        "Library/Logs/Flow/crashes",
    };
    char home[FL_PATH_MAX];
    return home_dir(home, sizeof(home)) && format_path(out, cap, "%s/%s", home, sub[kind]);
}

#else

/* Resolve an XDG base directory, honouring only absolute overrides as the spec requires. */
static bool xdg_dir(const char *env, const char *fallback, char *out, size_t cap)
{
    const char *value = getenv(env);
    if (value && value[0] == '/') return format_path(out, cap, "%s%s", value, "");
    char home[FL_PATH_MAX];
    return home_dir(home, sizeof(home)) && format_path(out, cap, "%s/%s", home, fallback);
}

static bool os_user_dir(Fl_PathKind kind, char *out, size_t cap)
{
    char base[FL_PATH_MAX];
    switch (kind) {
    case FL_PATH_CONFIG:
        return xdg_dir("XDG_CONFIG_HOME", ".config", base, sizeof(base)) &&
               format_path(out, cap, "%s/%s", base, "flow");
    case FL_PATH_DATA:
        return xdg_dir("XDG_DATA_HOME", ".local/share", base, sizeof(base)) &&
               format_path(out, cap, "%s/%s", base, "flow");
    case FL_PATH_CACHE:
        return xdg_dir("XDG_CACHE_HOME", ".cache", base, sizeof(base)) &&
               format_path(out, cap, "%s/%s", base, "flow");
    case FL_PATH_LOGS:
        return xdg_dir("XDG_STATE_HOME", ".local/state", base, sizeof(base)) &&
               format_path(out, cap, "%s/%s", base, "flow/logs");
    case FL_PATH_CRASHES:
        return xdg_dir("XDG_STATE_HOME", ".local/state", base, sizeof(base)) &&
               format_path(out, cap, "%s/%s", base, "flow/logs/crashes");
    case FL_PATH_KIND_COUNT:
        break;
    }
    return false;
}

#endif
#endif
