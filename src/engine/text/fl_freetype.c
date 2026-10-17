#include "fl_freetype.h"
#include "platform/fl_fs.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* Stream state: the file plus its current offset, so sequential reads skip seeking and keep stdio's buffer. */
typedef struct Fl_FontStream { FT_StreamRec stream; FILE *file; unsigned long position; } Fl_FontStream;

/* FreeType stream reader; a zero count is a seek that returns non-zero on failure. */
static unsigned long stream_read(FT_Stream stream, unsigned long offset, unsigned char *buffer, unsigned long count)
{
    Fl_FontStream *s = (Fl_FontStream *)stream;
    if (offset != s->position) {
        if (offset > (unsigned long)LONG_MAX || fseek(s->file, (long)offset, SEEK_SET) != 0) return count ? 0 : 1;
        s->position = offset;
    }
    if (!count) return 0;
    unsigned long read = (unsigned long)fread(buffer, 1, count, s->file);
    s->position += read;
    if (read < count) { clearerr(s->file); s->position = ULONG_MAX; }
    return read;
}

/* Close the file and release the stream when FreeType discards the face. */
static void stream_close(FT_Stream stream)
{
    Fl_FontStream *s = (Fl_FontStream *)stream;
    if (s->file) fclose(s->file);
    free(s);
}

FT_Error fl_freetype_open(FT_Library library, const char *path, const unsigned char *data, size_t size,
                          long index, FT_Face *out)
{
    *out = NULL;
    if (!library) return FT_Err_Invalid_Library_Handle;
    if (!path) {
        if (!data || !size || size > (size_t)LONG_MAX) return FT_Err_Invalid_Argument;
        return FT_New_Memory_Face(library, data, (FT_Long)size, index, out);
    }
    int64_t length = fl_fs_file_size(path);
    if (length <= 0 || (uint64_t)length > (uint64_t)LONG_MAX) return FT_Err_Cannot_Open_Resource;
    Fl_FontStream *s = calloc(1, sizeof(*s));
    if (!s) return FT_Err_Out_Of_Memory;
    if (!(s->file = fl_fs_fopen(path, "rb"))) { free(s); return FT_Err_Cannot_Open_Resource; }
    FT_Stream stream = &s->stream;
    stream->size = (unsigned long)length;
    stream->descriptor.pointer = s->file;
    stream->read = stream_read;
    stream->close = stream_close;
    FT_Open_Args args = {.flags = FT_OPEN_STREAM, .stream = stream};
    /* FreeType invokes stream_close on failure as well as on FT_Done_Face. */
    return FT_Open_Face(library, &args, index, out);
}
