#include "fl_manuscript_panel.h"
#include "project/fl_asset_import.h"
#include "project/fl_manuscript.h"
#include "ui/fl_button.h"
#include "style.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 480
#define LINE_CAP 40

typedef struct Fl_PhaseDraft Fl_PhaseDraft;
typedef struct Fl_ManuscriptPanel Fl_ManuscriptPanel;
typedef struct Fl_LineEdit { Fl_ManuscriptPanel *panel; Fl_Id phase; size_t line; } Fl_LineEdit;
typedef struct Fl_PhaseAction { Fl_ManuscriptPanel *panel; Fl_Id phase; } Fl_PhaseAction;
typedef struct Fl_ManuscriptPanel {
    Fl_SceneEdit *edit;
    Fl_Speech *speech;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Ca_Effect *effect;
    Ca_Div *well;
    Ca_Label *total;
    Ca_Label *speech_label;
    Ca_TextInput *transcript_input;
    Fl_PhaseDraft *drafts;
    size_t draft_count;
    const char *speakers[12];
    const char *languages[12];
    char extra_speaker[FL_VOICE_NAME_MAX + 1];
    char extra_language[32];
    char transcript[CA_INPUT_TEXT_MAX];
    char total_text[64];
    char message[192];
    bool transcript_dirty, applying, building, running_ui;
} Fl_ManuscriptPanel;

struct Fl_PhaseDraft {
    Fl_Id id;
    char title[FL_PHASE_TITLE_MAX + 1];
    char *text;
    char *rest;
    char timing_text[64];
    bool dirty;
    bool *newline_after;
    Ca_TextInput **inputs;
    Ca_TextInput *title_input;
    Ca_Label *timing;
    Fl_LineEdit *edits;
    Fl_PhaseAction *action;
    size_t line_count;
};

static void invalidate(Fl_ManuscriptPanel *p)
{
    if (p->dock) ca_dock_invalidate_panel(p->dock, FL_MANUSCRIPT_PANEL_ID);
}

static void status(Fl_ManuscriptPanel *p, const char *message)
{
    snprintf(p->message, sizeof(p->message), "%s", message ? message : "");
    invalidate(p);
}

static const Fl_Scene *active_scene(const Fl_ManuscriptPanel *p)
{
    return fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
}

static Fl_PhaseDraft *draft_by_id(Fl_ManuscriptPanel *p, Fl_Id id)
{
    for (size_t i = 0; i < p->draft_count; ++i) if (p->drafts[i].id == id) return &p->drafts[i];
    return NULL;
}

static void free_lines(Fl_PhaseDraft *d)
{
    free(d->inputs); free(d->newline_after); free(d->edits); free(d->action);
    d->inputs = NULL; d->newline_after = NULL; d->edits = NULL; d->action = NULL;
    d->line_count = 0; d->title_input = NULL; d->timing = NULL;
}

static void free_draft(Fl_PhaseDraft *d)
{
    free_lines(d); free(d->text); free(d->rest); *d = (Fl_PhaseDraft){0};
}

static char *join_lines(const Fl_PhaseDraft *d)
{
    size_t n = 1;
    for (size_t i = 0; i < d->line_count; ++i) n += strlen(ca_input_text(d->inputs[i])) + 1;
    if (d->rest) n += strlen(d->rest);
    char *text = malloc(n);
    if (!text) return NULL;
    text[0] = 0;
    for (size_t i = 0; i < d->line_count; ++i) {
        strcat(text, ca_input_text(d->inputs[i]));
        if (d->newline_after[i]) strcat(text, "\n");
    }
    if (d->rest) strcat(text, d->rest);
    return text;
}

static bool capture_draft(Fl_PhaseDraft *d)
{
    if (!d->inputs) return true;
    char *text = join_lines(d);
    if (!text) return false;
    free(d->text); d->text = text;
    return true;
}

static size_t take_line(const char **cursor, char *out, bool *newline_after)
{
    const char *p = *cursor;
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    bool wrapped = false;
    if (len > LINE_MAX) {
        len = LINE_MAX;
        while (len && ((unsigned char)p[len] & 0xC0) == 0x80) --len;
        if (!len) len = 1;
        wrapped = true;
    }
    memcpy(out, p, len); out[len] = 0;
    p += len;
    *newline_after = !wrapped && *p == '\n';
    if (*newline_after) ++p;
    *cursor = p;
    return len;
}

