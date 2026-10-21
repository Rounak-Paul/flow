#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Decode a representative frame of a video file (10% in, at most one second)
 * as straight-alpha sRGB RGBA8 scaled to fit max_side, using the platform's
 * media framework (AVFoundation on macOS). Blocking; call from a worker.
 *
 * video_width/height  Optional; receive the displayed (orientation-applied) video size.
 * seconds             Optional; receives the duration in seconds (0 when unknown).
 * Returns             malloc'd pixels (caller frees), or NULL where unsupported or on failure.
 */
unsigned char *fl_video_frame(const char *path, uint32_t max_side, uint32_t *width, uint32_t *height,
                              uint32_t *video_width, uint32_t *video_height, double *seconds);

#include <stdatomic.h>
/* Decode the frame at a source timestamp on a worker, with bounded waiting and cancellation. */
unsigned char *fl_video_frame_at(const char *path, double seconds, uint32_t max_side,
                                 uint32_t *width, uint32_t *height, const atomic_bool *cancel);
