#include "fl_font_paths.h"
#include "fl_fs.h"
#include "fl_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <CoreText/CoreText.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(FLOW_HAS_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

#define SCAN_DEPTH 8

typedef struct Fl_FontScan {
    Fl_FontPathFn fn;
    void *user_data;
    size_t count;
    unsigned depth;
    char directory[FL_PATH_MAX];
} Fl_FontScan;

/* Accept font container extensions FreeType can rasterize, ignoring case. */
static bool font_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || strlen(dot) != 4) return false;
    char ext[4];
    for (size_t i = 0; i < 3; ++i) ext[i] = (char)(dot[i + 1] >= 'A' && dot[i + 1] <= 'Z' ? dot[i + 1] + 32 : dot[i + 1]);
    ext[3] = 0;
    return !strcmp(ext, "ttf") || !strcmp(ext, "otf") || !strcmp(ext, "ttc");
}

static void scan_directory(Fl_FontScan *scan, const char *directory);

/* Visit one directory entry, descending into subdirectories up to SCAN_DEPTH. */
static bool scan_entry(const char *name, bool directory, void *data)
{
    Fl_FontScan *scan = data;
    char path[FL_PATH_MAX];
    if (name[0] == '.' || !fl_fs_join(path, sizeof(path), scan->directory, name)) return true;
    if (directory) {
        if (scan->depth < SCAN_DEPTH) {
            char saved[FL_PATH_MAX];
            memcpy(saved, scan->directory, sizeof(saved));
            ++scan->depth; scan_directory(scan, path); --scan->depth;
            memcpy(scan->directory, saved, sizeof(saved));
        }
    } else if (font_extension(name)) {
        scan->fn(path, scan->user_data); ++scan->count;
    }
    return true;
}

/* Recursively report font files below directory. */
static void scan_directory(Fl_FontScan *scan, const char *directory)
{
    if (strlen(directory) >= sizeof(scan->directory)) return;
    snprintf(scan->directory, sizeof(scan->directory), "%s", directory);
    fl_fs_list_dir(directory, scan_entry, scan);
}

/* Join an environment-provided root with a relative font directory and scan it. */
static void scan_relative(Fl_FontScan *scan, const char *root, const char *relative)
{
    char path[FL_PATH_MAX];
    if (root && root[0] && fl_fs_join(path, sizeof(path), root, relative)) scan_directory(scan, path);
}

/* Scan the conventional per-platform font directories. */
static void scan_known_directories(Fl_FontScan *scan)
{
#if defined(_WIN32)
    wchar_t wide[FL_PATH_MAX];
    char root[FL_PATH_MAX];
    const wchar_t *names[] = {L"WINDIR", L"LOCALAPPDATA"};
    const char *relative[] = {"Fonts", "Microsoft\\Windows\\Fonts"};
    for (size_t i = 0; i < 2; ++i) {
        DWORD length = GetEnvironmentVariableW(names[i], wide, FL_PATH_MAX);
        if (length && length < FL_PATH_MAX &&
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, root, (int)sizeof(root), NULL, NULL) > 0)
            scan_relative(scan, root, relative[i]);
    }
#elif defined(__APPLE__)
    scan_directory(scan, "/System/Library/Fonts");
    scan_directory(scan, "/Library/Fonts");
    scan_relative(scan, getenv("HOME"), "Library/Fonts");
#else
    scan_directory(scan, "/usr/share/fonts");
    scan_directory(scan, "/usr/local/share/fonts");
    const char *data = getenv("XDG_DATA_HOME");
    if (data && data[0]) scan_relative(scan, data, "fonts");
    else scan_relative(scan, getenv("HOME"), ".local/share/fonts");
    scan_relative(scan, getenv("HOME"), ".fonts");
#endif
}

/* Ask the platform font service for every activated font file. */
static void list_native(Fl_FontScan *scan)
{
#if defined(__APPLE__)
    CFArrayRef urls = CTFontManagerCopyAvailableFontURLs();
    if (!urls) return;
    char path[FL_PATH_MAX];
    for (CFIndex i = 0, n = CFArrayGetCount(urls); i < n; ++i) {
        CFURLRef url = (CFURLRef)CFArrayGetValueAtIndex(urls, i);
        if (url && CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, (CFIndex)sizeof(path))) {
            scan->fn(path, scan->user_data); ++scan->count;
        }
    }
    CFRelease(urls);
#elif defined(FLOW_HAS_FONTCONFIG)
    if (!FcInit()) return;
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *objects = FcObjectSetBuild(FC_FILE, (char *)NULL);
    FcFontSet *set = pattern && objects ? FcFontList(NULL, pattern, objects) : NULL;
    for (int i = 0; set && i < set->nfont; ++i) {
        FcChar8 *file = NULL;
        if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) == FcResultMatch && file && font_extension((const char *)file)) {
            scan->fn((const char *)file, scan->user_data); ++scan->count;
        }
    }
    if (set) FcFontSetDestroy(set);
    if (objects) FcObjectSetDestroy(objects);
    if (pattern) FcPatternDestroy(pattern);
#else
    (void)scan;
#endif
}

size_t fl_font_paths_list(Fl_FontPathFn fn, void *user_data)
{
    if (!fn) return 0;
    Fl_FontScan *scan = calloc(1, sizeof(*scan));
    if (!scan) return 0;
    scan->fn = fn; scan->user_data = user_data;
    list_native(scan);
    if (!scan->count) scan_known_directories(scan);
    size_t count = scan->count;
    free(scan);
    return count;
}
