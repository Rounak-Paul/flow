#pragma once
#include <stdint.h>
#include <stdatomic.h>
/* Decode one timestamp, scaled to max_side, into malloc-owned RGBA8. Worker-only; bounded and cancellable.
   Returns NULL on cancellation, unsupported input or decoder failure. Never extracts a whole video. */
unsigned char *fl_video_decode(const char *path, double seconds, uint32_t max_side,
                               uint32_t *width, uint32_t *height, const atomic_bool *cancel);
