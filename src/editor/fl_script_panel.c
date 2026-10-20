#include "fl_script_panel.h"
#include "ui/fl_button.h"
#include "fl_file_browser.h"
#include "style.h"
#include "components/fl_script.h"
#include "platform/fl_fs.h"
#include "scripting/fl_script_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_ID "scripts"
#define PREVIEW_LINES 120u
#define USER_NAMES 6u
#define NEW_SCRIPT_SOURCE "function on_evaluate(self, time)\nend\n"

typedef struct Fl_ScriptRow { struct Fl_ScriptPanel *panel; Fl_Id script; } Fl_ScriptRow;

struct Fl_ScriptPanel {
    Fl_SceneEdit *edit;
    Fl_ScriptSync *sync;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Ca_Label *status;
    Fl_FileBrowser *browser;
    Fl_ScriptRow *rows;
    size_t row_count;
    Fl_Id selected;
    char name_draft[FL_SCRIPT_NAME_MAX + 1];
    Fl_Id name_draft_for;
    char message[256];
};

/* Rebuild the panel on the next frame. */
static void invalidate(Fl_ScriptPanel *p)
{
    if (p->dock) ca_dock_invalidate_panel(p->dock, PANEL_ID);
}

/* Show (or with "" clear) the status line; it is only emitted while non-empty. */
static void status(Fl_ScriptPanel *p, const char *message)
{
    bool visibility = !p->message[0] != !message[0];
    snprintf(p->message, sizeof(p->message), "%s", message);
    if (visibility) invalidate(p);
    else if (p->status) ca_set_text(p->status, p->message);
}

static const Fl_Scene *active_scene(const Fl_ScriptPanel *p)
{
    return fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
}

/* Pick a name based on base that no script of scene uses, writing it to out (FL_SCRIPT_NAME_MAX + 1 bytes). */
static void unique_name(const Fl_Scene *scene, const char *base, char *out)
{
    snprintf(out, FL_SCRIPT_NAME_MAX + 1, "%s", base);
    for (unsigned n = 2; fl_scene_script_named(scene, out) && n < 10000; ++n) {
        char suffix[16];
        int length = snprintf(suffix, sizeof(suffix), " %u", n);
        snprintf(out, FL_SCRIPT_NAME_MAX + 1, "%.*s%s", (int)(FL_SCRIPT_NAME_MAX - (size_t)length), base, suffix);
    }
}

/* Create a script in the active scene and select it; reports a failure on the status line. */
static bool create_script(Fl_ScriptPanel *p, const char *base, const char *source, size_t size)
{
    const Fl_Scene *scene = active_scene(p);
    char name[FL_SCRIPT_NAME_MAX + 1];
    Fl_Id id = 0;
    if (!scene) return false;
    unique_name(scene, base, name);
    if (!fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene->id, .name = name,
                          .source = source, .source_size = size}, &id)) {
        status(p, "The script could not be created.");
        return false;
    }
    p->selected = id;
    status(p, "");
    invalidate(p);
    return true;
}

static void new_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    create_script(p, "Script", NEW_SCRIPT_SOURCE, strlen(NEW_SCRIPT_SOURCE));
}

/* Create a script from a chosen .lua file; the document keeps its own copy of the text. */
static void import_chosen(const char *const *paths, size_t count, void *data)
{
    Fl_ScriptPanel *p = data;
    if (!count) return;
    size_t size = 0;
    char *source = fl_fs_read_file(paths[0], FL_SCRIPT_SOURCE_LIMIT, &size);
    if (!source || !fl_script_source_valid(source, size)) {free(source); status(p, "That file is empty, too large or not text."); return;}
    const char *leaf = strrchr(paths[0], '/'), *back = strrchr(paths[0], '\\');
    leaf = leaf && back ? (leaf > back ? leaf : back) + 1 : leaf ? leaf + 1 : back ? back + 1 : paths[0];
    char base[FL_SCRIPT_NAME_MAX + 1];
    snprintf(base, sizeof(base), "%s", leaf);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    if (!fl_script_name_valid(base)) snprintf(base, sizeof(base), "Imported script");
    create_script(p, base, source, size);
    free(source);
}