static void remember_lines(Fl_PhaseDraft *d, char lines[][LINE_MAX + 1])
{
    const char *p = d->text ? d->text : "";
    d->line_count = 0;
    if (!*p) {lines[0][0] = 0; d->newline_after[0] = false; d->line_count = 1; return;}
    while (*p && d->line_count < LINE_CAP) take_line(&p, lines[d->line_count], &d->newline_after[d->line_count]), ++d->line_count;
    if (*p) d->rest = strdup(p);
}

static void refresh_total(Fl_ManuscriptPanel *p)
{
    size_t words = 0;
    for (size_t i = 0; i < p->draft_count; ++i) {
        Fl_PhaseDraft *d = &p->drafts[i];
        const char *text = d->text ? d->text : "";
        words += fl_manuscript_words(text);
        double seconds = fl_manuscript_seconds(text);
        char clock[32];
        fl_manuscript_format(seconds, clock, sizeof(clock));
        snprintf(d->timing_text, sizeof(d->timing_text), "%s · %zu words", clock, fl_manuscript_words(text));
        if (d->timing) ca_set_text(d->timing, d->timing_text);
    }
    double minutes = (double)words / (double)FL_NARRATION_WPM;
    snprintf(p->total_text, sizeof(p->total_text), "%.1f min · %zu words", minutes, words);
    if (p->total) ca_set_text(p->total, p->total_text);
}

static void note_line(Ca_TextInput *input, void *data)
{
    (void)input;
    Fl_LineEdit *edit = data;
    Fl_PhaseDraft *d = draft_by_id(edit->panel, edit->phase);
    if (!d || !capture_draft(d)) return;
    d->dirty = true;
    refresh_total(edit->panel);
}

static void note_title(Ca_TextInput *input, void *data)
{
    Fl_PhaseAction *action = data;
    Fl_PhaseDraft *d = draft_by_id(action->panel, action->phase);
    if (!d) return;
    snprintf(d->title, sizeof(d->title), "%s", ca_input_text(input));
    d->dirty = true;
}

static bool any_focused(const Fl_ManuscriptPanel *p)
{
    if (p->transcript_input && ca_input_is_focused(p->transcript_input)) return true;
    for (size_t i = 0; i < p->draft_count; ++i) {
        const Fl_PhaseDraft *d = &p->drafts[i];
        if (d->title_input && ca_input_is_focused(d->title_input)) return true;
        for (size_t line = 0; line < d->line_count; ++line) if (d->inputs && d->inputs[line] && ca_input_is_focused(d->inputs[line])) return true;
    }
    return false;
}

/* 1 applied, 0 nothing pending, -1 the document rejected the edit. */
static int commit_drafts(Fl_ManuscriptPanel *p)
{
    const Fl_Scene *scene = active_scene(p);
    if (!scene) return 0;
    bool needed = p->transcript_dirty;
    for (size_t i = 0; i < p->draft_count; ++i) needed |= p->drafts[i].dirty;
    if (!needed) return 0;
    for (size_t i = 0; i < p->draft_count; ++i) if (p->drafts[i].dirty && !capture_draft(&p->drafts[i])) {status(p, "Could not update the manuscript."); return -1;}
    Fl_Project *project = fl_scene_edit_project(p->edit);
    p->applying = true;
    bool began = fl_project_edit_begin(project, "Edit manuscript", false);
    bool ok = began;
    for (size_t i = 0; ok && i < p->draft_count; ++i) {
        Fl_PhaseDraft *d = &p->drafts[i];
        if (!d->dirty) continue;
        const Fl_Phase *phase = fl_scene_phase(scene, d->id);
        if (!phase) {d->dirty = false; continue;}
        const char *title = d->title_input ? ca_input_text(d->title_input) : d->title;
        if (!title[0] || strlen(title) > FL_PHASE_TITLE_MAX || !d->text || strlen(d->text) > FL_PHASE_TEXT_MAX) ok = false;
        else ok = fl_project_apply(project, &(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene->id, .phase = d->id,
            .name = title, .source = d->text, .beat = phase->beat, .clip = phase->clip}, NULL);
        scene = active_scene(p);
    }
    if (ok && p->transcript_dirty) {
        scene = active_scene(p);
        const char *voice = scene && scene->voice ? scene->voice : "Ryan";
        const char *language = scene && scene->language ? scene->language : "English";
        ok = scene && fl_project_apply(project, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene->id, .name = voice, .language = language,
            .asset = scene->voice_reference, .source = p->transcript}, NULL);
    }
    if (began) {
        if (ok) ok = fl_project_edit_commit(project);
        else fl_project_edit_cancel(project);
    }
    p->applying = false;
    if (!ok) {status(p, "Could not update the manuscript."); return -1;}
    for (size_t i = 0; i < p->draft_count; ++i) p->drafts[i].dirty = false;
    p->transcript_dirty = false;
    p->message[0] = 0;
    invalidate(p);
    return 1;
}

