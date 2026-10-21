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
static bool sync_parent(const char *path);

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
    if (ok) ok = fl_fs_flush(file);
    if (file && fclose(file) != 0) ok = false;
    if (ok) ok = fl_fs_commit(temp, path);
    if (!ok) fl_fs_remove(temp);
    free(temp);
    return ok;
}

bool fl_fs_flush(FILE *file)
{
    return file && fflush(file) == 0 && flush_to_disk(file);
}

bool fl_fs_commit(const char *from, const char *path)
{
    return fl_fs_rename(from, path) && sync_parent(path);
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

bool fl_fs_list_dir(const char *path, Fl_FsEntryFn fn, void *user_data)
{
    if (!path || !fn) return false;
    char pattern[4096];
    if (!fl_fs_join(pattern, sizeof(pattern), path, "*")) return false;
    wchar_t *wide = widen(pattern);
    if (!wide) return false;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(wide, &entry);
    free(wide);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    char name[1024];
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        if (WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, name, (int)sizeof(name), NULL, NULL) <= 0) continue;
        if (!fn(name, (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user_data)) break;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return true;
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

bool fl_fs_modified_ns(const char *path, int64_t *out)
{
    wchar_t *wide = widen(path);
    if (!wide || !out) {free(wide); return false;}
    WIN32_FILE_ATTRIBUTE_DATA data;
    BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
    free(wide);
    if (!ok || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *out = (int64_t)((((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime) * 100u);
    return true;
}

#else

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
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

bool fl_fs_list_dir(const char *path, Fl_FsEntryFn fn, void *user_data)
{
    if (!path || !fn) return false;
    DIR *dir = opendir(path);
    if (!dir) return false;
    char child[4096];
    for (struct dirent *entry; (entry = readdir(dir)) != NULL; ) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        bool directory = false;
#if defined(DT_DIR)
        if (entry->d_type == DT_DIR) directory = true;
        else if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK)
#endif
            directory = fl_fs_join(child, sizeof(child), path, name) && directory_exists(child);
        if (!fn(name, directory, user_data)) break;
    }
    closedir(dir);
    return true;
}

int64_t fl_fs_file_size(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

bool fl_fs_modified_ns(const char *path, int64_t *out)
{
    struct stat st;
    if (!path || !out || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
#if defined(__APPLE__)
    const long nanoseconds = st.st_mtimespec.tv_nsec;
#else
    const long nanoseconds = st.st_mtim.tv_nsec;
#endif
    *out = (int64_t)st.st_mtime * 1000000000 + nanoseconds;
    return true;
}

#endif

/* Flush the renamed entry so file replacement survives a host crash. */
static bool sync_parent(const char *path)
{
#ifdef _WIN32
    (void)path; return true; /* MoveFileExW already requests WRITE_THROUGH. */
#else
    size_t size = strlen(path) + 1;
    char *parent = malloc(size);
    if (!parent) return false;
    memcpy(parent, path, size);
    char *slash = strrchr(parent, '/');
    if (!slash) { free(parent); parent = NULL; }
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    int fd = open(parent ? parent : ".", O_RDONLY);
    bool ok = fd >= 0 && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    free(parent); return ok;
#endif
}

bool fl_fs_make_private(const char *path)
{
#if defined(_WIN32)
    (void)path;
    return true;
#else
    return chmod(path, S_IRUSR | S_IWUSR) == 0;
#endif
}

bool fl_fs_absolute(const char *path, char *out, size_t cap)
{
    if (!path || !out || !cap) return false;
    out[0] = 0;
#ifdef _WIN32
    wchar_t *wide = widen(path);
    wchar_t *absolute = wide ? _wfullpath(NULL, wide, 0) : NULL;
    free(wide);
    int size = absolute ? WideCharToMultiByte(CP_UTF8, 0, absolute, -1, NULL, 0, NULL, NULL) : 0;
    bool ok = size > 0 && (size_t)size <= cap &&
        WideCharToMultiByte(CP_UTF8, 0, absolute, -1, out, size, NULL, NULL) != 0;
    free(absolute); return ok;
#else
    char *absolute = realpath(path, NULL);
    if (!absolute) return false;
    size_t size = strlen(absolute) + 1;
    bool ok = size <= cap;
    if (ok) memcpy(out, absolute, size);
    free(absolute); return ok;
#endif
}

bool fl_fs_mkdir_exclusive(const char *path)
{
    if (!path || !path[0]) return false;
#ifdef _WIN32
    wchar_t *wide=widen(path); bool ok=wide && CreateDirectoryW(wide,NULL); free(wide); return ok;
#else
    return mkdir(path,0700)==0;
#endif
}

typedef struct Fl_RemoveTree {const char *path; unsigned depth; bool ok;} Fl_RemoveTree;
static bool remove_tree(const char *path, unsigned depth);
/* Delete one enumerated child; recursive entry checks links again rather than trusting followed metadata. */
static bool remove_tree_entry(const char *name, bool directory, void *data)
{
    (void)directory; Fl_RemoveTree *tree=data;
    size_t size=strlen(tree->path)+strlen(name)+2; char *child=malloc(size);
    tree->ok=child && fl_fs_join(child,size,tree->path,name) && remove_tree(child,tree->depth+1);
    free(child); return tree->ok;
}
/* Bounded traversal that unlinks links instead of descending into their targets. */
static bool remove_tree(const char *path, unsigned depth)
{
    if (depth>64) return false;
#ifdef _WIN32
    wchar_t *wide=widen(path); if (!wide) return false;
    DWORD attributes=GetFileAttributesW(wide);
    if (attributes==INVALID_FILE_ATTRIBUTES) {DWORD error=GetLastError(); free(wide); return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;}
    bool directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    if (!directory || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        bool ok=directory ? RemoveDirectoryW(wide)!=0 : DeleteFileW(wide)!=0; free(wide); return ok;
    }
    Fl_RemoveTree tree={path,depth,true}; bool ok=fl_fs_list_dir(path,remove_tree_entry,&tree) && tree.ok && RemoveDirectoryW(wide);
    free(wide); return ok;
#else
    struct stat status;
    if (lstat(path,&status)) return errno==ENOENT;
    if (!S_ISDIR(status.st_mode)) return unlink(path)==0;
    Fl_RemoveTree tree={path,depth,true};
    return fl_fs_list_dir(path,remove_tree_entry,&tree) && tree.ok && rmdir(path)==0;
#endif
}

bool fl_fs_remove_tree(const char *path)
{
    return path && path[0] && strcmp(path,"/") && strcmp(path,".") && strcmp(path,"..") && remove_tree(path,0);
}
