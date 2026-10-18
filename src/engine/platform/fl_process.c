#include "fl_process.h"
#include "fl_fs.h"
#include "fl_paths.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#define EXE ".exe"
#define PATH_SEPARATOR ';'
#else
#define EXE ""
#define PATH_SEPARATOR ':'
#endif

const char *fl_ffmpeg_path(void)
{
    static char found[FL_PATH_MAX];
    static bool searched;
    if (searched) return found[0] ? found : NULL;
    searched = true;
    const char *override = getenv("FLOW_FFMPEG");
    if (override && fl_fs_exists(override) && strlen(override) < sizeof(found)) {strcpy(found, override); return found;}
    const char *path = getenv("PATH");
    while (path && *path) {
        const char *end = strchr(path, PATH_SEPARATOR);
        size_t length = end ? (size_t)(end - path) : strlen(path);
        char dir[FL_PATH_MAX], candidate[FL_PATH_MAX];
        if (length && length < sizeof(dir)) {
            memcpy(dir, path, length); dir[length] = 0;
            if (fl_fs_join(candidate, sizeof(candidate), dir, "ffmpeg" EXE) && fl_fs_exists(candidate)) {strcpy(found, candidate); return found;}
        }
        path = end ? end + 1 : NULL;
    }
    static const char *const common[] = {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"};
    for (size_t i = 0; i < sizeof(common) / sizeof(*common); ++i) if (fl_fs_exists(common[i])) {strcpy(found, common[i]); return found;}
    return NULL;
}

bool fl_command_append(char *command, size_t capacity, const char *arg)
{
    size_t used = strlen(command), length = strlen(arg);
    if (used + length * 4 + 4 > capacity) return false;
    char *w = command + used;
    *w++ = ' ';
#if defined(_WIN32)
    *w++ = '"';
    for (size_t i = 0; i < length; ++i) {if (arg[i] == '"') *w++ = '\\'; *w++ = arg[i];}
    *w++ = '"';
#else
    *w++ = '\'';
    for (size_t i = 0; i < length; ++i) {
        if (arg[i] == '\'') {memcpy(w, "'\\''", 4); w += 4;}
        else *w++ = arg[i];
    }
    *w++ = '\'';
#endif
    *w = 0;
    return true;
}

bool fl_command_run(const char *command)
{
    FILE *pipe = popen(command, "r");
    if (!pipe) return false;
    char sink[256];
    while (fread(sink, 1, sizeof(sink), pipe) == sizeof(sink)) {}
    return pclose(pipe) == 0;
}

bool fl_open_path(const char *path)
{
    char command[4096] = "";
#if defined(_WIN32)
    bool built = fl_command_append(command, sizeof(command), "cmd") && fl_command_append(command, sizeof(command), "/c") &&
                 fl_command_append(command, sizeof(command), "start") && fl_command_append(command, sizeof(command), "") &&
                 fl_command_append(command, sizeof(command), path);
#elif defined(__APPLE__)
    bool built = fl_command_append(command, sizeof(command), "open") && fl_command_append(command, sizeof(command), path);
#else
    bool built = fl_command_append(command, sizeof(command), "xdg-open") && fl_command_append(command, sizeof(command), path);
#endif
    return built && fl_command_run(command);
}

FILE *fl_command_open_writer(const char *command)
{
#if defined(_WIN32)
    return popen(command, "wb");
#else
    return popen(command, "w");
#endif
}

bool fl_command_close(FILE *writer)
{
    return writer && pclose(writer) == 0;
}
