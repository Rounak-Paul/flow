#include "fl_manuscript_panel.h"
#include "fl_file_browser.h"
#include "project/fl_asset_import.h"
#include "project/fl_manuscript.h"
#include "project/fl_speech_cues.h"
#include "media/fl_audio_player.h"
#include "media/fl_voice.h"
#include "core/fl_clock.h"
#include "ui/fl_button.h"
#include "style.h"
#include <ca_icons.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unaccepted writing belongs to its scene and survives panel rebuilds and scene switches. */
typedef struct Fl_ManuscriptDraft {
    struct Fl_ManuscriptDraft *next;
    Fl_Id scene, phase, reference;
    char text[FL_PHASE_TEXT_MAX + 1], title[FL_PHASE_TITLE_MAX + 1];
    char transcript[FL_VOICE_TRANSCRIPT_MAX + 1];
    char speaker[FL_VOICE_NAME_MAX + 1], language[32];
    bool reference_mode;
    uint64_t revision;
} Fl_ManuscriptDraft;

typedef struct Fl_ManuscriptRow {
    struct Fl_ManuscriptPanel *panel;
    Fl_Id phase;
} Fl_ManuscriptRow;

/* Stable cue callback context for one visible page. */
typedef struct Fl_ManuscriptCueRow {struct Fl_ManuscriptPanel *panel; Fl_Id cue;} Fl_ManuscriptCueRow;

struct Fl_ManuscriptPanel {
    Fl_SceneEdit *edit;
    Fl_Speech *speech;
    Fl_AssetImporter *importer;
    uint64_t import_request;
    Fl_Id import_scene;
    Fl_AudioPlayer *player;
    Fl_EventBus *bus;
    Fl_FileBrowser *browser;
    Ca_Dock *dock;
    Ca_Window *window;
    Ca_Effect *effect;
    Ca_Div *well;
    Ca_Label *progress_label, *estimate;
    Ca_Button *accept_button, *generate_button, *play_button;
    Fl_ManuscriptDraft *drafts, *draft;
    Fl_ManuscriptRow *rows;
    size_t row_count;
    const char **asset_names;
    Fl_Id *asset_ids;
    size_t asset_count;
    Fl_Id generated_scene, generated_phase, browser_scene;
    Fl_Id expanded_phase, selected_cue;
    size_t cue_page;
    int cue_kind;
    Fl_ManuscriptCueRow cue_rows[40];
    char cue_start[48], cue_end[48];
    uint64_t generated_revision, started, playing;
    bool was_running, applying, building;
    char message[256];
    char playing_path[FL_PATH_MAX];
};

/* Invalidate the manuscript panel after controller state changes. */
static void invalidate(Fl_ManuscriptPanel *p)
{
    if (p->dock && !p->building && !p->applying) ca_dock_invalidate_panel(p->dock, FL_MANUSCRIPT_PANEL_ID);
}

/* Set the panel's user-visible status to message. */
static void status(Fl_ManuscriptPanel *p, const char *message)
{
    snprintf(p->message, sizeof(p->message), "%s", message ? message : "");
    invalidate(p);
}

/* Borrow the scene currently selected by the editor. */
static const Fl_Scene *active_scene(const Fl_ManuscriptPanel *p)
{
    return fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
}

/* Return generated narration only when it matches the current draft and voice settings. */
static const char *ready_audio(const Fl_ManuscriptPanel *p)
{
    const Fl_ManuscriptDraft *d = p->draft;
    return d && d->scene == p->generated_scene && d->phase == p->generated_phase &&
        d->revision == p->generated_revision ? fl_speech_preview_path(p->speech) : NULL;
}

/* Refresh controls and reading estimate without rebuilding a focused text area. */
static void refresh(Fl_ManuscriptPanel *p)
{
    Fl_ManuscriptDraft *d = p->draft;
    bool running = fl_speech_running(p->speech);
    if (p->accept_button) ca_set_disabled(p->accept_button, running || !ready_audio(p) || !d || !d->title[0]);
    if (p->play_button) {ca_set_disabled(p->play_button, running || !ready_audio(p)); ca_set_hidden(p->play_button, !ready_audio(p));}
    if (p->generate_button) ca_set_disabled(p->generate_button, running || !d || !fl_manuscript_words(d->text) || (d->reference_mode && !d->reference));
    if (p->estimate && d) {
        char time[32], label[96]; fl_manuscript_format(fl_manuscript_seconds(d->text), time, sizeof(time));
        snprintf(label, sizeof(label), "%zu words · %s estimated", fl_manuscript_words(d->text), time);
        ca_set_text(p->estimate, label);
    }
}

