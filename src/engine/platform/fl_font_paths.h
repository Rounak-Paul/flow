#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * Visitor receiving one installed font file.
 *
 * path       UTF-8 path, borrowed for the duration of the call.
 * user_data  Pointer supplied to fl_font_paths_list.
 */
typedef void (*Fl_FontPathFn)(const char *path, void *user_data);

/*
 * Enumerate installed font files: CoreText on macOS, fontconfig on Linux when
 * available, otherwise (and on Windows) the platform's font directories.
 * Files may repeat; callers deduplicate. Blocking; call from a worker.
 *
 * Returns  Number of paths reported.
 */
size_t fl_font_paths_list(Fl_FontPathFn fn, void *user_data);
