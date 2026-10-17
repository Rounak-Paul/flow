#pragma once
#include "project/fl_project.h"
#include "platform/fl_paths.h"
#include <stdint.h>

/* Thread-safe copy of what thumbnailing needs from an asset record. */
typedef struct Fl_ThumbnailSource {
    char directory[FL_PATH_MAX];    /* project directory */
    unsigned char hash[32];
    char extension[FL_ASSET_EXTENSION_MAX + 1];
    Fl_AssetKind kind;
} Fl_ThumbnailSource;

/* Capture asset's thumbnail source within project; false when it does not fit. */
bool fl_thumbnail_source(const Fl_Project *project, const Fl_Asset *asset, Fl_ThumbnailSource *out);
/*
 * Return a straight-alpha sRGB RGBA8 preview fitting max_side: a downscaled
 * picture, a video frame (where the platform can decode it), an "Aa"
 * specimen for fonts or a peak waveform for audio. Results are cached under
 * .flow/cache/thumbnails. Blocking; call from a worker.
 *
 * Returns  malloc'd pixels (caller frees), or NULL when generation fails.
 */
unsigned char *fl_thumbnail_get(const Fl_ThumbnailSource *source, uint32_t max_side, uint32_t *width, uint32_t *height);