static void note_transcript(Ca_TextInput *input, void *data)
{
    Fl_ManuscriptPanel *p = data;
    snprintf(p->transcript, sizeof(p->transcript), "%s", ca_input_text(input));
    p->transcript_dirty = true;
}

static void focused_tick(void *data)
{
    Fl_ManuscriptPanel *p = data;
    if (p->building) return;
    fl_speech_pump(p->speech);
    if (p->speech_label) ca_set_text(p->speech_label, fl_speech_message(p->speech));
    if (!any_focused(p)) commit_drafts(p);
    if (p->running_ui != fl_speech_running(p->speech)) invalidate(p);
    if (p->window) ca_instance_request_frame_after(ca_window_instance(p->window), 0.25);
}

static void choose_voice(Fl_ManuscriptPanel *p, const char *voice, const char *language)
{
    const Fl_Scene *scene = active_scene(p);
    if (!scene || p->applying) return;
    if (!voice) voice = scene->voice ? scene->voice : "Ryan";
    if (!language) language = scene->language ? scene->language : "English";
    const char *transcript = p->transcript_dirty ? p->transcript : scene->voice_transcript;
    p->applying = true;
    bool ok = fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene->id, .name = voice,
        .language = language, .asset = scene->voice_reference, .source = transcript}, NULL);
    p->applying = false;
    if (!ok) status(p, "That voice could not be selected.");
    else {p->transcript_dirty = false; p->message[0] = 0; invalidate(p);}
}

static void speaker_changed(Ca_Select *select, void *data)
{
    Fl_ManuscriptPanel *p = data;
    int index = ca_select_get(select);
    if (index >= 0 && index < (int)(sizeof(p->speakers) / sizeof(*p->speakers)) && p->speakers[index]) choose_voice(p, p->speakers[index], NULL);
}

static void language_changed(Ca_Select *select, void *data)
{
    Fl_ManuscriptPanel *p = data;
    int index = ca_select_get(select);
    if (index >= 0 && index < (int)(sizeof(p->languages) / sizeof(*p->languages)) && p->languages[index]) choose_voice(p, NULL, p->languages[index]);
}

static void add_phase(Ca_Button *button, void *data)
{
    (void)button;
    Fl_ManuscriptPanel *p = data;
    if (any_focused(p)) commit_drafts(p);
    const Fl_Scene *scene = active_scene(p);
    if (!scene || fl_speech_running(p->speech)) return;
    char title[32];
    snprintf(title, sizeof(title), "Phase %zu", scene->phase_count + 1);
    if (!fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene->id, .name = title,
        .source = "", .position = scene->phase_count}, NULL)) status(p, "Could not add a phase.");
    else {p->message[0] = 0; invalidate(p);}
}

static void generate(Ca_Button *button, void *data)
{
    (void)button;
    Fl_PhaseAction *action = data;
    Fl_ManuscriptPanel *p = action->panel;
    if (commit_drafts(p) < 0) return;
    const Fl_Scene *scene = active_scene(p);
    if (!scene) return;
    if (!fl_speech_start(p->speech, scene->id, action->phase)) status(p, fl_speech_message(p->speech));
    else {p->message[0] = 0; invalidate(p);}
}

