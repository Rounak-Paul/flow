#pragma once
#include "project/fl_project.h"

/* One audible interval in composition time. Rate maps composition seconds to source seconds; holds are silent. */
typedef struct Fl_AudioSegment {
    Fl_Id asset;
    double start,duration,source_offset,rate,gain;
    double envelope_offset,envelope_duration,fade_in,fade_out;
} Fl_AudioSegment;
typedef struct Fl_AudioPlan {Fl_AudioSegment *segments; size_t count,capacity; char error[256];} Fl_AudioPlan;
/* Compile native/prototype clips through nested stretch/fixed/loop clocks. False leaves an empty diagnosed plan.
   The result owns metadata only, borrowing neither the scene nor its project. Output must start empty. */
bool fl_audio_plan_compile(const Fl_Scene *scene, Fl_AudioPlan *out);
/* Release segments and reset the plan. NULL is safe. */
void fl_audio_plan_clear(Fl_AudioPlan *plan);

#include "media/fl_audio_mixer.h"
/* Resolve one compiled segment to the shared mixer contract; origin shifts scenes/crossfades on an export timeline. */
bool fl_audio_segment_clip(const Fl_Project *project, const Fl_AudioSegment *segment, double origin, Fl_MixerClip *out);
