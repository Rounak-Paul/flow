#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stddef.h>

/* Open face index from a UTF-8 path (streamed, never fully loaded) or from borrowed memory when path is NULL. */
FT_Error fl_freetype_open(FT_Library library, const char *path, const unsigned char *data, size_t size,
                          long index, FT_Face *out);
