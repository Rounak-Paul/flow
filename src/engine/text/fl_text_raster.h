#pragma once
#include "text/fl_font_catalog.h"
#include <stdint.h>

/* Single-thread rasterizer owning a FreeType library and a small LRU of open faces. */
typedef struct Fl_TextRasterizer Fl_TextRasterizer;
/* Face plus pixel size and the synthesis needed when the face lacks the requested style. */
typedef struct Fl_TextStyle {
    const Fl_FontFace *face;
    double size;
    bool synthetic_bold, synthetic_italic;
    int align;                  /* 0 left, 1 centre, 2 right (Fl_TextAlign) */
} Fl_TextStyle;

/* Create a rasterizer; NULL on failure. */
Fl_TextRasterizer *fl_text_rasterizer_create(void);
/* Close every cached face and free the rasterizer. NULL is safe. */
void fl_text_rasterizer_destroy(Fl_TextRasterizer *rasterizer);
/* Close cached faces, e.g. after the catalogs that own face descriptions changed. */
void fl_text_rasterizer_flush(Fl_TextRasterizer *rasterizer);
/*
 * Rasterize UTF-8 text, word-wrapped at spaces (long words break) and aligned within a width x height box, into an
 * 8-bit coverage mask. Boxes over 4096 px per side are uniformly downscaled.
 *
 * out_width/out_height  Receive the mask dimensions.
 * Returns               malloc'd mask (caller frees), or NULL on invalid input or failure.
 */
unsigned char *fl_text_rasterize(Fl_TextRasterizer *rasterizer, const Fl_TextStyle *style,
                                 const unsigned char *text, size_t length, double width, double height,
                                 uint32_t *out_width, uint32_t *out_height);