/* Copy narration edits into the active scene draft and invalidate any older generated take. */
static void text_changed(Ca_TextInput *input, void *data)
{
    Fl_ManuscriptPanel *p = data;
    if (!p->draft) return;
    snprintf(p->draft->text, sizeof(p->draft->text), "%s", ca_input_text(input));
    ++p->draft->revision; refresh(p);
}

/* Update the phase title; title changes do not require regenerating speech. */
static void title_changed(Ca_TextInput *input, void *data)
{
    Fl_ManuscriptPanel *p = data;
    if (p->draft) snprintf(p->draft->title, sizeof(p->draft->title), "%s", ca_input_text(input));
    refresh(p);
}

/* Stop only playback started by this panel, preserving previews started elsewhere. */
static void stop_audio(Fl_ManuscriptPanel *p)
{
    if (p->playing && fl_audio_player_current(p->player) == p->playing) fl_audio_player_stop(p->player);
    p->playing = 0; p->playing_path[0] = 0;
}

/* Toggle playback of path through the workspace's shared preview player. */
static void play_path(Fl_ManuscriptPanel *p, const char *path)
{
    if (p->playing && fl_audio_player_current(p->player) == p->playing && path && !strcmp(path, p->playing_path)) {stop_audio(p); invalidate(p); return;}
    stop_audio(p);
    p->playing = path ? fl_audio_player_play(p->player, path) : 0;
    if (!p->playing) status(p, "Could not play this audio.");
    else {snprintf(p->playing_path, sizeof(p->playing_path), "%s", path); invalidate(p);}
}

/* Preview the generated narration for the current draft. */
static void play_generated(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data; play_path(p, ready_audio(p));
}

/* Start temporary narration generation using the draft's selected voice source. */
static void start_generation(Fl_ManuscriptPanel *p)
{
    Fl_ManuscriptDraft *d = p->draft;
    if (!d || fl_speech_running(p->speech) || (d->reference_mode && !d->reference)) return;
    stop_audio(p);
    p->generated_scene = d->scene; p->generated_phase = d->phase; p->generated_revision = d->revision;
    p->started = fl_clock_monotonic_ns(); p->message[0] = 0;
    if (!fl_speech_preview(p->speech, d->scene, d->text,
        d->speaker, d->language, d->reference_mode ? d->reference : 0, d->transcript)) status(p, fl_speech_message(p->speech));
    p->was_running = fl_speech_running(p->speech);
    invalidate(p);
}

/* Generate the draft without importing audio or changing a phase. */
static void generate(Ca_Button *button, void *data)
{
    (void)button; start_generation(data);
}

/* Play the bundled sample of the chosen built-in voice immediately. */
static void sample_voice(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data; char path[FL_PATH_MAX];
    if (p->draft && fl_voice_preset_path(p->draft->speaker, path, sizeof(path))) play_path(p, path);
    else status(p, "The built-in voice preview is missing.");
}

/* Request cancellation; the worker retains its files until it has stopped. */
static void cancel_generation(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data; fl_speech_cancel(p->speech); invalidate(p);
}

/* Save accepted narration and its voice settings in one undoable project transaction. */
static void accept_phase(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data; Fl_ManuscriptDraft *d = p->draft;
    if (!d || !ready_audio(p)) return;
    p->applying = true; stop_audio(p);
    bool ok = fl_speech_accept(p->speech, d->scene, d->phase, d->title);
    if (ok) {
        d->phase = 0; d->text[0] = 0; ++d->revision;
        const Fl_Scene *scene = active_scene(p);
        snprintf(d->title, sizeof(d->title), "Phase %zu", scene ? scene->phase_count + 1 : 1);
    }
    p->applying = false; status(p, fl_speech_message(p->speech));
}

/* Switch between built-in and recording sources without discarding either source's settings. */
static void source_changed(Ca_TabBar *tabs, void *data)
{
    Fl_ManuscriptPanel *p = data;
    if (!p->draft || fl_speech_running(p->speech)) return;
    p->draft->reference_mode = ca_tabs_active(tabs) == 1; ++p->draft->revision; invalidate(p);
}

/* Change the built-in voice and mark the existing take stale. */
static void speaker_changed(Ca_Select *select, void *data)
{
    Fl_ManuscriptPanel *p = data; size_t count; const char *const *voices = fl_manuscript_speakers(&count);
    int index = ca_select_get(select);
    if (!p->draft || index < 0 || (size_t)index >= count) return;
    snprintf(p->draft->speaker, sizeof(p->draft->speaker), "%s", voices[index]); ++p->draft->revision; invalidate(p);
}

/* Change narration language and mark the existing take stale. */
static void language_changed(Ca_Select *select, void *data)
{
    Fl_ManuscriptPanel *p = data; size_t count; const char *const *languages = fl_manuscript_languages(&count);
    int index = ca_select_get(select);
    if (!p->draft || index < 0 || (size_t)index >= count) return;
    snprintf(p->draft->language, sizeof(p->draft->language), "%s", languages[index]); ++p->draft->revision; invalidate(p);
}

