#include "fl_speech.h"
#include "media/fl_voice.h"
#include "project/fl_asset_import.h"
#include "project/fl_manuscript.h"
#include "core/fl_atomic.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_SpeechItem {
    Fl_Id phase, beat, clip, previous_asset;
    char title[FL_PHASE_TITLE_MAX + 1];
    char text_path[FL_PATH_MAX];
    char wav_path[FL_PATH_MAX];
} Fl_SpeechItem;

struct Fl_Speech {
    Fl_Project *project;
    Fl_JobSystem *jobs;
    Fl_EventBus *bus;
    Fl_EventType event;
    Fl_JobHandle job;
    Fl_VoiceItem *voices;
    Fl_SpeechItem *items;
    size_t count;
    Fl_Id scene;
    char work[FL_PATH_MAX];
    volatile uint32_t cancel, done;
    uint32_t total, seen;
    int result;
    bool running;
    char state_name[16];
    char message[512];
    char error[512];
    uint64_t serial;
};

static void publish(Fl_Speech *s)
{
    if (!s->bus || !s->event) return;
    uint32_t done = fl_atomic_load_u32(&s->done);
    fl_event_publish(s->bus, s->event, &done, sizeof(done));
}

static void set_state(Fl_Speech *s, const char *state, const char *message)
{
    snprintf(s->state_name, sizeof(s->state_name), "%s", state);
    if (message) snprintf(s->message, sizeof(s->message), "%s", message);
    publish(s);
}

static void clear_job(Fl_Speech *s)
{
    free(s->voices); free(s->items);
    s->voices = NULL; s->items = NULL; s->count = 0;
    if (s->work[0]) fl_fs_remove_tree(s->work);
    s->work[0] = 0;
}

static void clip_title(const char *title, char *out, size_t cap)
{
    const char *src = title && title[0] ? title : "Narration";
    size_t n = strlen(src);
    if (n > 80) {
        n = 80;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) --n;
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, src, n); out[n] = 0;
    if (!out[0]) snprintf(out, cap, "Narration");
}

static const Fl_AudioClip *find_clip(const Fl_Scene *s, Fl_Id id)
{
    for (size_t i = 0; s && i < s->clip_count; ++i) if (s->clips[i].id == id) return &s->clips[i];
    return NULL;
}

