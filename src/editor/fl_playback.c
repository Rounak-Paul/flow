#include "fl_playback.h"
#include "core/fl_clock.h"
#include "media/fl_audio_mixer.h"
#include <stdlib.h>
#include <string.h>

struct Fl_Playback {
    Ca_Instance *instance;
    Fl_SceneEdit *edit;
    Fl_EventBus *bus;
    Fl_EventType changed;
    Fl_AudioMixer *mixer;
    Ca_Effect *ticker;
    bool playing, mixing, advancing;
    Fl_Id scene;
    Fl_Tick from;
    uint64_t started_ns;
};

/* Announce a playing/paused transition. */
static void announce(Fl_Playback *p)
{
    Fl_PlaybackChange change = {p, p->playing};
    fl_event_publish(p->bus, p->changed, &change, sizeof(change));
}

/* (Re)start the clock and the mixer at the current time. */
static void start_clock(Fl_Playback *p)
{
    Fl_Project *project = fl_scene_edit_project(p->edit);
    const Fl_Scene *scene = fl_project_scene(project, fl_scene_edit_scene(p->edit));
    p->scene = fl_scene_edit_scene(p->edit);
    p->from = fl_scene_edit_time(p->edit);
    p->started_ns = fl_clock_monotonic_ns();
    p->mixing = false;
    fl_audio_mixer_stop(p->mixer);
    if (!scene || !scene->clip_count) return;
    Fl_MixerClip *clips = calloc(scene->clip_count, sizeof(*clips));
    size_t count = 0;
    for (size_t i = 0; clips && i < scene->clip_count; ++i) {
        const Fl_AudioClip *c = &scene->clips[i];
        const Fl_Asset *asset = fl_project_asset(project, c->asset);
        Fl_MixerClip *m = &clips[count];
        if (!asset || !fl_project_asset_path(project, asset, m->path, sizeof(m->path))) continue;
        m->start = fl_anchor_resolve(scene, &c->start);
        m->source_offset = (Fl_Tick)c->source_offset; m->duration = (Fl_Tick)c->duration; m->gain = c->gain;
        m->fade_in = (Fl_Tick)c->fade_in; m->fade_out = (Fl_Tick)c->fade_out;
        ++count;
    }
    p->mixing = count && fl_audio_mixer_start(p->mixer, clips, count, p->from);
    free(clips);
}

/* Frame effect: advance time from the audio clock (or wall clock) and stop at the scene end. */
static void tick(void *data)
{
    Fl_Playback *p = data;
    if (!p->playing) return;
    Fl_Tick now;
    if (!p->mixing || !fl_audio_mixer_position(p->mixer, &now))
        now = p->from + (Fl_Tick)((fl_clock_monotonic_ns() - p->started_ns) * (double)FL_TICKS_PER_SECOND / 1e9);
    Fl_Tick length = fl_scene_length(fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit)));
    p->advancing = true;
    fl_scene_edit_set_time(p->edit, now < length ? now : length);
    p->advancing = false;
    if (now >= length) fl_playback_pause(p);
    else ca_instance_request_frame_after(p->instance, 0);
}

/* Follow external time moves (scrubbing) and timeline edits while playing. */
static bool time_changed(const Fl_Event *event, void *data)
{
    (void)event; Fl_Playback *p = data;
    if (p->playing && !p->advancing) start_clock(p);
    return false;
}
static bool project_changed(const Fl_Event *event, void *data)
{
    const Fl_ProjectChange *change = event->payload; Fl_Playback *p = data;
    if (!p->playing || change->project != fl_scene_edit_project(p->edit)) return false;
    switch (change->kind) {
    case FL_OP_BEAT_ADD: case FL_OP_BEAT_REMOVE: case FL_OP_BEAT_UPDATE: case FL_OP_BEAT_MOVE:
    case FL_OP_CLIP_ADD: case FL_OP_CLIP_UPDATE: case FL_OP_CLIP_REMOVE: case FL_OP_SCENE_REMOVE:
        start_clock(p); break;
    default: break;
    }
    return false;
}
static bool selection_changed(const Fl_Event *event, void *data)
{
    (void)event; Fl_Playback *p = data;
    if (p->playing && p->scene != fl_scene_edit_scene(p->edit)) start_clock(p);
    return false;
}

Fl_Playback *fl_playback_create(const Fl_PlaybackDesc *desc)
{
    if (!desc || !desc->instance || !desc->edit || !desc->bus) return NULL;
    Fl_Playback *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->instance = desc->instance; p->edit = desc->edit; p->bus = desc->bus;
    p->changed = fl_event_type_register(p->bus, FL_EVENT_PLAYBACK_CHANGED, sizeof(Fl_PlaybackChange));
    p->mixer = fl_audio_mixer_create(&(Fl_AudioMixerDesc){.silent = desc->silent});
    p->ticker = p->mixer ? ca_frame_effect(p->instance, tick, p) : NULL;
    bool ok = p->changed != FL_EVENT_TYPE_INVALID && p->ticker &&
        fl_event_subscribe(p->bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(p->bus, FL_EVENT_TIME_CHANGED), .fn = time_changed, .user_data = p, .owner = p}).generation &&
        fl_event_subscribe(p->bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(p->bus, FL_EVENT_PROJECT_CHANGED), .fn = project_changed, .user_data = p, .owner = p}).generation &&
        fl_event_subscribe(p->bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(p->bus, FL_EVENT_SELECTION_CHANGED), .fn = selection_changed, .user_data = p, .owner = p}).generation;
    if (!ok) { fl_playback_destroy(p); return NULL; }
    return p;
}

void fl_playback_destroy(Fl_Playback *p)
{
    if (!p) return;
    fl_event_unsubscribe_owner(p->bus, p);
    ca_effect_destroy(p->ticker);
    fl_audio_mixer_destroy(p->mixer);
    free(p);
}

void fl_playback_play(Fl_Playback *p)
{
    if (!p || p->playing) return;
    const Fl_Scene *scene = fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
    if (!scene) return;
    if (fl_scene_edit_time(p->edit) >= fl_scene_length(scene)) fl_scene_edit_set_time(p->edit, 0);
    p->playing = true;
    start_clock(p);
    ca_instance_request_frame_after(p->instance, 0);
    announce(p);
}

void fl_playback_pause(Fl_Playback *p)
{
    if (!p || !p->playing) return;
    p->playing = false; p->mixing = false;
    fl_audio_mixer_stop(p->mixer);
    announce(p);
}

void fl_playback_toggle(Fl_Playback *p)
{
    if (p && p->playing) fl_playback_pause(p); else fl_playback_play(p);
}

void fl_playback_seek(Fl_Playback *p, Fl_Tick tick)
{
    if (p) fl_scene_edit_set_time(p->edit, tick);
}

bool fl_playback_playing(const Fl_Playback *p) { return p && p->playing; }