static void import_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    if (!p->window) return;
    if (!p->browser) p->browser = fl_file_browser_create(ca_window_instance(p->window));
    static const char *extensions[] = {"lua"};
    Fl_FileFilter filter = {"Lua scripts", extensions, 1};
    if (!fl_file_browser_show(p->browser, &(Fl_FileBrowserDesc){.mode = FL_FILE_BROWSER_OPEN_FILES, .title = "Import Lua script",
        .confirm_label = "Import", .filters = &filter, .filter_count = 1,
        .initial_path = fl_project_directory(fl_scene_edit_project(p->edit)), .on_confirm = import_chosen, .user_data = p}))
        status(p, "Could not open the file browser.");
}

static void row_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptRow *row = data;
    row->panel->selected = row->script;
    status(row->panel, "");
    invalidate(row->panel);
}

static void name_changed(Ca_TextInput *input, void *data)
{
    Fl_ScriptPanel *p = data;
    snprintf(p->name_draft, sizeof(p->name_draft), "%s", ca_input_text(input));
}

static void rename_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    const Fl_Scene *scene = active_scene(p);
    if (!scene || !fl_scene_script(scene, p->selected)) return;
    if (!fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene->id, .script = p->selected,
                          .name = p->name_draft}, NULL)) {
        status(p, "Names are 1 to 64 characters and unique within the scene.");
        return;
    }
    status(p, "Renamed.");
}

static void edit_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    char error[160] = "";
    if (!fl_script_sync_open(p->sync, fl_scene_edit_scene(p->edit), p->selected, error, sizeof(error))) {status(p, error); return;}
    status(p, "Open in your editor. Saving there updates the script.");
}

static void duplicate_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    const Fl_ScriptSource *script = fl_scene_script(active_scene(p), p->selected);
    if (!script) return;
    char base[FL_SCRIPT_NAME_MAX + 1];
    snprintf(base, sizeof(base), "%.*s copy", (int)(FL_SCRIPT_NAME_MAX - 5), script->name);
    char *source = malloc(script->source_size + 1);
    if (!source) return;
    memcpy(source, script->source, script->source_size + 1);
    create_script(p, base, source, script->source_size);
    free(source);
}

/* Give every selected entity of the active scene this script. */
static void attach_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    const Fl_Scene *scene = active_scene(p);
    Fl_Project *project = fl_scene_edit_project(p->edit);
    Fl_Component component = {0};
    if (!scene || !fl_scene_script(scene, p->selected) || !fl_script_encode(&(Fl_Script){p->selected, true}, &component)) return;
    size_t attached = 0;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        Fl_Id entity = scene->entities[i].id;
        if (!fl_scene_edit_contains(p->edit, entity)) continue;
        attached += fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene->id, .entity = entity, .component = component}, NULL);
    }
    free((void *)component.data);
    status(p, attached ? "Attached to the selection." : "Select an entity first.");
}

/* Delete a script nothing runs; scripts in use are kept and the user is told why. */
static void delete_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    const Fl_Scene *scene = active_scene(p);
    const Fl_ScriptSource *script = fl_scene_script(scene, p->selected);
    if (!script) return;
    size_t uses = fl_scene_script_uses(scene, script->id);
    char text[160];
    if (uses) {
        snprintf(text, sizeof(text), "\"%s\" runs on %zu entit%s. Detach it first.", script->name, uses, uses == 1 ? "y" : "ies");
        status(p, text);
        return;
    }
    snprintf(text, sizeof(text), "Deleted \"%s\".", script->name);
    if (fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_SCRIPT_REMOVE, .scene = scene->id, .script = script->id}, NULL)) {
        p->selected = 0;
        status(p, text);
    } else status(p, "The script could not be deleted.");
}

/* The first line-limited slice of source for display, with a note when more follows. Caller frees. */
static char *preview_of(const Fl_ScriptSource *script)
{
    size_t end = 0, lines = 0;
    while (end < script->source_size && lines < PREVIEW_LINES) {
        const char *newline = memchr(script->source + end, '\n', script->source_size - end);
        end = newline ? (size_t)(newline - script->source) + 1 : script->source_size;
        ++lines;
    }
    size_t more = 0;
    for (size_t i = end; i < script->source_size; ++i) more += script->source[i] == '\n';
    char *text = malloc(end + 48);
    if (!text) return NULL;
    memcpy(text, script->source, end);
    text[end] = 0;
    if (end < script->source_size) snprintf(text + end, 48, "… %zu more lines", more + 1);
    return text;
}