static void generate_all(Ca_Button *button, void *data)
{
    (void)button;
    Fl_PhaseAction action = {data, 0};
    generate(NULL, &action);
}

static void delete_phase(Ca_Button *button, void *data)
{
    (void)button;
    Fl_PhaseAction *action = data;
    Fl_ManuscriptPanel *p = action->panel;
    const Fl_Scene *scene = active_scene(p);
    if (!scene || fl_speech_running(p->speech)) return;
    Fl_PhaseDraft *d = draft_by_id(p, action->phase);
    if (d) d->dirty = false;
    p->applying = true;
    bool ok = fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_PHASE_REMOVE, .scene = scene->id, .phase = action->phase}, NULL);
    p->applying = false;
    if (!ok) status(p, "Could not delete the phase.");
    else status(p, "Deleted the phase. Its beat stays on the timeline.");
}

static void add_line(Ca_Button *button, void *data)
{
    (void)button;
    Fl_PhaseAction *action = data;
    Fl_PhaseDraft *d = draft_by_id(action->panel, action->phase);
    if (!d || !capture_draft(d)) return;
    size_t n = strlen(d->text ? d->text : "");
    char *text = malloc(n + 2);
    if (!text) return;
    memcpy(text, d->text ? d->text : "", n);
    text[n] = '\n'; text[n + 1] = 0;
    free(d->text); d->text = text; d->dirty = true;
    invalidate(action->panel);
}

static void paste_phase(Ca_Button *button, void *data)
{
    (void)button;
    Fl_PhaseAction *action = data;
    Fl_ManuscriptPanel *p = action->panel;
    Fl_PhaseDraft *d = draft_by_id(p, action->phase);
    const char *clip = p->window ? ca_clipboard_get_text(p->window) : NULL;
    if (!d || !clip || !clip[0]) {status(p, "Clipboard is empty."); return;}
    if (!capture_draft(d)) return;
    size_t n = strlen(d->text ? d->text : ""), extra = strlen(clip);
    bool gap = n && d->text[n - 1] != '\n';
    char *text = malloc(n + extra + 2);
    if (!text) return;
    memcpy(text, d->text ? d->text : "", n);
    if (gap) text[n++] = '\n';
    memcpy(text + n, clip, extra + 1);
    free(d->text); d->text = text; d->dirty = true;
    commit_drafts(p);
}

static void clear_reference(Ca_Button *button, void *data)
{
    (void)button;
    Fl_ManuscriptPanel *p = data;
    const Fl_Scene *scene = active_scene(p);
    if (!scene) return;
    const char *transcript = p->transcript_dirty ? p->transcript : scene->voice_transcript;
    p->applying = true;
    bool ok = fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene->id,
        .name = scene->voice ? scene->voice : "Ryan", .language = scene->language ? scene->language : "English",
        .asset = 0, .source = transcript}, NULL);
    p->applying = false;
    if (!ok) status(p, "Could not clear the cloned voice.");
    else {p->transcript_dirty = false; status(p, "Using the built-in voice.");}
}

static bool project_changed(const Fl_Event *event, void *data)
{
    (void)event;
    Fl_ManuscriptPanel *p = data;
    if (p->building || p->applying) return false;
    if (!commit_drafts(p)) invalidate(p);
    return false;
}

static bool selection_changed(const Fl_Event *event, void *data)
{
    return project_changed(event, data);
}

static bool speech_changed(const Fl_Event *event, void *data)
{
    (void)event;
    Fl_ManuscriptPanel *p = data;
    if (p->building || p->applying) return false;
    if (p->speech_label) ca_set_text(p->speech_label, fl_speech_message(p->speech));
    if (p->running_ui != fl_speech_running(p->speech)) invalidate(p);
    return false;
}

static void sync_drafts(Fl_ManuscriptPanel *p, const Fl_Scene *scene)
{
    size_t count = scene ? scene->phase_count : 0;
    Fl_PhaseDraft *next = count ? calloc(count, sizeof(*next)) : NULL;
    if (count && !next) return;
    for (size_t i = 0; i < count; ++i) {
        const Fl_Phase *phase = &scene->phases[i];
        Fl_PhaseDraft *old = draft_by_id(p, phase->id);
        if (old && old->dirty) {next[i] = *old; *old = (Fl_PhaseDraft){0};}
        else {
            next[i].id = phase->id;
            next[i].text = strdup(phase->text ? phase->text : "");
            snprintf(next[i].title, sizeof(next[i].title), "%s", phase->title ? phase->title : "");
        }
    }
    for (size_t i = 0; i < p->draft_count; ++i) free_draft(&p->drafts[i]);
    free(p->drafts); p->drafts = next; p->draft_count = count;
}

