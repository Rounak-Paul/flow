#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * Encode a rectangle of an RGBA8 image as a PNG, optionally shrinking it with a box filter.
 *
 * rgba, width, height   Tightly packed top-left-origin source pixels.
 * x, y, w, h            Region to keep; clamped to the image, returns NULL if empty.
 * max_width             Shrink so the output is at most this wide (zero keeps the region size; never enlarges).
 * out_size              Receives the PNG byte count.
 * out_width/out_height  Optional; receive the encoded dimensions.
 * Returns               Malloc'd PNG bytes, or NULL on invalid input or allocation failure.
 */
unsigned char *fl_png_encode_region(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, uint32_t max_width, size_t *out_size,
                                    uint32_t *out_width, uint32_t *out_height);
