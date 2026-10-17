#pragma once
#include "project/fl_project.h"
#include "platform/fl_paths.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Timeline audio mixer: plays any number of clips positioned on a scene
 * timeline, starting from a given scene time. Mixing runs on the device
 * thread; the number of frames the device has rendered is the playback
 * clock (fl_audio_mixer_position), so picture follows sound.
 */
typedef struct Fl_AudioMixer Fl_AudioMixer;
/* One clip resolved to scene time and a file path. */
typedef struct Fl_MixerClip {
    char path[FL_PATH_MAX];
    Fl_Tick start;              /* scene time of the clip's first played sample */
    Fl_Tick source_offset;      /* ticks skipped at the start of the file */
    Fl_Tick duration;           /* ticks played; zero plays to the end of the file */
    double gain;                /* linear */
    Fl_Tick fade_in, fade_out;
} Fl_MixerClip;
typedef struct Fl_AudioMixerDesc { bool silent; } Fl_AudioMixerDesc;

/* Create a mixer; the output device opens on the first start. NULL on failure. */
Fl_AudioMixer *fl_audio_mixer_create(const Fl_AudioMixerDesc *desc);
/* Stop and free. NULL is safe. */
void fl_audio_mixer_destroy(Fl_AudioMixer *mixer);
/* Replace the voices with clips and start playing from scene time from.
   Clips that cannot be opened are skipped. False when no output device is available. */
bool fl_audio_mixer_start(Fl_AudioMixer *mixer, const Fl_MixerClip *clips, size_t count, Fl_Tick from);
/* Stop playback and release the voices. */
void fl_audio_mixer_stop(Fl_AudioMixer *mixer);
/* Scene time reached by the device clock; false when the mixer is not running. */
bool fl_audio_mixer_position(Fl_AudioMixer *mixer, Fl_Tick *tick);
