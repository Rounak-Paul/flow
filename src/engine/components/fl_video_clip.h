#pragma once
#include "fl_component.h"
#include "time/fl_time.h"

/*
 * Playback of a video asset inside an entity's box: which asset, when it starts on the scene timeline, where in the
 * source it begins, how fast it runs and whether it loops. Before start it shows the first frame; after the end it
 * holds the last frame unless it loops.
 */
typedef struct Fl_VideoClip {
    Fl_Id asset;
    Fl_Tick start;          /* scene time at which playback begins */
    Fl_Tick offset;         /* source time at the start, in ticks */
    double rate;            /* playback speed multiplier, > 0 */
    bool loop;
} Fl_VideoClip;

/* Encoded size of the version-one payload. */
#define FL_VIDEO_CLIP_BYTES 33u

void fl_video_clip_encode(const Fl_VideoClip *clip, unsigned char out[FL_VIDEO_CLIP_BYTES]);
/* Decode a version-one video component; false for wrong size, a zero asset or out-of-range values. */
bool fl_video_clip_decode(const Fl_Component *component, Fl_VideoClip *out);
/* Source time in seconds shown at scene time tick, for a source lasting duration_ticks (zero when unknown). */
double fl_video_clip_source_seconds(const Fl_VideoClip *clip, Fl_Tick tick, uint64_t duration_ticks);