/* Select an existing audio asset as the recording source. */
static void asset_changed(Ca_Select *select, void *data)
{
    Fl_ManuscriptPanel *p = data; int index = ca_select_get(select);
    if (!p->draft || index < 0 || (size_t)index >= p->asset_count) return;
    p->draft->reference = p->asset_ids[index]; ++p->draft->revision; invalidate(p);
}

/* Store a dropped or chosen recording as a project asset and select it for cloning. */
static void use_recording(Fl_ManuscriptPanel *p, const char *path)
{
    if (!p->draft || fl_speech_running(p->speech)) return;
    p->import_scene = p->draft->scene;
    p->import_request = fl_asset_import(p->importer, path);
    status(p, p->import_request ? "Importing voice reference…" : "Could not import that voice reference.");
}

/* Observe this panel's asynchronous recording import and select its audio asset in the originating draft. */
static bool recording_imported(const Fl_Event *event, void *data)
{
    Fl_ManuscriptPanel *p = data; const Fl_AssetImported *result = event->payload;
    if (result->importer != p->importer || result->request != p->import_request) return false;
    p->import_request = 0;
    const Fl_Asset *asset = fl_project_asset(fl_scene_edit_project(p->edit), result->asset);
    if (!asset || asset->kind != FL_ASSET_AUDIO) {status(p, "Choose a supported audio recording."); return false;}
    for (Fl_ManuscriptDraft *d = p->drafts; d; d = d->next) if (d->scene == p->import_scene) {
        d->reference = asset->id; d->reference_mode = true; ++d->revision; break;
    }
    status(p, "Voice reference selected."); return false;
}

/* Handle a confirmed file-browser selection for the current scene draft. */
static void recording_chosen(const char *const *paths, size_t count, void *data)
{
    Fl_ManuscriptPanel *p = data;
    if (count && p->draft && p->draft->scene == p->browser_scene) use_recording(p, paths[0]);
}

/* Open an audio-only file chooser for a voice reference. */
static void browse_recording(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data;
    if (!p->window) return;
    if (!p->browser) p->browser = fl_file_browser_create(ca_window_instance(p->window));
    p->browser_scene = p->draft ? p->draft->scene : 0;
    static const char *const extensions[] = {"wav", "mp3", "flac", "ogg", "m4a", "aac", "aiff"};
    const Fl_FileFilter filter = {"Audio", extensions, sizeof(extensions) / sizeof(*extensions)};
    if (!p->browser || !fl_file_browser_show(p->browser, &(Fl_FileBrowserDesc){.mode = FL_FILE_BROWSER_OPEN_FILES,
        .title = "Choose voice reference", .confirm_label = "Use voice reference", .filters = &filter, .filter_count = 1,
        .on_confirm = recording_chosen, .user_data = p})) status(p, "Could not open the file browser.");
}

/* Find the audio asset currently attached to phase in scene. */
static Fl_Id phase_asset(const Fl_Scene *scene, const Fl_Phase *phase)
{
    for (size_t i = 0; scene && phase && i < scene->clip_count; ++i)
        if (scene->clips[i].id == phase->clip) return scene->clips[i].asset;
    return 0;
}

/* Play the accepted audio for a saved phase. */
static void play_phase(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptRow row = *(Fl_ManuscriptRow *)data;
    const Fl_Scene *scene = active_scene(row.panel); const Fl_Phase *phase = scene ? fl_scene_phase(scene, row.phase) : NULL;
    Fl_Project *project = fl_scene_edit_project(row.panel->edit);
    const Fl_Asset *asset = fl_project_asset(project, phase_asset(scene, phase)); char path[FL_PATH_MAX];
    if (asset && fl_project_asset_path(project, asset, path, sizeof(path))) play_path(row.panel, path);
}

/* Load a saved phase into the editor; regeneration remains unaccepted until Save phase. */
static void edit_phase(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptRow row = *(Fl_ManuscriptRow *)data; Fl_ManuscriptPanel *p = row.panel;
    const Fl_Scene *scene = active_scene(p); const Fl_Phase *phase = scene ? fl_scene_phase(scene, row.phase) : NULL;
    if (!phase || !p->draft || fl_speech_running(p->speech)) return;
    stop_audio(p); p->draft->phase = phase->id;
    snprintf(p->draft->title, sizeof(p->draft->title), "%s", phase->title);
    snprintf(p->draft->text, sizeof(p->draft->text), "%s", phase->text);
    ++p->draft->revision; p->message[0] = 0; invalidate(p);
}

