#pragma once
#include "core/fl_event.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Single-voice audio preview player. The output device opens on first use;
 * decoding and resampling run on the device thread. When a clip ends (or the
 * device fails) FL_EVENT_AUDIO_PLAYBACK is posted and delivered on the bus
 * owner thread, where the player also releases the finished source.
 */
typedef struct Fl_AudioPlayer Fl_AudioPlayer;
#define FL_EVENT_AUDIO_PLAYBACK "audio.playback"
/* Playback state change; token identifies the play request that ended. */
typedef struct Fl_AudioPlayback { Fl_AudioPlayer *player; uint64_t token; bool playing; } Fl_AudioPlayback;
typedef struct Fl_AudioPlayerDesc {
    Fl_EventBus *bus;       /* required; owner thread is the caller */
    bool silent;            /* use miniaudio's null backend (tests, headless) */
} Fl_AudioPlayerDesc;

/* Create a player; no device is opened yet. NULL on failure. */
Fl_AudioPlayer *fl_audio_player_create(const Fl_AudioPlayerDesc *desc);
/* Stop playback, close the device and free. NULL is safe. Owner thread only. */
void fl_audio_player_destroy(Fl_AudioPlayer *player);
/* Replace any current clip with path from the start; returns a non-zero token, or zero on failure. */
uint64_t fl_audio_player_play(Fl_AudioPlayer *player, const char *path);
/* Stop the current clip (no event is posted for an explicit stop). */
void fl_audio_player_stop(Fl_AudioPlayer *player);
/* Token of the clip currently playing, zero when idle. */
uint64_t fl_audio_player_current(const Fl_AudioPlayer *player);
