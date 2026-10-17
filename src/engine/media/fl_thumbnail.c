#include "fl_thumbnail.h"
#include "core/fl_sha256.h"
#include "doc/fl_binary.h"
#include "media/fl_media.h"
#include "platform/fl_fs.h"
#include "platform/fl_video_frame.h"
#include "text/fl_text_raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_MAGIC "FLTHUMB\0"

bool fl_thumbnail_source(const Fl_Project *project, const Fl_Asset *asset, Fl_ThumbnailSource *out)
{
    const char *directory = fl_project_directory(project);
    if (!asset || !out || !directory || strlen(directory) >= sizeof(out->directory)) return false;
    *out = (Fl_ThumbnailSource){.kind = asset->kind};
    snprintf(out->directory, sizeof(out->directory), "%s", directory);
    memcpy(out->hash, asset->hash, 32);
    snprintf(out->extension, sizeof(out->extension), "%s", asset->extension);
    return true;
}

/* Build .flow/cache/thumbnails/<hex>-<side>.flthumb for a source. */
static bool cache_path(const Fl_ThumbnailSource *s, uint32_t side, char *out, size_t capacity)
{
    char hex[65], leaf[96], folder[FL_PATH_MAX];
    fl_sha256_hex(s->hash, hex);
    snprintf(leaf, sizeof(leaf), "%s-%u.flthumb", hex, side);
    return fl_fs_join(folder, sizeof(folder), s->directory, ".flow/cache/thumbnails") && fl_fs_mkdir_p(folder) &&
           fl_fs_join(out, capacity, folder, leaf);
}

/* Load a cached preview, rejecting anything that does not exactly match its header. */
static unsigned char *load_cached(const char *path, uint32_t side, uint32_t *w, uint32_t *h)
{
    Fl_Binary b;
    if (!fl_binary_load(&b, path, CACHE_MAGIC, 1, NULL)) return NULL;
    uint64_t width = fl_binary_get_u64(&b), height = fl_binary_get_u64(&b);
    unsigned char *pixels = NULL;
    if (!b.failed && width && height && width <= side && height <= side && (b.size - b.cursor) == width * height * 4) {
        pixels = malloc((size_t)(width * height * 4));
        if (pixels) fl_binary_read(&b, pixels, (size_t)(width * height * 4));
        if (b.failed) { free(pixels); pixels = NULL; }
    }
    free(b.data);
    if (pixels) { *w = (uint32_t)width; *h = (uint32_t)height; }
    return pixels;
}

/* Persist a preview; failures only cost a regeneration later. */
static void store_cached(const char *path, const unsigned char *pixels, uint32_t w, uint32_t h)
{
    Fl_Binary b = {0};
    fl_binary_u64(&b, w); fl_binary_u64(&b, h); fl_binary_write(&b, pixels, (size_t)w * h * 4);
    fl_binary_save(&b, path, CACHE_MAGIC, 1);
    free(b.data);
}

/* Render "Aa" in the font's first face as white ink, cropped to the inked bounds. */
static unsigned char *font_specimen(const char *path, uint32_t side, uint32_t *w, uint32_t *h)
{
    Fl_FontCatalog *catalog = fl_font_catalog_from_files(&path, 1);
    const Fl_FontFace *face = fl_font_catalog_match(catalog, fl_font_catalog_family(catalog, 0), 400, false);
    Fl_TextRasterizer *r = face ? fl_text_rasterizer_create() : NULL;
    uint32_t mw = 0, mh = 0;
    unsigned char *mask = r ? fl_text_rasterize(r, &(Fl_TextStyle){face, side * 0.55, false, false},
                                                (const unsigned char *)"Aa", 2, side, side, &mw, &mh) : NULL;
    fl_text_rasterizer_destroy(r);
    fl_font_catalog_destroy(catalog);
    if (!mask) return NULL;
    uint32_t x0 = mw, y0 = mh, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < mh; ++y) for (uint32_t x = 0; x < mw; ++x)
        if (mask[(size_t)y * mw + x]) { if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
    unsigned char *pixels = NULL;
    if (x0 <= x1 && y0 <= y1) {
        uint32_t cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        pixels = malloc((size_t)cw * ch * 4);
        if (pixels) {
            for (uint32_t y = 0; y < ch; ++y) for (uint32_t x = 0; x < cw; ++x) {
                unsigned char *p = pixels + ((size_t)y * cw + x) * 4;
                p[0] = p[1] = p[2] = 255; p[3] = mask[(size_t)(y + y0) * mw + x + x0];
            }
            *w = cw; *h = ch;
        }
    }
    free(mask);
    return pixels;
}

unsigned char *fl_thumbnail_get(const Fl_ThumbnailSource *s, uint32_t side, uint32_t *w, uint32_t *h)
{
    if (!s || !side || !w || !h || s->kind == FL_ASSET_AUDIO) return NULL;
    char cache[FL_PATH_MAX], file[FL_PATH_MAX];
    bool cacheable = cache_path(s, side, cache, sizeof(cache));
    unsigned char *pixels = cacheable ? load_cached(cache, side, w, h) : NULL;
    if (pixels) return pixels;
    if (!fl_asset_store_path(s->directory, s->hash, s->extension, file, sizeof(file))) return NULL;
    if (s->kind == FL_ASSET_IMAGE) pixels = fl_image_decode(file, side, w, h);
    else if (s->kind == FL_ASSET_VIDEO) pixels = fl_video_frame(file, side, w, h, NULL, NULL);
    else if (s->kind == FL_ASSET_FONT) pixels = font_specimen(file, side, w, h);
    if (pixels && cacheable) store_cached(cache, pixels, *w, *h);
    return pixels;
}