/* Start a fresh phase draft, leaving all accepted phases untouched. */
static void new_phase(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data;
    if (!p->draft || fl_speech_running(p->speech)) return;
    stop_audio(p); p->draft->phase = 0; p->draft->text[0] = 0; ++p->draft->revision;
    const Fl_Scene *scene = active_scene(p);
    snprintf(p->draft->title, sizeof(p->draft->title), "Phase %zu", scene ? scene->phase_count + 1 : 1);
    invalidate(p);
}

/* Show a saved phase's measured word/sentence cues. */
static void toggle_cues(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptRow row = *(Fl_ManuscriptRow *)data; Fl_ManuscriptPanel *p = row.panel;
    p->expanded_phase = p->expanded_phase == row.phase ? 0 : row.phase;
    p->cue_page = 0; p->selected_cue = 0; invalidate(p);
}

/* Switch the cue browser between words and sentences. */
static void cue_kind_changed(Ca_TabBar *tabs, void *data)
{
    Fl_ManuscriptPanel *p = data; p->cue_kind = ca_tabs_active(tabs); p->cue_page = 0; p->selected_cue = 0; invalidate(p);
}

/* Browse the preceding fixed-size cue page. */
static void previous_cues(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data; if (p->cue_page) --p->cue_page; invalidate(p);
}

/* Browse the following fixed-size cue page. */
static void next_cues(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data; ++p->cue_page; invalidate(p);
}

/* Select a cue for timing inspection/repair and seek to its available scene time. */
static void select_cue(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptCueRow row = *(Fl_ManuscriptCueRow *)data; Fl_ManuscriptPanel *p = row.panel;
    const Fl_Scene *scene = active_scene(p); const Fl_SpeechCue *cue = fl_scene_speech_cue(scene,row.cue,NULL);
    if (!cue) return;
    p->selected_cue = cue->id;
    snprintf(p->cue_start,sizeof(p->cue_start),"%.3f",(double)cue->start / FL_TICKS_PER_SECOND);
    snprintf(p->cue_end,sizeof(p->cue_end),"%.3f",(double)cue->end / FL_TICKS_PER_SECOND);
    Fl_Tick tick; if (fl_speech_cue_sample(scene,cue->id,false,&tick)) fl_scene_edit_set_time(p->edit,tick);
    invalidate(p);
}

/* Retain a manual cue start draft without publishing incomplete numeric input. */
static void cue_start_changed(Ca_TextInput *input, void *data)
{
    Fl_ManuscriptPanel *p = data; snprintf(p->cue_start,sizeof(p->cue_start),"%s",ca_input_text(input));
}

/* Retain a manual cue end draft without publishing incomplete numeric input. */
static void cue_end_changed(Ca_TextInput *input, void *data)
{
    Fl_ManuscriptPanel *p = data; snprintf(p->cue_end,sizeof(p->cue_end),"%s",ca_input_text(input));
}

/* Explicitly correct one cue's offsets within the current aligned audio asset. */
static void repair_cue(Ca_Button *button, void *data)
{
    (void)button; Fl_ManuscriptPanel *p = data; const Fl_Scene *scene = active_scene(p);
    char *end_a, *end_b; double a = strtod(p->cue_start,&end_a), b = strtod(p->cue_end,&end_b); Fl_Tick start, end;
    if (!scene || !p->selected_cue || end_a == p->cue_start || *end_a || end_b == p->cue_end || *end_b ||
        !fl_time_from_seconds(a,&start) || !fl_time_from_seconds(b,&end)) {status(p,"Enter valid start and end times in seconds."); return;}
    if (!fl_project_apply(fl_scene_edit_project(p->edit),&(Fl_Op){.kind = FL_OP_CUE_UPDATE, .scene = scene->id,
        .cue = p->selected_cue, .cue_start = start, .cue_end = end},NULL)) status(p,"Timing must be inside the current audio. Removed cues must be rebound.");
    else status(p,"Speech cue timing updated.");
}

