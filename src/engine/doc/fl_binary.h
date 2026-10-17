#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Checked little-endian binary stream. Failed streams remain failed. */
typedef struct Fl_Binary {
    unsigned char *data;
    size_t size, capacity, cursor;
    bool failed;
} Fl_Binary;

/* Append bytes to an owning write stream; data may be NULL only for size zero. */
void fl_binary_write(Fl_Binary *stream, const void *data, size_t size);
/* Read bytes from a bounded borrowed stream into data. */
void fl_binary_read(Fl_Binary *stream, void *data, size_t size);
/* Append an unsigned integer in canonical byte order. */
void fl_binary_u64(Fl_Binary *stream, uint64_t value);
/* Read an unsigned integer; failure returns zero and marks the stream. */
uint64_t fl_binary_get_u64(Fl_Binary *stream);
/* Append a length-prefixed UTF-8 string without its terminator. */
void fl_binary_string(Fl_Binary *stream, const char *text);
/* Read an allocated, terminated string; reject embedded NUL bytes. Caller frees. */
char *fl_binary_get_string(Fl_Binary *stream);
/* Atomically write a versioned, checksummed file with the eight-byte magic. */
bool fl_binary_save(Fl_Binary *stream, const char *path, const char magic[8]);
/* Load and validate a file into an owning stream, positioned after its header. */
bool fl_binary_load(Fl_Binary *stream, const char *path, const char magic[8]);
