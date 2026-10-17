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
 * Replace a file's contents atomically: write a sibling temporary file,
 * flush it to stable storage, then rename it over the destination. Readers
 * see either the old or the new contents, never a partial write.
 *
 * Returns  true on success. Failure before rename preserves the destination;
 *          failure flushing the directory may leave the new file committed.
 */
bool fl_fs_write_atomic(const char *path, const void *data, size_t size);

/*
 * Read a whole file into a NUL-terminated heap buffer (caller frees).
 *
 * max_size  Files larger than this are rejected.
 * out_size  Optional; receives the byte count excluding the terminator.
 * Returns   The buffer, or NULL when missing, unreadable, too large or on
 *           allocation failure.
 */
char *fl_fs_read_file(const char *path, size_t max_size, size_t *out_size);

/*
 * Join two path components with the platform separator.
 *
 * out      Destination buffer.
 * cap      Capacity of out in bytes.
 * Returns  false when the result would not fit; out is then empty.
 */
bool fl_fs_join(char *out, size_t cap, const char *dir, const char *name);

/*
 * Visitor for fl_fs_list_dir.
 *
 * name       Entry name (UTF-8, no directory part); never "." or "..".
 * directory  true when the entry is a directory (symbolic links are followed).
 * user_data  Pointer supplied to fl_fs_list_dir.
 * Returns    false to stop the listing early.
 */
typedef bool (*Fl_FsEntryFn)(const char *name, bool directory, void *user_data);

/*
 * Enumerate a directory's entries in unspecified order.
 *
 * Returns  false when the directory cannot be opened; true otherwise, including
 *          when the visitor stops early.
 */
bool fl_fs_list_dir(const char *path, Fl_FsEntryFn fn, void *user_data);

/*
 * Flush a stream written with fl_fs_fopen to stable storage.
 *
 * Returns  true when both the C buffer and the OS cache were flushed.
 */
bool fl_fs_flush(FILE *file);

/*
 * Rename a fully flushed file over path, then flush the parent directory so
 * the new name survives a host crash.
 *
 * Returns  true when the rename was committed.
 */
bool fl_fs_commit(const char *from, const char *path);

/* Resolve an existing path to an absolute UTF-8 path, writing at most cap bytes. */
bool fl_fs_absolute(const char *path, char *out, size_t cap);