static bool import_audio(Fl_Speech *s)
{
    Fl_Project *p = s->project;
    bool editing = fl_project_edit_begin(p, "Speak manuscript", false);
    if (!editing) {snprintf(s->message, sizeof(s->message), "Could not edit the project."); return false;}
    bool placed = false;
    for (size_t i = 0; i < s->count; ++i) {
        Fl_SpeechItem *item = &s->items[i];
        const Fl_Scene *scene = fl_project_scene(p, s->scene);
        const Fl_Phase *phase = scene ? fl_scene_phase(scene, item->phase) : NULL;
        if (!phase) continue;
        char *title = malloc(strlen(phase->title) + 1), *text = malloc(strlen(phase->text) + 1);
        if (!title || !text) {free(title); free(text); fl_project_edit_cancel(p); snprintf(s->message, sizeof(s->message), "Out of memory."); return false;}
        memcpy(title, phase->title, strlen(phase->title) + 1);
        memcpy(text, phase->text, strlen(phase->text) + 1);
        Fl_StoredAsset stored = {0};
        Fl_ImportStatus status = fl_asset_store_file(fl_project_directory(p), item->wav_path, &stored);
        Fl_Id asset = 0;
        char name[96];
        clip_title(title, name, sizeof(name));
        if (status == FL_IMPORT_ADDED || status == FL_IMPORT_EXISTING) status = fl_asset_record(p, &stored, name, &asset);
        else free(stored.source);
        if ((status != FL_IMPORT_ADDED && status != FL_IMPORT_EXISTING) || !asset) {
            free(title); free(text); fl_project_edit_cancel(p);
            snprintf(s->message, sizeof(s->message), "Could not import narration for %s.", name);
            return false;
        }
        uint64_t duration = stored.duration;
        if (!duration) {
            double seconds = fl_manuscript_seconds(text);
            if (seconds < 1.0) seconds = 5.0;
            duration = (uint64_t)(seconds * (double)FL_TICKS_PER_SECOND + 0.5);
        }
        scene = fl_project_scene(p, s->scene);
        phase = scene ? fl_scene_phase(scene, item->phase) : NULL;
        if (!phase) {
            if (asset && !fl_project_asset_uses(p, asset)) fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = asset}, NULL);
            free(title); free(text); continue;
        }
        Fl_Id beat = phase->beat, clip = phase->clip;
        bool ok = true;
        if (!beat) ok = fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene->id, .name = title,
            .position = scene->beat_count, .beat_duration = duration}, &beat);
        else {
            const Fl_Beat *current = NULL;
            for (size_t b = 0; b < scene->beat_count; ++b) if (scene->beats[b].id == beat) current = &scene->beats[b];
            ok = current && fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene->id, .beat = beat,
                .name = current->name, .beat_duration = duration, .auto_advance = current->auto_advance}, NULL);
        }
        scene = fl_project_scene(p, s->scene);
        Fl_AudioClip record = {.asset = asset, .start = {.kind = FL_ANCHOR_BEAT, .beat = beat}, .gain = 1};
        if (ok && clip) ok = fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_UPDATE, .scene = scene->id, .clip = clip, .clip_record = &record}, NULL);
        else if (ok) ok = fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene->id, .clip_record = &record}, &clip);
        scene = fl_project_scene(p, s->scene);
        phase = scene ? fl_scene_phase(scene, item->phase) : NULL;
        if (ok && phase) ok = fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene->id, .phase = phase->id,
            .name = title, .source = text, .beat = beat, .clip = clip}, NULL);
        Fl_Id previous = item->previous_asset;
        if (ok && previous && previous != asset && !fl_project_asset_uses(p, previous))
            ok = fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = previous}, NULL);
        free(title); free(text);
        if (!ok) {fl_project_edit_cancel(p); snprintf(s->message, sizeof(s->message), "Could not place the narration."); return false;}
        placed = true;
    }
    if (!placed) {fl_project_edit_cancel(p); snprintf(s->message, sizeof(s->message), "The phases were removed before the voice finished."); return false;}
    if (!fl_project_edit_commit(p)) {snprintf(s->message, sizeof(s->message), "Could not save the narration edit."); return false;}
    snprintf(s->message, sizeof(s->message), "Narration is on the timeline.");
    return true;
}

static void finish_job(Fl_Speech *s)
{
    s->running = false;
    bool cancelled = fl_atomic_load_u32(&s->cancel) || s->result == FL_VOICE_CANCELLED;
    if (!cancelled && s->result == FL_VOICE_OK) {
        const Fl_Scene *scene = fl_project_scene(s->project, s->scene);
        if (!scene) snprintf(s->message, sizeof(s->message), "The scene was closed before the voice finished.");
        else if (import_audio(s)) {clear_job(s); set_state(s, "done", NULL); return;}
        clear_job(s); set_state(s, "failed", NULL); return;
    }
    if (!cancelled && s->error[0]) snprintf(s->message, sizeof(s->message), "%s", s->error);
    else if (!cancelled) snprintf(s->message, sizeof(s->message), "Qwen3-TTS failed.");
    else snprintf(s->message, sizeof(s->message), "Speech cancelled.");
    clear_job(s);
    set_state(s, cancelled ? "cancelled" : "failed", NULL);
}

static void speak_job(void *data)
{
    Fl_Speech *s = data;
    s->result = fl_voice_run(s->voices, s->count, &s->cancel, &s->done, s->error, sizeof(s->error));
}

