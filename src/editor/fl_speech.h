#pragma once
#include "core/fl_event.h"
#include "core/fl_job.h"
#include "project/fl_project.h"
#include <stdbool.h>
#include <stdint.h>

/* Background Qwen3-TTS. Previews remain temporary until accepted; batch manuscript jobs import on completion. */
#define FL_EVENT_SPEECH "speech.changed"

typedef struct Fl_Speech Fl_Speech;

Fl_Speech *fl_speech_create(Fl_Project *project, Fl_JobSystem *jobs, Fl_EventBus *bus);
/* Wait for an in-flight job. A finished job is imported; a still-running one is cancelled and not imported. */
void fl_speech_destroy(Fl_Speech *speech);

/* phase 0 speaks every non-empty phase. False leaves a reason in fl_speech_message. */
bool fl_speech_start(Fl_Speech *speech, Fl_Id scene, Fl_Id phase);
void fl_speech_cancel(Fl_Speech *speech);
/* Owner thread. Imports finished audio and publishes speech.changed when progress moves. */
void fl_speech_pump(Fl_Speech *speech);

bool fl_speech_running(const Fl_Speech *speech);
const char *fl_speech_state(const Fl_Speech *speech);
const char *fl_speech_message(const Fl_Speech *speech);
uint32_t fl_speech_done(const Fl_Speech *speech);
uint32_t fl_speech_total(const Fl_Speech *speech);

/* Generate a temporary preview using text and explicit voice settings; no phase or asset is added. */
bool fl_speech_preview(Fl_Speech *speech, Fl_Id scene, const char *text, const char *speaker,
                       const char *language, Fl_Id reference, const char *transcript);
/* Borrow the completed WAV path until discard, acceptance or the next generation. NULL when unavailable. */
const char *fl_speech_preview_path(const Fl_Speech *speech);
/* Import the preview and atomically create/update phase, beat and clip. phase zero creates a phase. */
bool fl_speech_accept(Fl_Speech *speech, Fl_Id scene, Fl_Id phase, const char *title);
/* Discard temporary preview audio, or request cancellation if its worker is running. */
void fl_speech_discard(Fl_Speech *speech);
