#include "project/fl_source_asset.h"
#include "fl_script_panel.h"
#include "platform/fl_process.h"
#include "platform/fl_paths.h"
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
#define NEW_SCRIPT_SOURCE "return { sample = function(ctx) local self=ctx.entity; local time=ctx.time.seconds; \nend\n }"

typedef struct Fl_ScriptRow { struct Fl_ScriptPanel *panel; Fl_Id script; } Fl_ScriptRow;

struct Fl_ScriptPanel {
    Fl_SceneEdit *edit;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Ca_Label *status;
    Fl_FileBrowser *browser;
    Fl_ScriptRow *rows;
    size_t row_count;
    Fl_Id selected;
    char name_draft[FL_SOURCE_NAME_MAX + 1];
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

/* Create a project script and select it; reports a failure on the status line. */
static bool create_script(Fl_ScriptPanel *p, const char *base, const char *source, size_t size)
{
    Fl_Project *project=fl_scene_edit_project(p->edit);
    char name[FL_SOURCE_NAME_MAX + 1];
    Fl_Id id = 0;
    if (!fl_source_asset_unique_name(project,FL_ASSET_LUA,base,name)) {status(p,"Could not name the script."); return false;}
    if (!fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .name = name,
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
    char base[FL_SOURCE_NAME_MAX + 1];
    snprintf(base, sizeof(base), "%s", leaf);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    if (!fl_source_asset_name_valid(base)) snprintf(base, sizeof(base), "Imported script");
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
    Fl_Project *project=fl_scene_edit_project(p->edit);
    if (!fl_project_asset(project,p->selected)) return;
    if (!fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .asset = p->selected,
                          .name = p->name_draft}, NULL)) {
        status(p, "Names are 1 to 64 characters and unique within the project.");
        return;
    }
    status(p, "Renamed.");
}

static void edit_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    Fl_Project *project=fl_scene_edit_project(p->edit);
    const Fl_Asset *asset=fl_project_asset(project,p->selected); char path[FL_PATH_MAX];
    if (!asset || asset->kind!=FL_ASSET_LUA || !fl_project_save(project) || !fl_project_asset_path(project,asset,path,sizeof(path)) || !fl_open_path(path)) {
        status(p,"Could not save or open the Lua asset."); return;
    }
    status(p, "Open in your editor. Saving there updates the script.");
}

static void duplicate_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    const Fl_Asset *script = fl_project_asset(fl_scene_edit_project(p->edit), p->selected);
    if (!script) return;
    char base[FL_SOURCE_NAME_MAX + 1];
    snprintf(base, sizeof(base), "%.*s copy", (int)(FL_SOURCE_NAME_MAX - 5), script->name);
    char *source = malloc(script->size + 1);
    if (!source) return;
    memcpy(source, script->source, script->size + 1);
    create_script(p, base, source, script->size);
    free(source);
}

/* Give every selected entity of the active scene this script. */
static void attach_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    const Fl_Scene *scene = active_scene(p);
    Fl_Project *project = fl_scene_edit_project(p->edit);
    Fl_Component component = {0};
    if (!scene || !fl_project_asset(scene->project, p->selected) || !fl_script_encode(&(Fl_Script){.script=p->selected,.enabled=true}, &component)) return;
    size_t attached = 0;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        Fl_Id entity = scene->entities[i].id;
        if (!fl_scene_edit_contains(p->edit, entity)) continue;
        const Fl_Component *existing=fl_entity_script_component(&scene->entities[i],p->selected);
        component.id=existing ? existing->id : 0;
        attached += fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene->id, .entity = entity, .component = component}, NULL);
    }
    free((void *)component.data);
    status(p, attached ? "Attached to the selection." : "Select an entity first.");
}