static int fill_choice(const char *const *known, size_t known_count, const char *current, const char **out, char *extra, size_t extra_cap)
{
    int selected = 0, count = 0;
    for (size_t i = 0; i < known_count && count < 11; ++i) {
        out[count] = known[i];
        if (current && !strcmp(current, known[i])) selected = count;
        ++count;
    }
    if (current && current[0]) {
        bool found = false;
        for (int i = 0; i < count; ++i) found |= !strcmp(out[i], current);
        if (!found && count < 12) {snprintf(extra, extra_cap, "%s", current); out[count] = extra; selected = count++;}
    }
    return selected;
}

static void emit_phase(Fl_ManuscriptPanel *p, Fl_PhaseDraft *d)
{
    char lines[LINE_CAP][LINE_MAX + 1];
    free(d->rest); d->rest = NULL; free_lines(d);
    size_t slots = 1;
    const char *scan = d->text ? d->text : "";
    if (*scan) {
        slots = 0; const char *ptext = scan;
        while (*ptext && slots < LINE_CAP) {char scratch[LINE_MAX + 1]; bool nl; take_line(&ptext, scratch, &nl); ++slots;}
    }
    d->inputs = calloc(slots, sizeof(*d->inputs));
    d->newline_after = calloc(slots, sizeof(*d->newline_after));
    d->edits = calloc(slots, sizeof(*d->edits));
    d->action = calloc(1, sizeof(*d->action));
    if (!d->inputs || !d->newline_after || !d->edits || !d->action) {free_lines(d); return;}
    d->action->panel = p; d->action->phase = d->id;
    remember_lines(d, lines);
    bool running = fl_speech_running(p->speech);
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .inline_style = "width: 100%; gap: 4px; padding: 8px; border: 1px solid rgba(255,255,255,0.08); border-radius: 8px;"});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_PROPERTY_ROW});
    d->title_input = ca_input(&(Ca_InputDesc){.text = d->title, .style = FL_CLASS_PROPERTY_INPUT, .on_change = note_title, .change_data = d->action, .disabled = running});
    d->timing = ca_text(&(Ca_TextDesc){.text = d->timing_text, .style = FL_CLASS_MUTED});
    fl_button(&(Fl_ButtonDesc){.text = "Generate", .on_click = generate, .click_data = d->action, .disabled = running});
    fl_button(&(Fl_ButtonDesc){.text = "Delete", .on_click = delete_phase, .click_data = d->action, .disabled = running, .destructive = true});
    ca_div_end();
    for (size_t i = 0; i < d->line_count; ++i) {
        d->edits[i] = (Fl_LineEdit){p, d->id, i};
        d->inputs[i] = ca_input(&(Ca_InputDesc){.text = lines[i], .style = FL_CLASS_PROPERTY_INPUT, .on_change = note_line, .change_data = &d->edits[i], .disabled = running});
    }
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_PROPERTY_ROW});
    fl_button(&(Fl_ButtonDesc){.text = "Add line", .on_click = add_line, .click_data = d->action, .disabled = running});
    fl_button(&(Fl_ButtonDesc){.text = "Paste", .on_click = paste_phase, .click_data = d->action, .disabled = running});
    ca_div_end();
    ca_div_end();
}

