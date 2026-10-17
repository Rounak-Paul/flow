#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * File-system helpers. Every path is UTF-8 on every platform.
 */

/*
 * Create a directory and any missing parents.
 *
 * Returns  true when the directory exists afterwards.
 */
bool fl_fs_mkdir_p(const char *path);

/*
 * Open a file with fopen semantics.
 *
 * mode     fopen mode string, e.g. "rb", "ab".
 * Returns  Stream, or NULL on failure.
 */
FILE *fl_fs_fopen(const char *path, const char *mode);

/*
 * Rename a file, replacing any existing destination.
 *
 * Returns  true on success.
 */
bool fl_fs_rename(const char *from, const char *to);

/*
 * Delete a file.
 *
 * Returns  true when the file was removed or did not exist.
 */
bool fl_fs_remove(const char *path);

/*
 * Report whether a regular file or directory exists at path.
 */
bool fl_fs_exists(const char *path);

/*
 * Size of a regular file.
 *
 * Returns  Size in bytes, or -1 when the file is missing or unreadable.
 */
int64_t fl_fs_file_size(const char *path);

/*
 * Join two path components with the platform separator.
 *
 * out      Destination buffer.
 * cap      Capacity of out in bytes.
 * Returns  false when the result would not fit; out is then empty.
 */
bool fl_fs_join(char *out, size_t cap, const char *dir, const char *name);