/* Delete a script nothing runs; scripts in use are kept and the user is told why. */
static void delete_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_ScriptPanel *p = data;
    Fl_Project *project=fl_scene_edit_project(p->edit);
    const Fl_Asset *script = fl_project_asset(project, p->selected);
    if (!script) return;
    size_t uses = fl_project_asset_uses(project, script->id);
    char text[160];
    if (uses) {
        snprintf(text, sizeof(text), "\"%s\" has %zu attachment%s. Detach it first.", script->name, uses, uses == 1 ? "" : "s");
        status(p, text);
        return;
    }
    snprintf(text, sizeof(text), "Deleted \"%s\".", script->name);
    if (fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = script->id}, NULL)) {
        p->selected = 0;
        status(p, text);
    } else status(p, "The script could not be deleted.");
}

/* The first line-limited slice of source for display, with a note when more follows. Caller frees. */
static char *preview_of(const Fl_Asset *script)
{
    size_t end = 0, lines = 0;
    while (end < script->size && lines < PREVIEW_LINES) {
        const char *newline = memchr(script->source + end, '\n', script->size - end);
        end = newline ? (size_t)(newline - script->source) + 1 : script->size;
        ++lines;
    }
    size_t more = 0;
    for (size_t i = end; i < script->size; ++i) more += script->source[i] == '\n';
    char *text = malloc(end + 48);
    if (!text) return NULL;
    memcpy(text, script->source, end);
    text[end] = 0;
    if (end < script->size) snprintf(text + end, 48, "… %zu more lines", more + 1);
    return text;
}

/* Names of the entities that run script, joined for one muted line. */
static void users_text(const Fl_Project *project, Fl_Id script, char *out, size_t size)
{
    size_t shown = 0, total = 0, length = 0;
    out[0] = 0;
    for (size_t j=0;j<fl_project_scene_count(project);++j) {
      const Fl_Scene *scene=fl_project_scene_at(project,j);
      for (size_t i = 0; i < scene->entity_count; ++i) {
        if (!fl_entity_script_component(&scene->entities[i],script)) continue;
        ++total;
        if (shown < USER_NAMES && length + 2 < size) {
            length += (size_t)snprintf(out + length, size - length, "%s%s", shown ? ", " : "", scene->entities[i].name);
            if (length >= size) length = size - 1;
            ++shown;
        }
    }
    }
    if (total > shown && length + 16 < size) snprintf(out + length, size - length, " and %zu more", total - shown);
}

/* Evaluator diagnostic of the first entity running script that currently fails, or "". */
static const char *runtime_error(Fl_ScriptPanel *p, const Fl_Scene *scene, Fl_Id script)
{
    const Fl_SceneState *state = fl_scene_edit_state(p->edit);
    for (size_t i = 0; scene && state && fl_scene_state_scripts(state) && state->scene == scene->id && i < scene->entity_count; ++i) {
        if (!fl_entity_script_component(&scene->entities[i],script)) continue;
        const char *error = fl_script_runtime_error(fl_scene_state_scripts(state), i);
        if (error && error[0]) return error;
    }
    return "";
}

static void emit_row(Fl_ScriptPanel *p, const Fl_Asset *script)
{
    Fl_ScriptRow *row = &p->rows[p->row_count++];
    *row = (Fl_ScriptRow){p, script->id};
    char id[48], uses[24];
    snprintf(id, sizeof(id), "script-%llu", (unsigned long long)script->id);
    snprintf(uses, sizeof(uses), "%zu", fl_project_asset_uses(fl_scene_edit_project(p->edit), script->id));
    ca_btn_begin(&(Ca_BtnDesc){.on_click = row_clicked, .click_data = row, .id = id,
        .style = p->selected == script->id ? FL_CLASS_SCRIPT_ROW_SELECTED : FL_CLASS_SCRIPT_ROW});
    ca_text(&(Ca_TextDesc){.text = CA_ICON_NF_FA_CODE, .style = FL_CLASS_SCRIPT_ICON});
    ca_text(&(Ca_TextDesc){.text = script->name, .style = FL_CLASS_SCRIPT_NAME});
    ca_text(&(Ca_TextDesc){.text = uses, .style = FL_CLASS_MUTED});
    ca_btn_end();
}

