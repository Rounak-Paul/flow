#pragma once
#include "core/fl_event.h"
#include "core/fl_job.h"
#include "project/fl_project.h"
#include <stdbool.h>
#include <stdint.h>

/* Background Qwen3-TTS for a scene manuscript. The worker runs the bundled llama-tts binary; the owner thread imports the audio. */
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
