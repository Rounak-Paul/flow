#pragma once
#include "fl_scene_edit.h"
#include <causality.h>

/*
 * Scene playback: advances the edit controller's current time while playing.
 * When the scene has audio clips, the timeline mixer's device clock drives
 * time; otherwise the monotonic clock does. Publishes FL_EVENT_PLAYBACK_CHANGED
 * when playing starts or stops.
 */
typedef struct Fl_Playback Fl_Playback;
#define FL_EVENT_PLAYBACK_CHANGED "playback.changed"
typedef struct Fl_PlaybackChange { Fl_Playback *playback; bool playing; } Fl_PlaybackChange;
typedef struct Fl_PlaybackDesc {
    Ca_Instance *instance;      /* required; frames are requested while playing */
    Fl_SceneEdit *edit;         /* required; owns the current time */
    Fl_EventBus *bus;           /* required */
    bool silent;                /* null audio backend (tests, headless) */
} Fl_PlaybackDesc;

/* Create a stopped playback controller borrowing the described objects. NULL on failure. */
Fl_Playback *fl_playback_create(const Fl_PlaybackDesc *desc);
/* Stop and free. NULL is safe. */
void fl_playback_destroy(Fl_Playback *playback);
/* Start from the current time (from the beginning when at the end). */
void fl_playback_play(Fl_Playback *playback);
/* Stop advancing, keeping the current time. */
void fl_playback_pause(Fl_Playback *playback);
/* Toggle between playing and paused. */
void fl_playback_toggle(Fl_Playback *playback);
/* Move the current time; continues playing from there when playing. */
void fl_playback_seek(Fl_Playback *playback, Fl_Tick tick);
/* Report whether playback is running. */
bool fl_playback_playing(const Fl_Playback *playback);