/* Render a bounded, scrollable cue page with explicit unresolved diagnostics. */
static void build_cues(Fl_ManuscriptPanel *p, const Fl_Scene *scene, const Fl_Phase *phase, bool running)
{
    const char *tabs[] = {"Words","Sentences"};
    ca_tabs(&(Ca_TabBarDesc){.labels = tabs, .count = 2, .active = p->cue_kind, .on_change = cue_kind_changed, .change_data = p});
    size_t count = 0;
    for (size_t i = 0; i < phase->cue_count; ++i) if ((int)phase->cues[i].kind == p->cue_kind) ++count;
    if (p->cue_page * 40 >= count) p->cue_page = count ? (count - 1) / 40 : 0;
    ca_text(&(Ca_TextDesc){.text = "Select a cue to seek or correct its timing. Bind animation keys in the Timeline's Speech cue controls.", .wrap = true, .style = FL_CLASS_MUTED});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .inline_style = "width: 100%; max-height: 220px; min-height: 40px; flex-shrink: 0; gap: 4px; padding: 8px; overflow-y: auto;"});
    size_t index = 0, visible = 0;
    for (size_t i = 0; i < phase->cue_count && visible < 40; ++i) {
        const Fl_SpeechCue *cue = &phase->cues[i]; if ((int)cue->kind != p->cue_kind) continue;
        if (index++ < p->cue_page * 40) continue;
        Fl_ManuscriptCueRow *row = &p->cue_rows[visible++]; *row = (Fl_ManuscriptCueRow){p,cue->id};
        Fl_Tick tick; bool resolved = fl_phase_speech_cue_sample(scene,phase,cue,false,&tick);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .inline_style = "width: 100%; flex-shrink: 0; gap: 8px; align-items: center;"});
        char time[96];
        if (resolved) snprintf(time,sizeof(time),"%.2f–%.2fs",(double)cue->start / FL_TICKS_PER_SECOND,(double)cue->end / FL_TICKS_PER_SECOND);
        else snprintf(time,sizeof(time),"%s",cue->retired ? "Removed / ambiguous" : "Needs timing review");
        fl_button(&(Fl_ButtonDesc){.text = "Select", .on_click = select_cue, .click_data = row, .active = p->selected_cue == cue->id});
        ca_text(&(Ca_TextDesc){.text = time, .style = resolved ? FL_CLASS_MUTED : FL_CLASS_WARNING});
        ca_text(&(Ca_TextDesc){.text = cue->text, .wrap = true});
        ca_div_end();
    }
    if (!count) ca_text(&(Ca_TextDesc){.text = "Generate narration to measure speech cues.", .style = FL_CLASS_MUTED});
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .inline_style = "width: 100%; flex-shrink: 0; gap: 8px; flex-wrap: wrap; align-items: center;"});
    fl_button(&(Fl_ButtonDesc){.text = "Previous", .on_click = previous_cues, .click_data = p, .disabled = !p->cue_page});
    char page[64]; snprintf(page,sizeof(page),"%zu–%zu of %zu",count ? p->cue_page * 40 + 1 : 0,p->cue_page * 40 + visible,count);
    ca_text(&(Ca_TextDesc){.text = page, .style = FL_CLASS_MUTED});
    fl_button(&(Fl_ButtonDesc){.text = "Next", .on_click = next_cues, .click_data = p, .disabled = (p->cue_page + 1) * 40 >= count});
    ca_div_end();
    const Fl_Phase *owner = NULL; const Fl_SpeechCue *cue = fl_scene_speech_cue(scene,p->selected_cue,&owner);
    if (cue && owner == phase && !cue->retired) {
        ca_text(&(Ca_TextDesc){.text = "Timing in seconds into this phase's audio. Regeneration measures it again.", .wrap = true, .style = FL_CLASS_MUTED});
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .inline_style = "width: 100%; flex-shrink: 0; gap: 8px; flex-wrap: wrap; align-items: center;"});
        ca_text(&(Ca_TextDesc){.text = "Start"});
        ca_input(&(Ca_InputDesc){.text = p->cue_start, .id = "manuscript-cue-start", .input_mode = CA_INPUT_FLOAT, .max_length = 47, .width = 80,
            .style = FL_CLASS_PROPERTY_INPUT, .inline_style = "flex-grow: 0;", .on_change = cue_start_changed, .change_data = p, .disabled = running});
        ca_text(&(Ca_TextDesc){.text = "End"});
        ca_input(&(Ca_InputDesc){.text = p->cue_end, .id = "manuscript-cue-end", .input_mode = CA_INPUT_FLOAT, .max_length = 47, .width = 80,
            .style = FL_CLASS_PROPERTY_INPUT, .inline_style = "flex-grow: 0;", .on_change = cue_end_changed, .change_data = p, .disabled = running});
        fl_button(&(Fl_ButtonDesc){.text = "Set timing", .on_click = repair_cue, .click_data = p, .disabled = running});
        ca_div_end();
    }
}

/* Observe document/selection/playback changes without overwriting unaccepted writing. */
static bool changed(const Fl_Event *event, void *data)
{
    (void)event; Fl_ManuscriptPanel *p = data; invalidate(p); return false;
}

/* Pump worker completion and update elapsed generation status without resetting keyboard focus. */
static void tick(void *data)
{
    Fl_ManuscriptPanel *p = data;
    if (p->building) return;
    fl_speech_pump(p->speech);
    bool running = fl_speech_running(p->speech);
    if (running && !p->was_running) {p->was_running = true; p->started = fl_clock_monotonic_ns(); invalidate(p);}
    if (p->was_running && !running) {
        p->was_running = false;
        invalidate(p);
    }
    if (p->progress_label) {
        char message[768];
        if (running) snprintf(message, sizeof(message), "%s · %.0fs elapsed", fl_speech_message(p->speech),
            (double)(fl_clock_monotonic_ns() - p->started) / 1e9);
        else snprintf(message, sizeof(message), "%s", fl_speech_message(p->speech));
        ca_set_text(p->progress_label, message);
    }
    refresh(p);
    if (p->window) ca_instance_request_frame_after(ca_window_instance(p->window), 0.2);
}

