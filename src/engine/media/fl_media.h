#pragma once
#include "project/fl_project.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Number of leading file bytes fl_media_sniff inspects. */
#define FL_MEDIA_SNIFF_BYTES 32

/* Recognised container/codec family with its canonical stored extension. */
typedef struct Fl_MediaFormat {
    Fl_AssetKind kind;
    const char *extension;
    const char *label;          /* short human-readable format name */
} Fl_MediaFormat;

/* Identify media from leading bytes (signatures only; extensions are untrusted). */
bool fl_media_sniff(const unsigned char *head, size_t size, Fl_MediaFormat *out);
/* Read image dimensions without decoding pixels; false for unsupported or corrupt images. */
bool fl_image_probe(const char *path, uint32_t *width, uint32_t *height);
/* Decode an image to straight-alpha sRGB RGBA8, box-downsampling so neither side exceeds max_side.
   Caller releases pixels with fl_image_free. NULL on failure. */
unsigned char *fl_image_decode(const char *path, uint32_t max_side, uint32_t *width, uint32_t *height);
/* Release pixels returned by fl_image_decode; NULL is safe. */
void fl_image_free(unsigned char *pixels);
