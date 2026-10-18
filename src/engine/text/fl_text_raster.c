#include "fl_text_raster.h"
#include "fl_freetype.h"
#include FT_SYNTHESIS_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FACE_CACHE 16
#define MAX_SIDE 4096.0

typedef struct Fl_CachedFace {
    char *path;
    const unsigned char *data;
    long index;
    FT_Face face;
    uint64_t used;
} Fl_CachedFace;
struct Fl_TextRasterizer {
    FT_Library library;
    Fl_CachedFace faces[FACE_CACHE];
    uint64_t clock;
};

/* Release one cache slot. */
static void evict(Fl_CachedFace *slot)
{
    if (slot->face) FT_Done_Face(slot->face);
    free(slot->path);
    *slot = (Fl_CachedFace){0};
}

/* Return an open FreeType face for desc, reusing or replacing the least recently used slot. */
static FT_Face acquire(Fl_TextRasterizer *r, const Fl_FontFace *desc)
{
    Fl_CachedFace *victim = &r->faces[0];
    for (size_t i = 0; i < FACE_CACHE; ++i) {
        Fl_CachedFace *slot = &r->faces[i];
        if (slot->face && slot->index == desc->index &&
            (desc->path ? slot->path && !strcmp(slot->path, desc->path) : !slot->path && slot->data == desc->data)) {
            slot->used = ++r->clock; return slot->face;
        }
        if (!slot->face || slot->used < victim->used) victim = slot;
    }
    evict(victim);
    char *path = NULL;
    if (desc->path) {
        size_t size = strlen(desc->path) + 1;
        if (!(path = malloc(size))) return NULL;
        memcpy(path, desc->path, size);
    }
    FT_Face face;
    if (fl_freetype_open(r->library, desc->path, desc->data, desc->size, desc->index, &face)) { free(path); return NULL; }
    *victim = (Fl_CachedFace){path, desc->path ? NULL : desc->data, desc->index, face, ++r->clock};
    return face;
}

Fl_TextRasterizer *fl_text_rasterizer_create(void)
{
    Fl_TextRasterizer *r = calloc(1, sizeof(*r));
    if (r && FT_Init_FreeType(&r->library)) { free(r); return NULL; }
    return r;
}

void fl_text_rasterizer_flush(Fl_TextRasterizer *r)
{
    for (size_t i = 0; r && i < FACE_CACHE; ++i) evict(&r->faces[i]);
}

void fl_text_rasterizer_destroy(Fl_TextRasterizer *r)
{
    if (!r) return;
    fl_text_rasterizer_flush(r);
    FT_Done_FreeType(r->library);
    free(r);
}

