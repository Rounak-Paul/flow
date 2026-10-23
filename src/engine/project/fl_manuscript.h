#pragma once
#include "doc/fl_binary.h"
#include "fl_project.h"

/* Spoken script for one scene. A phase is one stretch of narration, usually tied to a beat.
   150 words is about one minute of voiceover. */

#define FL_PHASE_MAX 256u
#define FL_PHASE_TITLE_MAX 128u
#define FL_PHASE_TEXT_MAX (16u * 1024u)
#define FL_VOICE_NAME_MAX 64u
#define FL_VOICE_TRANSCRIPT_MAX 4096u
#define FL_NARRATION_WPM 150u

/* Words separated by ASCII whitespace. */
size_t fl_manuscript_words(const char *text);
/* Reading time at FL_NARRATION_WPM. Empty text is zero. */
double fl_manuscript_seconds(const char *text);
/* "0:42" under a minute, otherwise "1.5 min". */
void fl_manuscript_format(double seconds, char *out, size_t capacity);

/* Qwen3-TTS CustomVoice speakers, and languages accepted by voice.set. */
const char *const *fl_manuscript_speakers(size_t *count);
const char *const *fl_manuscript_languages(size_t *count);
bool fl_manuscript_speaker_ok(const char *name);
bool fl_manuscript_language_ok(const char *name);

/* Borrow a phase by id, or NULL. */
const Fl_Phase *fl_scene_phase(const Fl_Scene *scene, Fl_Id phase);

/* Release phase text and the voice strings. */
void fl_manuscript_free(Fl_Scene *scene);
/* Apply FL_OP_PHASE_* or FL_OP_VOICE_SET. False leaves the scene unchanged. */
bool fl_manuscript_apply(const Fl_Project *project, Fl_Scene *scene, const Fl_Op *op, uint64_t *next_id, Fl_Id *created);
/* Clear links when a beat or clip goes away. The phase text stays. */
void fl_manuscript_detach_beat(Fl_Scene *scene, Fl_Id beat);
void fl_manuscript_detach_clip(Fl_Scene *scene, Fl_Id clip);
bool fl_manuscript_valid(const Fl_Project *project, const Fl_Scene *scene, uint64_t next_id);
size_t fl_manuscript_id_count(const Fl_Scene *scene);
void fl_manuscript_ids(const Fl_Scene *scene, Fl_Id *ids);
/* Voice-clone reference uses. Generated clips are counted with the timeline. */
size_t fl_manuscript_asset_uses(const Fl_Scene *scene, Fl_Id asset);
void fl_manuscript_save(const Fl_Scene *scene, Fl_Binary *stream);
bool fl_manuscript_load(Fl_Scene *scene, Fl_Binary *stream);
