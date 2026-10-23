#pragma once
#include "project/fl_speech_cues.h"
#include <stdatomic.h>

/* Worker-owned measured cues. Unmatched script words are explicitly unresolved. */
typedef struct Fl_SpeechAlignment {
    Fl_SpeechCue *cues;
    size_t count;
} Fl_SpeechAlignment;

/* Analyze audio locally with the bundled recognizer and match word timings to script.
   work is an exclusively owned directory; cancel interrupts analysis. error receives failures. */
bool fl_speech_align(const char *audio, const char *script, const char *language, const char *work,
                     const atomic_bool *cancel, Fl_SpeechAlignment *out, char *error, size_t capacity);
/* Match recognizer JSON against script; audio_duration bounds all accepted timestamp intervals. */
bool fl_speech_alignment_parse(const char *json, const char *script, Fl_Tick audio_duration,
                               Fl_SpeechAlignment *out, char *error, size_t capacity);
/* Release all measured cue text and reset alignment. */
void fl_speech_alignment_free(Fl_SpeechAlignment *alignment);
