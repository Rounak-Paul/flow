#include "fl_log_file.h"

#include "core/fl_thread.h"
#include "fl_fs.h"
#include "fl_paths.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_ROTATE_BYTES (8ull * 1024ull * 1024ull)
#define DEFAULT_KEEP_FILES   3u

typedef struct LogFile {
    Fl_Mutex   lock;
    FILE      *file;
    Fl_LogSink sink;
    char       path[FL_PATH_MAX];
    uint64_t   bytes;
    uint64_t   rotate_bytes;
    uint32_t   keep_files;
} LogFile;

/* Guards open/close and the path; the sink callback itself runs under the logger lock. */
static LogFile g_file = { .lock = FL_MUTEX_INITIALIZER };

static bool rotated_path(char *out, size_t cap, const char *path, uint32_t index)
{
    int n = snprintf(out, cap, "%s.%u", path, index);
    return n > 0 && (size_t)n < cap;
}

/* Shift path.N-1 → path.N … path → path.1, dropping the oldest. */
static void rotate_files(const char *path, uint32_t keep)
{
    char from[FL_PATH_MAX], to[FL_PATH_MAX];
    if (rotated_path(to, sizeof(to), path, keep)) fl_fs_remove(to);
    for (uint32_t i = keep; i > 1u; --i) {
        if (rotated_path(from, sizeof(from), path, i - 1u) &&
            rotated_path(to, sizeof(to), path, i) && fl_fs_exists(from))
            fl_fs_rename(from, to);
    }
    if (keep > 0u && rotated_path(to, sizeof(to), path, 1u)) fl_fs_rename(path, to);
    else fl_fs_remove(path);
}

static FILE *open_append(const char *path, uint64_t *out_bytes)
{
    FILE *file = fl_fs_fopen(path, "ab");
    if (!file) return NULL;
    int64_t size = fl_fs_file_size(path);
    *out_bytes = size > 0 ? (uint64_t)size : 0u;
    return file;
}

static void file_write(const Fl_LogRecord *record, void *user_data)
{
    LogFile *log = user_data;
    if (!log->file) return;

    char header[192];
    int header_len = fl_log_format_prefix(record, header, sizeof(header));
    if (header_len < 0) return;

    int written = record->level >= FL_LOG_WARN
        ? fprintf(log->file, "%s%s (%s:%d)\n", header, record->message, record->file, record->line)
        : fprintf(log->file, "%s%s\n", header, record->message);
    if (written > 0) log->bytes += (uint64_t)written;
    if (record->level >= FL_LOG_INFO) fflush(log->file);

    if (log->bytes > log->rotate_bytes) {
        fclose(log->file);
        rotate_files(log->path, log->keep_files);
        log->file = open_append(log->path, &log->bytes);
    }
}

static void file_flush(void *user_data)
{
    LogFile *log = user_data;
    if (log->file) fflush(log->file);
}

bool fl_log_file_open(const Fl_LogFileDesc *desc)
{
    if (!desc || !desc->dir || !desc->dir[0]) return false;
    fl_log_file_close();

    char path[FL_PATH_MAX];
    const char *name = desc->name && desc->name[0] ? desc->name : "flow.log";
    if (!fl_fs_mkdir_p(desc->dir) || !fl_fs_join(path, sizeof(path), desc->dir, name))
        return false;

    const uint64_t rotate_bytes = desc->rotate_bytes ? desc->rotate_bytes : DEFAULT_ROTATE_BYTES;
    const uint32_t keep_files = desc->keep_files ? desc->keep_files : DEFAULT_KEEP_FILES;
    int64_t existing = fl_fs_file_size(path);
    if (existing > 0 && (uint64_t)existing > rotate_bytes) rotate_files(path, keep_files);

    uint64_t bytes = 0;
    FILE *file = open_append(path, &bytes);
    if (!file) return false;

    fl_mutex_lock(&g_file.lock);
    g_file.file = file;
    g_file.bytes = bytes;
    g_file.rotate_bytes = rotate_bytes;
    g_file.keep_files = keep_files;
    memcpy(g_file.path, path, strlen(path) + 1u);
    g_file.sink = fl_log_add_sink(&(Fl_LogSinkDesc){
        .write = file_write, .flush = file_flush, .user_data = &g_file,
        .min_level = desc->min_level,
    });
    const bool attached = g_file.sink.id != 0;
    if (!attached) {
        fclose(g_file.file);
        g_file.file = NULL;
        g_file.path[0] = '\0';
    }
    fl_mutex_unlock(&g_file.lock);
    return attached;
}

void fl_log_file_close(void)
{
    fl_mutex_lock(&g_file.lock);
    if (g_file.sink.id) fl_log_remove_sink(g_file.sink);
    g_file.sink = (Fl_LogSink){0};
    if (g_file.file) fclose(g_file.file);
    g_file.file = NULL;
    g_file.path[0] = '\0';
    fl_mutex_unlock(&g_file.lock);
}

bool fl_log_file_path(char *out, size_t cap)
{
    if (!out || !cap) return false;
    fl_mutex_lock(&g_file.lock);
    size_t len = strlen(g_file.path);
    bool ok = len > 0 && len < cap;
    if (ok) memcpy(out, g_file.path, len + 1u);
    else out[0] = '\0';
    fl_mutex_unlock(&g_file.lock);
    return ok;
}
