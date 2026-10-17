#include "fl_media.h"
#include "platform/fl_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS (1 << 16)
#define STBI_MALLOC(size) malloc(size)
#define STBI_REALLOC(pointer, size) realloc(pointer, size)
#define STBI_FREE(pointer) free(pointer)
#include <stb_image.h>

/* Compare a signature at offset without reading past size. */
static bool match(const unsigned char *head, size_t size, size_t offset, const char *signature, size_t length)
{
    return offset <= size && length <= size - offset && !memcmp(head + offset, signature, length);
}

bool fl_media_sniff(const unsigned char *h, size_t n, Fl_MediaFormat *out)
{
    static const Fl_MediaFormat png = {FL_ASSET_IMAGE, "png", "PNG"}, jpg = {FL_ASSET_IMAGE, "jpg", "JPEG"},
        gif = {FL_ASSET_IMAGE, "gif", "GIF"}, bmp = {FL_ASSET_IMAGE, "bmp", "BMP"},
        ttf = {FL_ASSET_FONT, "ttf", "TrueType"}, otf = {FL_ASSET_FONT, "otf", "OpenType"},
        ttc = {FL_ASSET_FONT, "ttc", "Font collection"}, mp4 = {FL_ASSET_VIDEO, "mp4", "MP4"},
        mov = {FL_ASSET_VIDEO, "mov", "QuickTime"}, m4a = {FL_ASSET_AUDIO, "m4a", "AAC"},
        mkv = {FL_ASSET_VIDEO, "mkv", "Matroska/WebM"}, avi = {FL_ASSET_VIDEO, "avi", "AVI"},
        wav = {FL_ASSET_AUDIO, "wav", "WAV"}, mp3 = {FL_ASSET_AUDIO, "mp3", "MP3"},
        ogg = {FL_ASSET_AUDIO, "ogg", "Ogg"}, flac = {FL_ASSET_AUDIO, "flac", "FLAC"};
    const Fl_MediaFormat *format = NULL;
    if (!h || !out) return false;
    if (match(h, n, 0, "\x89PNG\r\n\x1a\n", 8)) format = &png;
    else if (match(h, n, 0, "\xff\xd8\xff", 3)) format = &jpg;
    else if (match(h, n, 0, "GIF87a", 6) || match(h, n, 0, "GIF89a", 6)) format = &gif;
    else if (match(h, n, 0, "BM", 2) && n >= 18 && h[14] >= 12 && !h[15] && !h[16] && !h[17]) format = &bmp;
    else if (match(h, n, 0, "\x00\x01\x00\x00", 4) || match(h, n, 0, "true", 4)) format = &ttf;
    else if (match(h, n, 0, "OTTO", 4)) format = &otf;
    else if (match(h, n, 0, "ttcf", 4)) format = &ttc;
    else if (match(h, n, 4, "ftyp", 4)) {
        if (match(h, n, 8, "qt  ", 4)) format = &mov;
        else if (match(h, n, 8, "M4A ", 4) || match(h, n, 8, "M4B ", 4)) format = &m4a;
        else format = &mp4;
    }
    else if (match(h, n, 0, "\x1a\x45\xdf\xa3", 4)) format = &mkv;
    else if (match(h, n, 0, "RIFF", 4) && match(h, n, 8, "AVI ", 4)) format = &avi;
    else if (match(h, n, 0, "RIFF", 4) && match(h, n, 8, "WAVE", 4)) format = &wav;
    else if (match(h, n, 0, "ID3", 3) || (n >= 2 && h[0] == 0xff && (h[1] & 0xe6) == 0xe2)) format = &mp3;
    else if (match(h, n, 0, "OggS", 4)) format = &ogg;
    else if (match(h, n, 0, "fLaC", 4)) format = &flac;
    if (!format) return false;
    *out = *format; return true;
}

bool fl_image_probe(const char *path, uint32_t *width, uint32_t *height)
{
    FILE *file = fl_fs_fopen(path, "rb");
    if (!file) return false;
    int w = 0, h = 0, channels = 0;
    bool ok = stbi_info_from_file(file, &w, &h, &channels) && w > 0 && h > 0;
    fclose(file);
    if (ok) { if (width) *width = (uint32_t)w; if (height) *height = (uint32_t)h; }
    return ok;
}

unsigned char *fl_image_decode(const char *path, uint32_t max_side, uint32_t *width, uint32_t *height)
{
    if (!max_side || !width || !height) return NULL;
    FILE *file = fl_fs_fopen(path, "rb");
    if (!file) return NULL;
    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_file(file, &w, &h, &channels, 4);
    fclose(file);
    if (!pixels || w <= 0 || h <= 0) { stbi_image_free(pixels); return NULL; }
    uint32_t factor = 1;
    while ((uint32_t)w / factor > max_side || (uint32_t)h / factor > max_side) ++factor;
    if (factor == 1) { *width = (uint32_t)w; *height = (uint32_t)h; return pixels; }
    uint32_t dw = (uint32_t)w / factor, dh = (uint32_t)h / factor;
    unsigned char *scaled = malloc((size_t)dw * dh * 4);
    if (!scaled) { stbi_image_free(pixels); return NULL; }
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x) {
            uint64_t sum[4] = {0};
            for (uint32_t sy = 0; sy < factor; ++sy) {
                const unsigned char *row = pixels + (((size_t)(y * factor + sy) * (size_t)w) + (size_t)x * factor) * 4;
                for (uint32_t sx = 0; sx < factor * 4; ++sx) sum[sx & 3] += row[sx];
            }
            uint64_t area = (uint64_t)factor * factor;
            for (unsigned c = 0; c < 4; ++c) scaled[((size_t)y * dw + x) * 4 + c] = (unsigned char)((sum[c] + area / 2) / area);
        }
    stbi_image_free(pixels);
    *width = dw; *height = dh;
    return scaled;
}

void fl_image_free(unsigned char *pixels) { free(pixels); }
