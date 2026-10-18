#pragma once
#include "project/fl_project.h"

/* Frame rate of the decoded frame cache; sources are resampled to it. */
#define FL_VIDEO_CACHE_FPS 60u

/*
 * Random access to video frames. The first request for a video asset extracts every frame to JPEG files under
 * the project's cache (with ffmpeg), after which any time maps to a file; the cache is rebuilt only if deleted.
 * Blocking; owner thread.
 */

/* Extract the asset's frames if needed. False when ffmpeg is missing or fails; the failure is logged. */
bool fl_video_cache_prepare(const Fl_Project *project, const Fl_Asset *asset);
/* Path of the frame shown at source time seconds (clamped to the video), preparing the cache first. */
bool fl_video_cache_frame(const Fl_Project *project, const Fl_Asset *asset, double seconds, char *out, size_t capacity);