/* Release the asset chooser's owned option labels and stable IDs. */
static void free_choices(Fl_ManuscriptPanel *p)
{
    for (size_t i = 0; i < p->asset_count; ++i) free((char *)p->asset_names[i]);
    free(p->asset_names); free(p->asset_ids); p->asset_names = NULL; p->asset_ids = NULL; p->asset_count = 0;
}

/* Build an audio-only asset chooser with an explicit unselected option. */
static void build_assets(Fl_ManuscriptPanel *p, bool running)
{
    free_choices(p); Fl_Project *project = fl_scene_edit_project(p->edit);
    size_t count = fl_project_asset_count(project), capacity = 1;
    for (size_t i = 0; i < count; ++i) if (fl_project_asset_at(project, i)->kind == FL_ASSET_AUDIO) ++capacity;
    p->asset_names = calloc(capacity, sizeof(*p->asset_names)); p->asset_ids = calloc(capacity, sizeof(*p->asset_ids));
    if (!p->asset_names || !p->asset_ids) {free_choices(p); return;}
    p->asset_names[0] = strdup("Choose from Assets…");
    if (!p->asset_names[0]) {free_choices(p); return;}
    p->asset_count = 1; int selected = 0;
    for (size_t i = 0; i < count; ++i) {
        const Fl_Asset *asset = fl_project_asset_at(project, i);
        if (asset->kind != FL_ASSET_AUDIO) continue;
        char *name = strdup(asset->name);
        if (!name) break;
        size_t index = p->asset_count++; p->asset_names[index] = name; p->asset_ids[index] = asset->id;
        if (asset->id == p->draft->reference) selected = (int)index;
    }
    ca_select(&(Ca_SelectDesc){.options = p->asset_names, .option_count = (int)p->asset_count, .selected = selected,
        .width = 240, .on_change = asset_changed, .change_data = p, .disabled = running});
}

/* Find or create the scene's unaccepted editor draft from its saved voice preferences. */
static Fl_ManuscriptDraft *scene_draft(Fl_ManuscriptPanel *p, const Fl_Scene *scene)
{
    for (Fl_ManuscriptDraft *d = p->drafts; d; d = d->next) if (d->scene == scene->id) return d;
    Fl_ManuscriptDraft *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->scene = scene->id; d->reference = scene->voice_reference; d->reference_mode = d->reference != 0;
    snprintf(d->speaker, sizeof(d->speaker), "%s", scene->voice ? scene->voice : "Ryan");
    snprintf(d->language, sizeof(d->language), "%s", scene->language ? scene->language : "English");
    snprintf(d->transcript, sizeof(d->transcript), "%s", scene->voice_transcript ? scene->voice_transcript : "");
    snprintf(d->title, sizeof(d->title), "Phase %zu", scene->phase_count + 1);
    d->next = p->drafts; p->drafts = d; return d;
}