/* Names of the entities that run script, joined for one muted line. */
static void users_text(const Fl_Scene *scene, Fl_Id script, char *out, size_t size)
{
    size_t shown = 0, total = 0, length = 0;
    Fl_Script attached;
    out[0] = 0;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        if (!fl_script_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_SCRIPT), &attached) || attached.script != script) continue;
        ++total;
        if (shown < USER_NAMES && length + 2 < size) {
            length += (size_t)snprintf(out + length, size - length, "%s%s", shown ? ", " : "", scene->entities[i].name);
            if (length >= size) length = size - 1;
            ++shown;
        }
    }
    if (total > shown && length + 16 < size) snprintf(out + length, size - length, " and %zu more", total - shown);
}

/* Evaluator diagnostic of the first entity running script that currently fails, or "". */
static const char *runtime_error(Fl_ScriptPanel *p, const Fl_Scene *scene, Fl_Id script)
{
    const Fl_SceneState *state = fl_scene_edit_state(p->edit);
    Fl_Script attached;
    for (size_t i = 0; state && state->scripts && state->scene == scene->id && i < scene->entity_count; ++i) {
        if (!fl_script_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_SCRIPT), &attached) || attached.script != script) continue;
        const char *error = fl_script_runtime_error(state->scripts, i);
        if (error && error[0]) return error;
    }
    return "";
}

static void emit_row(Fl_ScriptPanel *p, const Fl_Scene *scene, const Fl_ScriptSource *script)
{
    Fl_ScriptRow *row = &p->rows[p->row_count++];
    *row = (Fl_ScriptRow){p, script->id};
    char id[48], uses[24];
    snprintf(id, sizeof(id), "script-%llu", (unsigned long long)script->id);
    snprintf(uses, sizeof(uses), "%zu", fl_scene_script_uses(scene, script->id));
    ca_btn_begin(&(Ca_BtnDesc){.on_click = row_clicked, .click_data = row, .id = id,
        .style = p->selected == script->id ? FL_CLASS_SCRIPT_ROW_SELECTED : FL_CLASS_SCRIPT_ROW});
    ca_text(&(Ca_TextDesc){.text = CA_ICON_NF_FA_CODE, .style = FL_CLASS_SCRIPT_ICON});
    ca_text(&(Ca_TextDesc){.text = script->name, .style = FL_CLASS_SCRIPT_NAME});
    ca_text(&(Ca_TextDesc){.text = uses, .style = FL_CLASS_MUTED});
    ca_btn_end();
}

static void emit_detail(Fl_ScriptPanel *p, const Fl_Scene *scene, const Fl_ScriptSource *script)
{
    if (p->name_draft_for != script->id) {
        snprintf(p->name_draft, sizeof(p->name_draft), "%s", script->name);
        p->name_draft_for = script->id;
    }
    ca_input(&(Ca_InputDesc){.text = p->name_draft, .id = "script-name", .style = FL_CLASS_PROPERTY_INPUT, .on_change = name_changed, .change_data = p});
    char users[256], summary[320];
    users_text(scene, script->id, users, sizeof(users));
    if (users[0]) snprintf(summary, sizeof(summary), "Runs on %s", users);
    else snprintf(summary, sizeof(summary), "Not attached to any entity");
    ca_text(&(Ca_TextDesc){.text = summary, .style = FL_CLASS_MUTED, .wrap = true});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_SCRIPT_ACTIONS});
    static const struct { const char *label, *id; Ca_ClickFn fn; } actions[] = {
        {"Rename", "script-rename", rename_clicked}, {"Edit in editor", "script-edit", edit_clicked}, {"Attach to selection", "script-attach", attach_clicked},
        {"Duplicate", "script-duplicate", duplicate_clicked}, {"Delete", "script-delete", delete_clicked}};
    for (size_t i = 0; i < sizeof(actions) / sizeof(*actions); ++i) {
        fl_button(&(Fl_ButtonDesc){.text = actions[i].label, .id = actions[i].id, .on_click = actions[i].fn, .click_data = p});
    }
    ca_div_end();
    char error[256];
    const char *problem = fl_script_check(script->source, script->source_size, error, sizeof(error)) ? runtime_error(p, scene, script->id) : error;
    if (problem[0]) ca_text(&(Ca_TextDesc){.text = problem, .style = FL_CLASS_WARNING, .wrap = true});
    char *preview = preview_of(script);
    if (preview) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCRIPT_CODE});
        ca_text(&(Ca_TextDesc){.text = preview, .style = FL_CLASS_SCRIPT_SOURCE, .wrap = true});
        ca_div_end();
        free(preview);
    }
}