void fl_manuscript_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_ManuscriptPanel *p = data;
    p->building = true; p->dock = dock; p->window = window; p->well = NULL; p->total = NULL; p->speech_label = NULL; p->transcript_input = NULL;
    memset(p->speakers, 0, sizeof(p->speakers)); memset(p->languages, 0, sizeof(p->languages));
    const Fl_Scene *scene = active_scene(p);
    sync_drafts(p, scene);
    for (size_t i = 0; i < p->draft_count; ++i) if (!p->drafts[i].timing_text[0]) snprintf(p->drafts[i].timing_text, sizeof(p->drafts[i].timing_text), "0:00 · 0 words");
    refresh_total(p);
    size_t speakers_n = 0, languages_n = 0;
    const char *const *speakers = fl_manuscript_speakers(&speakers_n);
    const char *const *languages = fl_manuscript_languages(&languages_n);
    const char *voice = scene && scene->voice ? scene->voice : "Ryan";
    const char *language = scene && scene->language ? scene->language : "English";
    int speaker = fill_choice(speakers, speakers_n, voice, p->speakers, p->extra_speaker, sizeof(p->extra_speaker));
    int language_index = fill_choice(languages, languages_n, language, p->languages, p->extra_language, sizeof(p->extra_language));
    int speaker_count = 0, language_count = 0;
    while (speaker_count < 12 && p->speakers[speaker_count]) ++speaker_count;
    while (language_count < 12 && p->languages[language_count]) ++language_count;
    bool running = fl_speech_running(p->speech);
    p->running_ui = running;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCRIPTS});
    ca_text(&(Ca_TextDesc){.text = "Write the narration in phases. About 150 words a minute. Generate uses the bundled Qwen3-TTS voice and drops the audio on the phase beat.",
        .wrap = true, .style = FL_CLASS_MUTED});
    p->total = ca_text(&(Ca_TextDesc){.text = p->total_text, .style = FL_CLASS_HEADING});
    if (!scene) {
        ca_text(&(Ca_TextDesc){.text = "Open a scene to write its manuscript.", .style = FL_CLASS_EMPTY});
        ca_div_end(); p->building = false; return;
    }
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_PROPERTY_ROW});
    ca_select(&(Ca_SelectDesc){.options = p->speakers, .option_count = speaker_count, .selected = speaker, .on_change = speaker_changed, .change_data = p, .disabled = running});
    ca_select(&(Ca_SelectDesc){.options = p->languages, .option_count = language_count, .selected = language_index, .on_change = language_changed, .change_data = p, .disabled = running});
    ca_div_end();
    const Fl_Asset *reference = scene->voice_reference ? fl_project_asset(fl_scene_edit_project(p->edit), scene->voice_reference) : NULL;
    char well[160];
    if (reference) snprintf(well, sizeof(well), "Reference: %s", reference->name);
    else snprintf(well, sizeof(well), "Drop a recording here to clone a voice");
    p->well = ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .inline_style = "width: 100%; padding: 8px; gap: 8px; border: 1px dashed rgba(255,255,255,0.25); border-radius: 8px;"});
    ca_text(&(Ca_TextDesc){.text = well, .style = FL_CLASS_MUTED});
    if (reference) fl_button(&(Fl_ButtonDesc){.text = "Clear", .on_click = clear_reference, .click_data = p, .disabled = running});
    ca_div_end();
    if (!p->transcript_dirty) snprintf(p->transcript, sizeof(p->transcript), "%s", scene->voice_transcript ? scene->voice_transcript : "");
    p->transcript_input = ca_input(&(Ca_InputDesc){.text = p->transcript, .placeholder = "Transcript of the recording (optional)",
        .style = FL_CLASS_PROPERTY_INPUT, .on_change = note_transcript, .change_data = p, .disabled = running});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_PROPERTY_ROW});
    fl_button(&(Fl_ButtonDesc){.text = "Add phase", .on_click = add_phase, .click_data = p, .disabled = running});
    fl_button(&(Fl_ButtonDesc){.text = "Generate all", .on_click = generate_all, .click_data = p, .disabled = running});
    ca_div_end();
    if (!p->draft_count) ca_text(&(Ca_TextDesc){.text = "No phases yet.", .style = FL_CLASS_EMPTY});
    for (size_t i = 0; i < p->draft_count; ++i) emit_phase(p, &p->drafts[i]);
    const char *speech = fl_speech_message(p->speech);
    p->speech_label = ca_text(&(Ca_TextDesc){.text = speech && speech[0] ? speech : " ", .style = FL_CLASS_MUTED});
    if (p->message[0]) ca_text(&(Ca_TextDesc){.text = p->message, .style = FL_CLASS_WARNING});
    ca_div_end();
    if (!p->effect) p->effect = ca_frame_effect(ca_window_instance(window), focused_tick, p);
    if (p->window) ca_instance_request_frame_after(ca_window_instance(p->window), 0.25);
    refresh_total(p);
    p->building = false;
}

