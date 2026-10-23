#pragma once
#include "fl_project.h"

/* Free cue strings and their array, including partial allocations. */
void fl_speech_cues_free(Fl_SpeechCue *cues, size_t count);
/* Compare cue text independently of ASCII case and punctuation; Unicode letters remain exact. */
bool fl_speech_cue_equal(const Fl_SpeechCue *a, const Fl_SpeechCue *b);
/* Apply measured cues to a phase, preserving stable IDs through unambiguous sequence matches. */
bool fl_speech_cues_apply(const Fl_Project *project, Fl_Scene *scene, const Fl_Op *op, uint64_t *next_id);
/* Resolve a live cue through its phase's aligned audio clip, including trim and placement. */
bool fl_speech_cue_sample(const Fl_Scene *scene, Fl_Id cue, bool end, Fl_Tick *out);
/* Resolve a known phase/cue pair without repeating the stable-ID lookup. */
bool fl_phase_speech_cue_sample(const Fl_Scene *scene, const Fl_Phase *phase, const Fl_SpeechCue *cue, bool end, Fl_Tick *out);
/* Borrow a cue and optionally its owning phase, including unresolved/retired records. */
const Fl_SpeechCue *fl_scene_speech_cue(const Fl_Scene *scene, Fl_Id id, const Fl_Phase **phase);
/* Validate one phase's persisted alignment and audio ownership. */
bool fl_speech_cues_valid(const Fl_Project *project, const Fl_Scene *scene, const Fl_Phase *phase, uint64_t next_id);