/* Build the choose → write → generate → preview → accept manuscript workspace. */
void fl_manuscript_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_ManuscriptPanel *p = data; p->building = true; p->dock = dock; p->window = window;
    p->well = NULL; p->progress_label = NULL; p->estimate = NULL;
    p->accept_button = p->generate_button = p->play_button = NULL;
    free(p->rows); p->rows = NULL; p->row_count = 0;
    const Fl_Scene *scene = active_scene(p);
    p->draft = scene ? scene_draft(p, scene) : NULL;
    Fl_ManuscriptDraft *d = p->draft; bool running = fl_speech_running(p->speech);
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "manuscript-body", .style = FL_CLASS_SCRIPTS, .inline_style = "padding: 16px; gap: 12px;"});
    if (!d) {
        ca_text(&(Ca_TextDesc){.text = scene ? "Could not open manuscript." : "Open a scene to write narration.", .style = FL_CLASS_EMPTY});
        ca_div_end(); p->building = false; return;
    }
    ca_text(&(Ca_TextDesc){.text = "Voice", .style = FL_CLASS_HEADING});
    const char *sources[] = {"Built-in voice", "Voice cloning"};
    ca_tabs(&(Ca_TabBarDesc){.labels = sources, .count = 2, .active = d->reference_mode ? 1 : 0,
        .on_change = source_changed, .change_data = p, .disabled = running});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .inline_style = "width: 100%; flex-shrink: 0; gap: 8px; flex-wrap: wrap; align-items: center;"});
    if (!d->reference_mode) {
        size_t count; const char *const *speakers = fl_manuscript_speakers(&count); int selected = 0;
        for (size_t i = 0; i < count; ++i) if (!strcmp(speakers[i], d->speaker)) selected = (int)i;
        ca_select(&(Ca_SelectDesc){.options = speakers, .option_count = (int)count, .selected = selected, .width = 180,
            .on_change = speaker_changed, .change_data = p, .disabled = running});
        char preview[FL_PATH_MAX];
        bool playing = fl_voice_preset_path(d->speaker, preview, sizeof(preview)) && p->playing &&
            fl_audio_player_current(p->player) == p->playing && !strcmp(preview, p->playing_path);
        fl_button(&(Fl_ButtonDesc){.text = playing ? CA_ICON_NF_FA_STOP : CA_ICON_NF_FA_PLAY, .kind = FL_BUTTON_ICON, .tooltip = "Preview this voice",
            .on_click = sample_voice, .click_data = p, .disabled = running});
    } else {
        build_assets(p, running);
        fl_button(&(Fl_ButtonDesc){.text = "Choose reference audio…", .on_click = browse_recording, .click_data = p, .disabled = running});
    }
    size_t count; const char *const *languages = fl_manuscript_languages(&count); int selected = 0;
    for (size_t i = 0; i < count; ++i) if (!strcmp(languages[i], d->language)) selected = (int)i;
    ca_select(&(Ca_SelectDesc){.options = languages, .option_count = (int)count, .selected = selected, .width = 160,
        .on_change = language_changed, .change_data = p, .disabled = running});
    ca_div_end();
    if (d->reference_mode) {
        p->well = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .inline_style = "width: 100%; flex-shrink: 0; padding: 12px; gap: 8px; border: 1px dashed " FL_COLOR_GLASS_BORDER "; border-radius: 8px;"});
        const Fl_Asset *asset = fl_project_asset(fl_scene_edit_project(p->edit), d->reference);
        ca_text(&(Ca_TextDesc){.text = asset ? asset->name : "Drop reference audio here to clone its voice, or choose a file above.", .wrap = true, .style = FL_CLASS_MUTED});
        ca_div_end();
    }
    ca_text(&(Ca_TextDesc){.text = d->phase ? "Edit phase" : "Narration", .style = FL_CLASS_HEADING});
    ca_input(&(Ca_InputDesc){.id = "manuscript-title", .text = d->title, .placeholder = "Phase title", .max_length = FL_PHASE_TITLE_MAX,
        .style = FL_CLASS_PROPERTY_INPUT, .inline_style = "width: 100%; height: 30px; flex-grow: 0; flex-shrink: 0;",
        .on_change = title_changed, .change_data = p, .disabled = running});
    ca_input(&(Ca_InputDesc){.id = "manuscript-narration", .text = d->text, .placeholder = "Write or paste your narration here…",
        .multiline = true, .max_length = FL_PHASE_TEXT_MAX, .style = FL_CLASS_PROPERTY_INPUT,
        .inline_style = "width: 100%; height: 200px; flex-grow: 0; flex-shrink: 0; padding: 10px;",
        .on_change = text_changed, .change_data = p, .disabled = running});
    p->estimate = ca_text(&(Ca_TextDesc){.text = " ", .style = FL_CLASS_MUTED});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .inline_style = "width: 100%; flex-shrink: 0; gap: 8px; flex-wrap: wrap; align-items: center;"});
    p->generate_button = fl_button(&(Fl_ButtonDesc){.text = ready_audio(p) ? "Regenerate" : "Generate", .on_click = generate, .click_data = p});
    p->play_button = fl_button(&(Fl_ButtonDesc){.text = p->playing && fl_audio_player_current(p->player) == p->playing && ready_audio(p) && !strcmp(p->playing_path, ready_audio(p)) ? CA_ICON_NF_FA_STOP : CA_ICON_NF_FA_PLAY,
        .kind = FL_BUTTON_ICON, .tooltip = "Play generated narration", .on_click = play_generated, .click_data = p});
    p->accept_button = fl_button(&(Fl_ButtonDesc){.text = d->phase ? "Save phase" : "Add as a phase", .on_click = accept_phase, .click_data = p});
    if (d->phase) fl_button(&(Fl_ButtonDesc){.text = "New draft", .on_click = new_phase, .click_data = p, .disabled = running});
    if (running) fl_button(&(Fl_ButtonDesc){.text = "Cancel", .on_click = cancel_generation, .click_data = p});
    ca_div_end();
    p->progress_label = ca_text(&(Ca_TextDesc){.text = fl_speech_message(p->speech), .wrap = true, .style = FL_CLASS_MUTED});
    if (p->message[0]) ca_text(&(Ca_TextDesc){.text = p->message, .wrap = true, .style = FL_CLASS_WARNING});
    if (scene->phase_count) {
        ca_text(&(Ca_TextDesc){.text = "Phases", .style = FL_CLASS_HEADING});
        p->rows = calloc(scene->phase_count, sizeof(*p->rows));
        if (p->rows) for (size_t i = 0; i < scene->phase_count; ++i) {
            const Fl_Phase *phase = &scene->phases[i]; Fl_ManuscriptRow *row = &p->rows[p->row_count++];
            *row = (Fl_ManuscriptRow){p, phase->id};
            ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .inline_style = "width: 100%; flex-shrink: 0; padding: 8px; gap: 8px; flex-wrap: wrap; align-items: center; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: 8px;"});
            ca_text(&(Ca_TextDesc){.text = phase->title, .wrap = true});
            fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_FA_PLAY, .kind = FL_BUTTON_ICON, .tooltip = "Play phase narration",
                .on_click = play_phase, .click_data = row, .disabled = !phase_asset(scene, phase)});
            fl_button(&(Fl_ButtonDesc){.text = "Edit", .on_click = edit_phase, .click_data = row, .disabled = running});
            fl_button(&(Fl_ButtonDesc){.text = "Speech cues", .on_click = toggle_cues, .click_data = row, .active = p->expanded_phase == phase->id});
            size_t unresolved = 0;
            for (size_t j = 0; j < phase->cue_count; ++j) {Fl_Tick tick; if (!fl_phase_speech_cue_sample(scene,phase,&phase->cues[j],false,&tick)) ++unresolved;}
            if (unresolved) {char warning[96]; snprintf(warning,sizeof(warning),"%zu cues need review",unresolved); ca_text(&(Ca_TextDesc){.text = warning, .style = FL_CLASS_WARNING});}
            ca_div_end();
            if (p->expanded_phase == phase->id) build_cues(p,scene,phase,running);
        }
    }
    ca_div_end(); p->building = false;
    if (!p->effect) p->effect = ca_frame_effect(ca_window_instance(window), tick, p);
    refresh(p); ca_instance_request_frame_after(ca_window_instance(window), 0.2);
}