Fl_Speech *fl_speech_create(Fl_Project *project, Fl_JobSystem *jobs, Fl_EventBus *bus)
{
    if (!project || !jobs) return NULL;
    Fl_Speech *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->project = project; s->jobs = jobs; s->bus = bus;
    snprintf(s->state_name, sizeof(s->state_name), "idle");
    if (bus) {
        s->event = fl_event_type_register(bus, FL_EVENT_SPEECH, sizeof(uint32_t));
        if (!s->event) {free(s); return NULL;}
    }
    return s;
}

void fl_speech_destroy(Fl_Speech *s)
{
    if (!s) return;
    if (s->running && !fl_job_is_done(s->jobs, s->job)) {
        fl_atomic_store_u32(&s->cancel, 1);
        fl_job_wait(s->jobs, s->job);
        s->running = false;
        clear_job(s);
    } else if (s->running) finish_job(s);
    free(s);
}

bool fl_speech_start(Fl_Speech *s, Fl_Id scene_id, Fl_Id phase_id)
{
    if (!s) return false;
    fl_speech_pump(s);
    if (s->running) {snprintf(s->message, sizeof(s->message), "Speech is already running."); set_state(s, "running", NULL); return false;}
    const Fl_Scene *scene = fl_project_scene(s->project, scene_id);
    if (!scene) {snprintf(s->message, sizeof(s->message), "That scene is not open."); set_state(s, "failed", NULL); return false;}
    size_t wanted = 0;
    for (size_t i = 0; i < scene->phase_count; ++i) {
        const Fl_Phase *phase = &scene->phases[i];
        if (phase_id && phase->id != phase_id) continue;
        if (fl_manuscript_words(phase->text)) ++wanted;
        else if (phase_id) {snprintf(s->message, sizeof(s->message), "This phase has no words to speak."); set_state(s, "failed", NULL); return false;}
    }
    if (!wanted) {snprintf(s->message, sizeof(s->message), "Write something in a phase first."); set_state(s, "failed", NULL); return false;}
    char cache[FL_PATH_MAX], folder[64];
    snprintf(folder, sizeof(folder), "speech-%llu", (unsigned long long)++s->serial);
    if (!fl_paths_user_dir(FL_PATH_CACHE, cache, sizeof(cache)) || !fl_fs_join(s->work, sizeof(s->work), cache, folder) || !fl_fs_mkdir_p(s->work)) {
        s->work[0] = 0; snprintf(s->message, sizeof(s->message), "Could not create a speech folder."); set_state(s, "failed", NULL); return false;
    }
    Fl_VoiceItem *voices = calloc(wanted, sizeof(*voices));
    Fl_SpeechItem *items = calloc(wanted, sizeof(*items));
    if (!voices || !items) {free(voices); free(items); clear_job(s); snprintf(s->message, sizeof(s->message), "Out of memory."); set_state(s, "failed", NULL); return false;}
    char reference[FL_PATH_MAX] = {0}, transcript[FL_PATH_MAX] = {0};
    if (scene->voice_reference) {
        const Fl_Asset *asset = fl_project_asset(s->project, scene->voice_reference);
        if (!asset || !fl_project_asset_path(s->project, asset, reference, sizeof(reference))) {
            free(voices); free(items); clear_job(s);
            snprintf(s->message, sizeof(s->message), "The voice reference could not be read."); set_state(s, "failed", NULL); return false;
        }
    }
    if (scene->voice_transcript && scene->voice_transcript[0]) {
        if (!fl_fs_join(transcript, sizeof(transcript), s->work, "reference.txt") ||
            !fl_fs_write_atomic(transcript, scene->voice_transcript, strlen(scene->voice_transcript))) {
            free(voices); free(items); clear_job(s);
            snprintf(s->message, sizeof(s->message), "Could not write the reference transcript."); set_state(s, "failed", NULL); return false;
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < scene->phase_count; ++i) {
        const Fl_Phase *phase = &scene->phases[i];
        if ((phase_id && phase->id != phase_id) || !fl_manuscript_words(phase->text)) continue;
        Fl_VoiceItem *voice = &voices[n];
        Fl_SpeechItem *item = &items[n];
        char text_name[32], wav_name[32];
        snprintf(text_name, sizeof(text_name), "text-%zu.txt", n);
        snprintf(wav_name, sizeof(wav_name), "phase-%zu.wav", n);
        if (!fl_fs_join(voice->text_path, sizeof(voice->text_path), s->work, text_name) ||
            !fl_fs_join(voice->out_path, sizeof(voice->out_path), s->work, wav_name) ||
            !fl_fs_write_atomic(voice->text_path, phase->text, strlen(phase->text))) {
            free(voices); free(items); clear_job(s);
            snprintf(s->message, sizeof(s->message), "Could not write a phase."); set_state(s, "failed", NULL); return false;
        }
        snprintf(voice->speaker, sizeof(voice->speaker), "%s", scene->voice ? scene->voice : "Ryan");
        snprintf(voice->language, sizeof(voice->language), "%s", scene->language ? scene->language : "English");
        if (reference[0]) snprintf(voice->reference, sizeof(voice->reference), "%s", reference);
        if (transcript[0]) snprintf(voice->reference_text, sizeof(voice->reference_text), "%s", transcript);
        item->phase = phase->id; item->beat = phase->beat; item->clip = phase->clip;
        snprintf(item->title, sizeof(item->title), "%s", phase->title);
        snprintf(item->text_path, sizeof(item->text_path), "%s", voice->text_path);
        snprintf(item->wav_path, sizeof(item->wav_path), "%s", voice->out_path);
        const Fl_AudioClip *clip = find_clip(scene, phase->clip);
        if (clip) item->previous_asset = clip->asset;
        ++n;
    }
    s->voices = voices; s->items = items; s->count = n; s->scene = scene_id;
    s->error[0] = 0; s->result = FL_VOICE_FAILED;
    fl_atomic_store_u32(&s->cancel, 0); fl_atomic_store_u32(&s->done, 0);
    s->total = (uint32_t)n; s->seen = 0;
    s->job = fl_job_submit(s->jobs, &(Fl_JobDesc){.fn = speak_job, .data = s, .priority = FL_JOB_PRIORITY_LOW});
    if (!s->job.index && !s->job.generation) {
        clear_job(s); snprintf(s->message, sizeof(s->message), "Could not start speech."); set_state(s, "failed", NULL); return false;
    }
    s->running = true;
    snprintf(s->message, sizeof(s->message), "Speaking with Qwen3-TTS · 0/%u", s->total);
    set_state(s, "running", NULL);
    return true;
}

void fl_speech_cancel(Fl_Speech *s)
{
    if (!s || !s->running) return;
    fl_atomic_store_u32(&s->cancel, 1);
    snprintf(s->message, sizeof(s->message), "Cancelling speech…");
    publish(s);
}

void fl_speech_pump(Fl_Speech *s)
{
    if (!s) return;
    if (!s->running) return;
    uint32_t done = fl_atomic_load_u32(&s->done);
    if (done != s->seen) {
        s->seen = done;
        snprintf(s->message, sizeof(s->message), "Speaking with Qwen3-TTS · %u/%u", done, s->total);
        publish(s);
    }
    if (!fl_job_is_done(s->jobs, s->job)) return;
    finish_job(s);
}

bool fl_speech_running(const Fl_Speech *s) {return s && s->running;}
const char *fl_speech_state(const Fl_Speech *s) {return s ? s->state_name : "idle";}
const char *fl_speech_message(const Fl_Speech *s) {return s ? s->message : "";}
uint32_t fl_speech_done(const Fl_Speech *s) {return s ? fl_atomic_load_u32(&s->done) : 0;}
uint32_t fl_speech_total(const Fl_Speech *s) {return s ? s->total : 0;}
