#pragma once
#include "platform/fl_paths.h"
#include "project/fl_manuscript.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* One line for the bundled Qwen3-TTS runner. reference empty uses voices/<speaker>.wav beside Flow when that file exists. */
typedef struct Fl_VoiceItem {
    char text_path[FL_PATH_MAX];
    char out_path[FL_PATH_MAX];
    char speaker[FL_VOICE_NAME_MAX + 1];
    char language[32];
    char reference[FL_PATH_MAX];
} Fl_VoiceItem;

/* Worker-written part counters; read with fl_atomic_load_u32 on the owner thread. */
typedef struct Fl_VoiceProgress {
    volatile uint32_t done, total;
} Fl_VoiceProgress;

typedef enum Fl_VoiceStatus {
    FL_VOICE_OK,
    FL_VOICE_FAILED,
    FL_VOICE_CANCELLED
} Fl_VoiceStatus;

/* Speak bounded UTF-8 parts and stitch each item into one complete WAV. Capped parts split and retry.
   cancel, completed-item done and part progress may be NULL; counters are atomic. error receives failures.
   Worker-safe: no project mutation; failed/cancelled items never publish partial output. */
Fl_VoiceStatus fl_voice_run(const Fl_VoiceItem *items, size_t count, const atomic_bool *cancel, volatile uint32_t *done, Fl_VoiceProgress *progress,
                            char *error, size_t error_capacity);

/* Resolve the bundled preview/reference for speaker into out, bounded by capacity. */
bool fl_voice_preset_path(const char *speaker, char *out, size_t capacity);
