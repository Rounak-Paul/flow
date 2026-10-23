#pragma once
#include "platform/fl_paths.h"
#include "project/fl_manuscript.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One line for the bundled Qwen3-TTS runner. reference empty uses voices/<speaker>.wav beside Flow when that file exists. */
typedef struct Fl_VoiceItem {
    char text_path[FL_PATH_MAX];
    char out_path[FL_PATH_MAX];
    char speaker[FL_VOICE_NAME_MAX + 1];
    char language[32];
    char reference[FL_PATH_MAX];
    char reference_text[FL_PATH_MAX];
} Fl_VoiceItem;

typedef enum Fl_VoiceStatus {
    FL_VOICE_OK,
    FL_VOICE_FAILED,
    FL_VOICE_CANCELLED
} Fl_VoiceStatus;

/* Speak every item with the llama-tts binary beside Flow, one phase at a time. cancel and done may be NULL; both are atomics.
   error receives a short reason when the status is FL_VOICE_FAILED. Worker-safe: it does not touch a project. */
Fl_VoiceStatus fl_voice_run(const Fl_VoiceItem *items, size_t count, volatile uint32_t *cancel, volatile uint32_t *done,
                            char *error, size_t error_capacity);