void fl_manuscript_panel_teardown(void *data)
{
    Fl_ManuscriptPanel *p = data;
    for (size_t i = 0; i < p->draft_count; ++i) {
        if (p->drafts[i].inputs) capture_draft(&p->drafts[i]);
        free_lines(&p->drafts[i]);
    }
    p->dock = NULL; p->window = NULL; p->well = NULL; p->total = NULL; p->speech_label = NULL; p->transcript_input = NULL;
}

bool fl_manuscript_panel_drop(Fl_ManuscriptPanel *panel, const char *const *paths, int count, float x, float y)
{
    if (!panel || !panel->well || !paths || count < 1) return false;
    float left, top, width, height;
    ca_div_screen_rect(panel->well, &left, &top, &width, &height);
    if (x < left || y < top || x >= left + width || y >= top + height) return false;
    const Fl_Scene *scene = active_scene(panel);
    if (!scene) {status(panel, "Open a scene before cloning a voice."); return true;}
    Fl_Project *project = fl_scene_edit_project(panel->edit);
    Fl_StoredAsset stored = {0};
    Fl_ImportStatus imported = fl_asset_store_file(fl_project_directory(project), paths[0], &stored);
    if (imported == FL_IMPORT_UNSUPPORTED || (imported != FL_IMPORT_FAILED && stored.kind != FL_ASSET_AUDIO)) {
        free(stored.source);
        status(panel, "Drop an audio recording to clone a voice.");
        return true;
    }
    panel->applying = true;
    bool begun = fl_project_edit_begin(project, "Clone voice", false);
    Fl_Id asset = 0;
    bool ok = begun && (imported == FL_IMPORT_ADDED || imported == FL_IMPORT_EXISTING);
    if (ok) imported = fl_asset_record(project, &stored, "Voice reference", &asset);
    else free(stored.source);
    ok = ok && (imported == FL_IMPORT_ADDED || imported == FL_IMPORT_EXISTING) && asset;
    const char *transcript = panel->transcript_dirty ? panel->transcript : scene->voice_transcript;
    if (ok) ok = fl_project_apply(project, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene->id,
        .name = scene->voice ? scene->voice : "Ryan", .language = scene->language ? scene->language : "English",
        .asset = asset, .source = transcript}, NULL);
    if (ok) ok = fl_project_edit_commit(project);
    else if (begun) fl_project_edit_cancel(project);
    panel->applying = false;
    if (!ok) status(panel, "Could not use that recording as a voice.");
    else {panel->transcript_dirty = false; status(panel, "Cloned voice from the recording.");}
    return true;
}

Fl_ManuscriptPanel *fl_manuscript_panel_create(Fl_SceneEdit *edit, Fl_Speech *speech, Fl_EventBus *bus)
{
    if (!edit || !speech || !bus) return NULL;
    Fl_ManuscriptPanel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = edit; p->speech = speech; p->bus = bus;
    const struct { Fl_EventType type; Fl_EventFn fn; } handlers[] = {
        {fl_event_type_find(bus, FL_EVENT_PROJECT_CHANGED), project_changed},
        {fl_event_type_find(bus, FL_EVENT_SELECTION_CHANGED), selection_changed},
        {fl_event_type_find(bus, FL_EVENT_SPEECH), speech_changed},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(*handlers); ++i) {
        if (handlers[i].type == FL_EVENT_TYPE_INVALID ||
            !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = handlers[i].type, .fn = handlers[i].fn, .user_data = p, .owner = p}).generation) {
            fl_manuscript_panel_destroy(p); return NULL;
        }
    }
    return p;
}

void fl_manuscript_panel_destroy(Fl_ManuscriptPanel *p)
{
    if (!p) return;
    ca_effect_destroy(p->effect);
    if (p->bus) fl_event_unsubscribe_owner(p->bus, p);
    for (size_t i = 0; i < p->draft_count; ++i) free_draft(&p->drafts[i]);
    free(p->drafts); free(p);
}