/* Decode one UTF-8 scalar, consuming malformed input as a replacement glyph. */
static uint32_t next_codepoint(const unsigned char *text, size_t length, size_t *cursor)
{
    uint32_t c = text[(*cursor)++]; if (c < 128) return c;
    unsigned remaining = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
    if (!remaining || remaining > length - *cursor) return 0xfffd;
    uint32_t value = c & ((1u << (6 - remaining)) - 1);
    for (unsigned i = 0; i < remaining; ++i) {
        unsigned char b = text[*cursor]; if ((b & 0xc0) != 0x80) return 0xfffd;
        ++*cursor; value = (value << 6) | (b & 63);
    }
    if ((remaining == 1 && value < 128) || (remaining == 2 && value < 2048) || (remaining == 3 && value < 65536) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0xfffd;
    return value;
}

/* One laid-out character: glyph index, advance and the kerning applied before it. */
typedef struct Fl_Glyph { FT_UInt index; int advance, kern; uint32_t code; } Fl_Glyph;
/* One wrapped line: glyph range [start, end) and its ink width in pixels. */
typedef struct Fl_Line { size_t start, end; int width; } Fl_Line;

unsigned char *fl_text_rasterize(Fl_TextRasterizer *r, const Fl_TextStyle *style,
                                 const unsigned char *text, size_t length, double width, double height,
                                 uint32_t *out_width, uint32_t *out_height)
{
    if (!r || !style || !style->face || (length && !text) || !out_width || !out_height || !isfinite(width) ||
        !isfinite(height) || !isfinite(style->size) || width <= 0 || height <= 0 || style->size < 1) return NULL;
    FT_Face face = acquire(r, style->face);
    if (!face) return NULL;
    double scale = fmin(1.0, MAX_SIDE / fmax(width, height));
    uint32_t w = (uint32_t)fmax(1, ceil(width * scale)), h = (uint32_t)fmax(1, ceil(height * scale));
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)fmin(65535, fmax(1, ceil(style->size * scale))))) return NULL;
    FT_Matrix shear = {0x10000, style->synthetic_italic ? 0x366a : 0, 0, 0x10000};
    FT_Set_Transform(face, &shear, NULL);
    unsigned char *pixels = calloc((size_t)w * h, 1);
    if (!pixels) { FT_Set_Transform(face, NULL, NULL); return NULL; }
    int ascender = (int)(face->size->metrics.ascender >> 6);
    int line_height = (int)(face->size->metrics.height >> 6);
    if (line_height < 1) line_height = 1;
    Fl_Glyph *glyphs = length ? malloc(length * sizeof(*glyphs)) : NULL;
    Fl_Line *lines = malloc((length + 1) * sizeof(*lines));
    size_t glyph_count = 0, line_count = 0;
    if (!lines || (length && !glyphs)) {free(glyphs); free(lines); free(pixels); FT_Set_Transform(face, NULL, NULL); return NULL;}
    FT_UInt previous = 0;
    for (size_t cursor = 0; cursor < length; ) {
        uint32_t code = next_codepoint(text, length, &cursor);
        if (code == '\r') continue;
        Fl_Glyph g = {.code = code};
        if (code == '\n') previous = 0;
        else {
            g.index = FT_Get_Char_Index(face, code == '\t' ? ' ' : code);
            if (!g.index) g.index = FT_Get_Char_Index(face, 0xfffd);
            if (previous && g.index && FT_HAS_KERNING(face)) {
                FT_Vector delta;
                if (!FT_Get_Kerning(face, previous, g.index, FT_KERNING_DEFAULT, &delta)) g.kern = (int)(delta.x >> 6);
            }
            if (!FT_Load_Glyph(face, g.index, FT_LOAD_NO_BITMAP)) {
                if (style->synthetic_bold) FT_GlyphSlot_Embolden(face->glyph);
                g.advance = (int)(face->glyph->advance.x >> 6) * (code == '\t' ? 4 : 1);
            }
            previous = g.index;
        }
        glyphs[glyph_count++] = g;
    }
    /* Greedy word wrap: break after the last space when a word overflows, or inside a word longer than the box. */
    size_t at = 0;
    do {
        size_t start = at, last_space = SIZE_MAX;
        int pen = 0, width_at_space = 0;
        bool wrapped = false;
        Fl_Line line = {.start = start};
        while (at < glyph_count && glyphs[at].code != '\n') {
            const Fl_Glyph *g = &glyphs[at];
            int kern = at == start ? 0 : g->kern;
            if (at > start && pen + kern + g->advance > (int)w && g->code != ' ') {
                if (last_space != SIZE_MAX) {
                    line.end = last_space; line.width = width_at_space; at = last_space + 1; wrapped = true;
                }
                break;
            }
            pen += kern + g->advance;
            if (g->code == ' ') {last_space = at; width_at_space = pen - kern - g->advance;}
            ++at;
        }
        if (!wrapped) {line.end = at; line.width = pen;}
        lines[line_count++] = line;
        if (!wrapped && at < glyph_count && glyphs[at].code == '\n') ++at;
    } while (at < glyph_count);
    int baseline = ascender;
    for (size_t l = 0; l < line_count && baseline - ascender < (int)h; ++l, baseline += line_height) {
        int pen_x = style->align == 1 ? ((int)w - lines[l].width) / 2 : style->align == 2 ? (int)w - lines[l].width : 0;
        for (size_t i = lines[l].start; i < lines[l].end; ++i) {
            const Fl_Glyph *g = &glyphs[i];
            if (i > lines[l].start) pen_x += g->kern;
            if (FT_Load_Glyph(face, g->index, FT_LOAD_NO_BITMAP)) {pen_x += g->advance; continue;}
            FT_GlyphSlot slot = face->glyph;
            if (style->synthetic_bold) FT_GlyphSlot_Embolden(slot);
            if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) {pen_x += g->advance; continue;}
            int pitch = slot->bitmap.pitch;
            for (unsigned y = 0; y < slot->bitmap.rows; ++y) {
                const unsigned char *row = slot->bitmap.buffer + (pitch >= 0 ? y : slot->bitmap.rows - 1 - y) * (size_t)abs(pitch);
                for (unsigned x = 0; x < slot->bitmap.width; ++x) {
                    int dx = pen_x + slot->bitmap_left + (int)x, dy = baseline - slot->bitmap_top + (int)y;
                    if (dx < 0 || dy < 0 || dx >= (int)w || dy >= (int)h) continue;
                    unsigned alpha = slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO ? ((row[x / 8] >> (7 - x % 8)) & 1) * 255 : row[x];
                    size_t offset = (size_t)dy * w + (unsigned)dx;
                    pixels[offset] = (unsigned char)(255 - ((255 - pixels[offset]) * (255 - alpha) + 127) / 255);
                }
            }
            pen_x += g->advance;
        }
    }
    free(glyphs); free(lines);
    FT_Set_Transform(face, NULL, NULL);
    *out_width = w; *out_height = h;
    return pixels;
}