void fl_script_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_ScriptPanel *p = data; p->dock = dock; p->window = window;
    const Fl_Scene *scene = active_scene(p);
    free(p->rows); p->rows = scene && scene->script_count ? calloc(scene->script_count, sizeof(*p->rows)) : NULL; p->row_count = 0;
    if (scene && scene->script_count && !p->rows) return;
    if (!scene || !fl_scene_script(scene, p->selected)) p->selected = scene && scene->script_count ? scene->scripts[0].id : 0;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCRIPTS});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_PROPERTY_ROW});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_FA_PLUS " New", .id = "script-new", .on_click = new_clicked, .click_data = p,
                               .disabled = !scene});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_FA_FILE_IMPORT " Import…", .id = "script-import", .on_click = import_clicked, .click_data = p,
                                .disabled = !scene});
    ca_div_end();
    p->status = p->message[0] ? ca_text(&(Ca_TextDesc){.text = p->message, .style = FL_CLASS_MUTED, .wrap = true}) : NULL;
    if (!scene || !scene->script_count) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_EMPTY});
        ca_text(&(Ca_TextDesc){.text = CA_ICON_NF_FA_CODE, .style = FL_CLASS_EMPTY_ICON});
        ca_text(&(Ca_TextDesc){.text = "No scripts in this scene", .style = FL_CLASS_MUTED});
        ca_div_end();
    } else {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCRIPT_LIST});
        for (size_t i = 0; i < scene->script_count; ++i) emit_row(p, scene, &scene->scripts[i]);
        ca_div_end();
        const Fl_ScriptSource *selected = fl_scene_script(scene, p->selected);
        if (selected) emit_detail(p, scene, selected);
    }
    ca_div_end();
}

void fl_script_panel_teardown(void *data)
{
    Fl_ScriptPanel *p = data;
    p->dock = NULL; p->window = NULL; p->status = NULL;
}

/* Rebuild when the document changes, so names, users and sources stay current. */
static bool project_changed(const Fl_Event *event, void *data)
{
    (void)event;
    invalidate(data);
    return false;
}

/* Rebuild when the active scene changes. */
static bool selection_changed(const Fl_Event *event, void *data)
{
    (void)event;
    invalidate(data);
    return false;
}

/* Select the requested script and bring the panel forward. */
static bool open_requested(const Fl_Event *event, void *data)
{
    Fl_ScriptPanel *p = data;
    const Fl_ScriptOpen *request = event->payload;
    if (request->scene != fl_scene_edit_scene(p->edit) || !fl_scene_script(active_scene(p), request->script)) return false;
    p->selected = request->script;
    if (p->dock) {ca_dock_open_panel(p->dock, PANEL_ID); ca_dock_focus_panel(p->dock, PANEL_ID);}
    invalidate(p);
    return false;
}

Fl_ScriptPanel *fl_script_panel_create(Fl_SceneEdit *edit, Fl_ScriptSync *sync, Fl_EventBus *bus)
{
    if (!edit || !sync || !bus) return NULL;
    Fl_ScriptPanel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = edit; p->sync = sync; p->bus = bus;
    Fl_EventType open = fl_event_type_register(bus, FL_EVENT_SCRIPT_OPEN, sizeof(Fl_ScriptOpen));
    const struct { Fl_EventType type; Fl_EventFn fn; } handlers[] = {
        {fl_event_type_find(bus, FL_EVENT_PROJECT_CHANGED), project_changed}, {fl_event_type_find(bus, FL_EVENT_SELECTION_CHANGED), selection_changed},
        {open, open_requested}};
    for (size_t i = 0; i < sizeof(handlers) / sizeof(*handlers); ++i) {
        if (handlers[i].type == FL_EVENT_TYPE_INVALID ||
            !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = handlers[i].type, .fn = handlers[i].fn, .user_data = p, .owner = p}).generation) {
            fl_script_panel_destroy(p);
            return NULL;
        }
    }
    return p;
}

void fl_script_panel_destroy(Fl_ScriptPanel *p)
{
    if (!p) return;
    fl_event_unsubscribe_owner(p->bus, p);
    fl_file_browser_destroy(p->browser);
    free(p->rows);
    free(p);
}

bool fl_script_panel_request(Fl_EventBus *bus, Fl_Id scene, Fl_Id script)
{
    Fl_EventType type = fl_event_type_register(bus, FL_EVENT_SCRIPT_OPEN, sizeof(Fl_ScriptOpen));
    Fl_ScriptOpen request = {scene, script};
    return type != FL_EVENT_TYPE_INVALID && fl_event_publish(bus, type, &request, sizeof(request));
}
