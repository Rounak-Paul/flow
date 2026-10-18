#include "fl_png.h"
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

unsigned char *fl_png_encode_region(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, uint32_t max_width, size_t *out_size,
                                    uint32_t *out_width, uint32_t *out_height)
{
    if (!rgba || !out_size || x >= width || y >= height) return NULL;
    if (w > width - x) w = width - x;
    if (h > height - y) h = height - y;
    if (!w || !h) return NULL;
    uint32_t ow = w, oh = h;
    if (max_width && max_width < w) {
        ow = max_width;
        oh = (uint32_t)(((uint64_t)h * ow + w / 2) / w);
        if (!oh) oh = 1;
    }
    if ((uint64_t)ow * oh > (UINT64_MAX >> 4) || (uint64_t)ow * oh * 4 > SIZE_MAX) return NULL;
    unsigned char *pixels = malloc((size_t)ow * oh * 4);
    if (!pixels) return NULL;
    for (uint32_t oy = 0; oy < oh; ++oy) {
        uint32_t y0 = (uint32_t)((uint64_t)oy * h / oh), y1 = (uint32_t)((uint64_t)(oy + 1) * h / oh);
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t ox = 0; ox < ow; ++ox) {
            uint32_t x0 = (uint32_t)((uint64_t)ox * w / ow), x1 = (uint32_t)((uint64_t)(ox + 1) * w / ow);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t sum[4] = {0};
            for (uint32_t sy = y0; sy < y1; ++sy) for (uint32_t sx = x0; sx < x1; ++sx) {
                const uint8_t *s = rgba + ((size_t)(y + sy) * width + x + sx) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += s[c];
            }
            uint64_t n = (uint64_t)(y1 - y0) * (x1 - x0);
            for (int c = 0; c < 4; ++c) pixels[((size_t)oy * ow + ox) * 4 + c] = (unsigned char)((sum[c] + n / 2) / n);
        }
    }
    int length = 0;
    unsigned char *png = stbi_write_png_to_mem(pixels, (int)ow * 4, (int)ow, (int)oh, 4, &length);
    free(pixels);
    if (!png || length <= 0) {STBIW_FREE(png); return NULL;}
    *out_size = (size_t)length;
    if (out_width) *out_width = ow;
    if (out_height) *out_height = oh;
    return png;
}