static void emit_detail(Fl_ScriptPanel *p, const Fl_Scene *scene, const Fl_Asset *script)
{
    if (p->name_draft_for != script->id) {
        snprintf(p->name_draft, sizeof(p->name_draft), "%s", script->name);
        p->name_draft_for = script->id;
    }
    ca_input(&(Ca_InputDesc){.text = p->name_draft, .id = "script-name", .style = FL_CLASS_PROPERTY_INPUT, .on_change = name_changed, .change_data = p});
    char users[256], summary[320];
    users_text(fl_scene_edit_project(p->edit), script->id, users, sizeof(users));
    if (users[0]) snprintf(summary, sizeof(summary), "Runs on %s", users);
    else snprintf(summary, sizeof(summary), "No scene attachments");
    size_t prototype_uses=0;
    Fl_Project *project=fl_scene_edit_project(p->edit);
    for (size_t i=0;i<fl_project_asset_count(project);++i) {
        const Fl_Asset *asset=fl_project_asset_at(project,i);
        for (size_t j=0;j<asset->reference_count;++j) prototype_uses+=asset->references[j]==script->id;
    }
    if (prototype_uses) {
        size_t length=strlen(summary);
        snprintf(summary+length,sizeof(summary)-length," · %zu prototype attachment%s",prototype_uses,prototype_uses==1 ? "" : "s");
    }
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
    const char *problem = fl_script_check(script->source, script->size, error, sizeof(error)) ? runtime_error(p, scene, script->id) : error;
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
    Fl_Project *project=fl_scene_edit_project(p->edit);
    size_t total=fl_project_asset_count(project), count=0; Fl_Id first=0;
    for (size_t i=0;i<total;++i) {const Fl_Asset *a=fl_project_asset_at(project,i); if (a->kind==FL_ASSET_LUA) {++count; if (!first) first=a->id;}}
    free(p->rows); p->rows=count ? calloc(count,sizeof(*p->rows)) : NULL; p->row_count=0;
    if (count && !p->rows) return;
    const Fl_Asset *current=fl_project_asset(project,p->selected);
    if (!current || current->kind!=FL_ASSET_LUA) p->selected=first;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCRIPTS});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_PROPERTY_ROW});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_FA_PLUS " New", .id = "script-new", .on_click = new_clicked, .click_data = p,
                               .disabled = !project});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_FA_FILE_IMPORT " Import…", .id = "script-import", .on_click = import_clicked, .click_data = p,
                                .disabled = !project});
    ca_div_end();
    p->status = p->message[0] ? ca_text(&(Ca_TextDesc){.text = p->message, .style = FL_CLASS_MUTED, .wrap = true}) : NULL;
    if (!count) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_EMPTY});
        ca_text(&(Ca_TextDesc){.text = CA_ICON_NF_FA_CODE, .style = FL_CLASS_EMPTY_ICON});
        ca_text(&(Ca_TextDesc){.text = "No scripts in this project", .style = FL_CLASS_MUTED});
        ca_div_end();
    } else {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCRIPT_LIST});
        for (size_t i=0;i<total;++i) {const Fl_Asset *a=fl_project_asset_at(project,i); if (a->kind==FL_ASSET_LUA) emit_row(p,a);}
        ca_div_end();
        const Fl_Asset *selected = fl_project_asset(project, p->selected);
        if (selected) emit_detail(p, active_scene(p), selected);
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
    if (request->scene != fl_scene_edit_scene(p->edit) || !fl_project_asset(fl_scene_edit_project(p->edit), request->script)) return false;
    p->selected = request->script;
    if (p->dock) {ca_dock_open_panel(p->dock, PANEL_ID); ca_dock_focus_panel(p->dock, PANEL_ID);}
    invalidate(p);
    return false;
}

Fl_ScriptPanel *fl_script_panel_create(Fl_SceneEdit *edit, Fl_EventBus *bus)
{
    if (!edit || !bus) return NULL;
    Fl_ScriptPanel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = edit; p->bus = bus;
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
