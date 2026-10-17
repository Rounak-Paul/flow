#include "fl_fs.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

static bool is_separator(char c)
{
#if defined(_WIN32)
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

bool fl_fs_join(char *out, size_t cap, const char *dir, const char *name)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!dir || !name) return false;
    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    const bool needs_sep = dir_len > 0 && !is_separator(dir[dir_len - 1]);
    const size_t total = dir_len + (needs_sep ? 1u : 0u) + name_len;
    if (total >= cap) return false;
    memcpy(out, dir, dir_len);
    size_t at = dir_len;
    if (needs_sep) out[at++] = SEPARATOR;
    memcpy(out + at, name, name_len);
    out[total] = '\0';
    return true;
}

static bool flush_to_disk(FILE *file);

bool fl_fs_write_atomic(const char *path, const void *data, size_t size)
{
    if (!path || (size && !data)) return false;
    const size_t len = strlen(path);
    char *temp = malloc(len + 8u);
    if (!temp) return false;
    memcpy(temp, path, len);
    memcpy(temp + len, ".tmp", 5u);

    FILE *file = fl_fs_fopen(temp, "wb");
    bool ok = file != NULL;
    if (ok && size) ok = fwrite(data, 1, size, file) == size;
    if (ok) ok = fflush(file) == 0 && flush_to_disk(file);
    if (file && fclose(file) != 0) ok = false;
    if (ok) ok = fl_fs_rename(temp, path);
    if (!ok) fl_fs_remove(temp);
    free(temp);
    return ok;
}

char *fl_fs_read_file(const char *path, size_t max_size, size_t *out_size)
{
    if (out_size) *out_size = 0;
    const int64_t size = fl_fs_file_size(path);
    if (size < 0 || (uint64_t)size > (uint64_t)max_size || (uint64_t)size >= SIZE_MAX) return NULL;
    FILE *file = fl_fs_fopen(path, "rb");
    if (!file) return NULL;
    char *buffer = malloc((size_t)size + 1u);
    size_t read = buffer ? fread(buffer, 1, (size_t)size, file) : 0;
    fclose(file);
    if (!buffer || read != (size_t)size) {
        free(buffer);
        return NULL;
    }
    buffer[read] = '\0';
    if (out_size) *out_size = read;
    return buffer;
}

#if defined(_WIN32)

#include <io.h>
#include <windows.h>

static bool flush_to_disk(FILE *file)
{
    return _commit(_fileno(file)) == 0;
}

/* Convert UTF-8 to a heap-allocated wide string; caller frees. */
static wchar_t *widen(const char *utf8)
{
    if (!utf8) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *wide = malloc((size_t)count * sizeof(wchar_t));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static bool directory_exists_w(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool fl_fs_mkdir_p(const char *path)
{
    wchar_t *wide = widen(path);
    if (!wide || !wide[0]) {
        free(wide);
        return false;
    }
    for (wchar_t *p = wide + 1; *p; ++p) {
        if (*p != L'\\' && *p != L'/') continue;
        if (p[-1] == L':') continue;
        wchar_t saved = *p;
        *p = L'\0';
        if (!directory_exists_w(wide) && !CreateDirectoryW(wide, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            free(wide);
            return false;
        }
        *p = saved;
    }
    bool ok = directory_exists_w(wide) || CreateDirectoryW(wide, NULL) ||
              GetLastError() == ERROR_ALREADY_EXISTS;
    ok = ok && directory_exists_w(wide);
    free(wide);
    return ok;
}

FILE *fl_fs_fopen(const char *path, const char *mode)
{
    wchar_t *wide_path = widen(path);
    wchar_t *wide_mode = widen(mode);
    FILE *file = NULL;
    if (wide_path && wide_mode) file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    free(wide_mode);
    return file;
}

bool fl_fs_rename(const char *from, const char *to)
{
    wchar_t *wide_from = widen(from);
    wchar_t *wide_to = widen(to);
    bool ok = wide_from && wide_to &&
              MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wide_from);
    free(wide_to);
    return ok;
}

bool fl_fs_remove(const char *path)
{
    wchar_t *wide = widen(path);
    if (!wide) return false;
    bool ok = DeleteFileW(wide) || GetLastError() == ERROR_FILE_NOT_FOUND ||
              GetLastError() == ERROR_PATH_NOT_FOUND;
    free(wide);
    return ok;
}

bool fl_fs_exists(const char *path)
{
    wchar_t *wide = widen(path);
    if (!wide) return false;
    bool ok = GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
    free(wide);
    return ok;
}

int64_t fl_fs_file_size(const char *path)
{
    wchar_t *wide = widen(path);
    if (!wide) return -1;
    WIN32_FILE_ATTRIBUTE_DATA data;
    BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
    free(wide);
    if (!ok || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return (int64_t)(((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow);
}

#else

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static bool flush_to_disk(FILE *file)
{
    return fsync(fileno(file)) == 0;
}

static bool directory_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool fl_fs_mkdir_p(const char *path)
{
    if (!path || !path[0]) return false;
    size_t len = strlen(path);
    char *copy = malloc(len + 1u);
    if (!copy) return false;
    memcpy(copy, path, len + 1u);
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            free(copy);
            return false;
        }
        *p = '/';
    }
    bool ok = (mkdir(copy, 0755) == 0 || errno == EEXIST) && directory_exists(copy);
    free(copy);
    return ok;
}

FILE *fl_fs_fopen(const char *path, const char *mode)
{
    if (!path || !mode) return NULL;
    return fopen(path, mode);
}

bool fl_fs_rename(const char *from, const char *to)
{
    return from && to && rename(from, to) == 0;
}

bool fl_fs_remove(const char *path)
{
    return path && (unlink(path) == 0 || errno == ENOENT);
}

bool fl_fs_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

int64_t fl_fs_file_size(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

#endif