/* Clear borrowed widget handles when the dock unmounts this panel. */
void fl_manuscript_panel_teardown(void *data)
{
    Fl_ManuscriptPanel *p = data; p->dock = NULL; p->window = NULL; p->well = NULL;
    p->progress_label = p->estimate = NULL; p->accept_button = p->generate_button = p->play_button = NULL;
}

/* Accept one external recording only when its drop lands on the recording well. */
bool fl_manuscript_panel_drop(Fl_ManuscriptPanel *p, const char *const *paths, int count, float x, float y)
{
    if (!p || !p->well || !paths || count < 1) return false;
    float left, top, width, height; ca_div_screen_rect(p->well, &left, &top, &width, &height);
    if (x < left || y < top || x >= left + width || y >= top + height) return false;
    if (fl_speech_running(p->speech)) {status(p, "Wait for generation to finish before changing the voice reference."); return true;}
    use_recording(p, paths[0]); return true;
}

/* Create a manuscript controller borrowing edit, speech, player and bus for its lifetime. */
Fl_ManuscriptPanel *fl_manuscript_panel_create(Fl_SceneEdit *edit, Fl_Speech *speech, Fl_AudioPlayer *player, Fl_AssetImporter *importer, Fl_EventBus *bus)
{
    if (!edit || !speech || !player || !importer || !bus) return NULL;
    Fl_ManuscriptPanel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = edit; p->speech = speech; p->player = player; p->importer = importer; p->bus = bus;
    const char *events[] = {FL_EVENT_PROJECT_CHANGED, FL_EVENT_SELECTION_CHANGED, FL_EVENT_AUDIO_PLAYBACK};
    for (size_t i = 0; i < sizeof(events) / sizeof(*events); ++i) {
        Fl_EventType type = fl_event_type_find(bus, events[i]);
        if (!type || !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = type, .fn = changed, .user_data = p, .owner = p}).generation) {
            fl_manuscript_panel_destroy(p); return NULL;
        }
    }
    Fl_EventType imported = fl_event_type_find(bus, FL_EVENT_ASSET_IMPORTED);
    if (!imported || !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = imported, .fn = recording_imported, .user_data = p, .owner = p}).generation) {
        fl_manuscript_panel_destroy(p); return NULL;
    }
    return p;
}

/* Destroy the controller, its chooser and unsaved drafts; borrowed services outlive it. */
void fl_manuscript_panel_destroy(Fl_ManuscriptPanel *p)
{
    if (!p) return;
    stop_audio(p); ca_effect_destroy(p->effect); fl_file_browser_destroy(p->browser);
    fl_event_unsubscribe_owner(p->bus, p); free_choices(p); free(p->rows);
    while (p->drafts) {Fl_ManuscriptDraft *next = p->drafts->next; free(p->drafts); p->drafts = next;}
    free(p);
}
