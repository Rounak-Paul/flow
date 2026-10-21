#include "fl_agent_internal.h"
#include "time/fl_composition.h"
#include "doc/fl_json.h"
#include "core/fl_log.h"
#include "media/fl_png.h"
#include "platform/fl_fs.h"
#include "project/fl_asset_import.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef cJSON *(*ToolFn)(Fl_Agent *agent, const cJSON *args, cJSON **error);

typedef struct ToolDef {
    const char *name, *description, *schema;
    bool read_only;
    ToolFn run;
} ToolDef;

const char *fl_agent_instructions(void)
{
    return "Flow is a motion-graphics and video editor running on the user's machine; you edit the project that is open "
           "in it, and the user sees your changes live. Call get_reference first for units, the operation vocabulary and the "
           "Lua scripting API. Build with apply_ops (atomic, undoable batches), then verify with render_frame and "
           "evaluate_scene. Times are in seconds, rotations in degrees.";
}

Fl_Agent *fl_agent_create(const Fl_AgentHost *host)
{
    if (!host || !host->project) return NULL;
    Fl_Agent *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->host = *host;
    return a;
}

/* Free frames a tool left for the transport. */
static void drop_frames(Fl_Agent *a)
{
    for (size_t i = 0; i < a->frames.count; ++i) free(a->frames.png[i]);
    a->frames.count = 0;
}

void fl_agent_destroy(Fl_Agent *a)
{
    if (!a) return;
    drop_frames(a);
    free(a->bindings); free(a);
}


static Fl_Project *project_or_fail(Fl_Agent *a, cJSON **error)
{
    Fl_Project *p = a->host.project(a->host.user_data);
    if (!p) {
        Fl_Fail fail; fl_fail(&fail, "no_project", "No project is open in Flow");
        *error = fl_fail_json(&fail);
    }
    return p;
}

static cJSON *history_json(const Fl_History *h)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "undo_available", (double)fl_history_undo_count(h));
    cJSON_AddNumberToObject(o, "redo_available", (double)fl_history_redo_count(h));
    if (fl_history_undo_label(h)) cJSON_AddStringToObject(o, "next_undo", fl_history_undo_label(h));
    if (fl_history_redo_label(h)) cJSON_AddStringToObject(o, "next_redo", fl_history_redo_label(h));
    return o;
}

static cJSON *tool_get_project(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {NULL};
    Fl_Fail fail;
    Fl_Project *p = project_or_fail(a, error);
    if (!p) return NULL;
    if (!agent_only_keys(args, &fail, "get_project", keys)) {*error = fl_fail_json(&fail); return NULL;}
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "name", fl_project_name(p));
    cJSON_AddBoolToObject(o, "unsaved_changes", fl_project_dirty(p));
    cJSON *scenes = cJSON_AddArrayToObject(o, "scenes"), *assets = cJSON_AddArrayToObject(o, "assets");
    for (size_t i = 0; scenes && i < fl_project_scene_count(p); ++i) cJSON_AddItemToArray(scenes, agent_scene_summary(fl_project_scene_at(p, i)));
    for (size_t i = 0; assets && i < fl_project_asset_count(p); ++i) {
        const Fl_Asset *asset = fl_project_asset_at(p, i);
        cJSON_AddItemToArray(assets, agent_asset_json(asset, fl_project_asset_uses(p, asset->id)));
    }
    cJSON_AddItemToObject(o, "history", history_json(fl_project_history(p)));
    return o;
}

/* Resolve the `scene` argument shared by scene-scoped tools. */
static const Fl_Scene *scene_arg(Fl_Agent *a, const cJSON *args, Fl_Project **project, Fl_Fail *fail, cJSON **error)
{
    *project = project_or_fail(a, error);
    if (!*project) return NULL;
    Fl_Id id;
    if (!agent_get_id(args, "scene", NULL, true, &id, fail, "scene")) {*error = fl_fail_json(fail); return NULL;}
    const Fl_Scene *s = fl_project_scene(*project, id);
    if (!s) {fl_fail(fail, "not_found", "No scene with id %llu", (unsigned long long)id); *error = fl_fail_json(fail);}
    return s;
}

/* Read accepted asset metadata and current source bytes, including unsaved source edits. */
static cJSON *tool_get_asset(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[]={"asset",NULL};
    Fl_Project *project=project_or_fail(a,error); Fl_Fail fail; Fl_Id id=0;
    if (!project) return NULL;
    if (!agent_only_keys(args,&fail,"get_asset",keys) || !agent_get_id(args,"asset",NULL,true,&id,&fail,"get_asset")) {*error=fl_fail_json(&fail); return NULL;}
    const Fl_Asset *asset=fl_project_asset(project,id);
    if (!asset) {fl_fail(&fail,"not_found","get_asset: no asset with id %llu",(unsigned long long)id); *error=fl_fail_json(&fail); return NULL;}
    cJSON *out=cJSON_CreateObject(), *value=agent_asset_json(asset,fl_project_asset_uses(project,id));
    Fl_Json context={0};
    if (!fl_json_add(&context,out,"asset",value)) {cJSON_Delete(out); return NULL;}
    if (asset->source) fl_json_add(&context,value,"source",cJSON_CreateString(asset->source));
    if (context.error[0]) {cJSON_Delete(out); return NULL;}
    return out;
}

static cJSON *tool_get_scene(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"scene", "include_scripts", NULL};
    Fl_Fail fail; Fl_Project *p; bool sources = false;
    if (!agent_only_keys(args, &fail, "get_scene", keys) || !agent_get_bool(args, "include_scripts", &sources, NULL, &fail, "get_scene")) {*error = fl_fail_json(&fail); return NULL;}
    const Fl_Scene *s = scene_arg(a, args, &p, &fail, error);
    return s ? agent_scene_json(p, s, sources) : NULL;
}

/* Resolve a `script` argument given as an id or an exact name; NULL when it names nothing. */
static const Fl_Asset *script_arg(const Fl_Project *p, const cJSON *item)
{
    if (cJSON_IsString(item)) {
        const Fl_Asset *named = fl_project_asset_named(p, FL_ASSET_LUA, item->valuestring);
        if (named) return named;
    }
    Fl_Id id; Fl_Fail fail;
    if (!agent_id_value(item,NULL,&id,&fail,"script")) return NULL;
    const Fl_Asset *asset = fl_project_asset(p, id);
    return asset && asset->kind == FL_ASSET_LUA ? asset : NULL;
}

static cJSON *tool_get_script(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"script", NULL};
    Fl_Fail fail; Fl_Project *p;
    if (!agent_only_keys(args, &fail, "get_script", keys)) {*error = fl_fail_json(&fail); return NULL;}
    p = project_or_fail(a, error);
    if (!p) return NULL;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "script");
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    if (item) {
        const Fl_Asset *script = script_arg(p, item);
        if (!script) {cJSON_Delete(out); fl_fail(&fail, "not_found", "get_script: the project has no Lua asset %s", cJSON_IsString(item) ? item->valuestring : "with that id"); *error = fl_fail_json(&fail); return NULL;}
        cJSON_AddItemToObject(out, "script", agent_script_json(p, script, true));
        return out;
    }
    cJSON *list = cJSON_AddArrayToObject(out, "scripts");
    for (size_t i=0; list && i<fl_project_asset_count(p); ++i) {
        const Fl_Asset *asset=fl_project_asset_at(p,i);
        if (asset->kind==FL_ASSET_LUA) cJSON_AddItemToArray(list,agent_script_json(p,asset,false));
    }
    return out;
}

static cJSON *tool_set_script(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"script", "name", "source", "edits", "attach", "enabled", NULL};
    Fl_Fail fail; Fl_Project *p;
    if (!agent_only_keys(args, &fail, "set_script", keys)) {*error = fl_fail_json(&fail); return NULL;}
    p = project_or_fail(a, error);
    if (!p) return NULL;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "script"), *name = cJSON_GetObjectItemCaseSensitive(args, "name"),
                *source = cJSON_GetObjectItemCaseSensitive(args, "source"), *edits = cJSON_GetObjectItemCaseSensitive(args, "edits"),
                *attach = cJSON_GetObjectItemCaseSensitive(args, "attach"), *enabled = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    if ((attach && !cJSON_IsArray(attach)) || (enabled && !cJSON_IsBool(enabled))) {
        fl_fail(&fail,"invalid_argument","attach must be an array and enabled must be boolean"); *error=fl_fail_json(&fail); return NULL;
    }
    const Fl_Asset *script = script_arg(p, item);
    if (item && !script && !(cJSON_IsString(item) && source)) {
        fl_fail(&fail, "not_found", "set_script: the project has no Lua asset %s", cJSON_IsString(item) ? item->valuestring : "with that id"); *error = fl_fail_json(&fail); return NULL;
    }
    cJSON *ops=cJSON_CreateArray(), *write=cJSON_CreateObject(), *request=cJSON_CreateObject();
    Fl_Json context={0};
    if (!ops || !write || !request) goto memory;
    if (!cJSON_AddItemToArray(ops,write)) goto memory;
    cJSON *operation=write; write=NULL;
    if (script) {
        fl_json_add(&context,operation,"op",cJSON_CreateString("script.update"));
        fl_json_add(&context,operation,"script",agent_id_json(script->id));
        if (name) fl_json_add(&context,operation,"name",cJSON_Duplicate(name,1));
        if (source) fl_json_add(&context,operation,"source",cJSON_Duplicate(source,1));
        if (edits) fl_json_add(&context,operation,"edits",cJSON_Duplicate(edits,1));
    } else {
        fl_json_add(&context,operation,"op",cJSON_CreateString("script.create"));
        fl_json_add(&context,operation,"as",cJSON_CreateString("$script"));
        if (name || item) fl_json_add(&context,operation,"name",cJSON_Duplicate(name ? name : item,1));
        if (source) fl_json_add(&context,operation,"source",cJSON_Duplicate(source,1));
    }
    if (context.error[0]) goto memory;
    for (const cJSON *entity=attach ? attach->child : NULL;entity;entity=entity->next) {
        cJSON *op=cJSON_CreateObject();
        if (!fl_json_add(&context,ops,NULL,op)) goto memory;
        fl_json_add(&context,op,"op",cJSON_CreateString("script.attach"));
        fl_json_add(&context,op,"entity",cJSON_Duplicate(entity,1));
        fl_json_add(&context,op,"script",script ? agent_id_json(script->id) : cJSON_CreateString("$script"));
        if (enabled) fl_json_add(&context,op,"enabled",cJSON_Duplicate(enabled,1));
        if (context.error[0]) goto memory;
    }
    fl_json_add(&context,request,"ops",ops); ops=NULL;
    fl_json_add(&context,request,"label",cJSON_CreateString("Edit script"));
    if (context.error[0]) goto memory;
    cJSON *result=agent_tool_apply_ops(a,request,error);
    cJSON_Delete(request); return result;
memory:
    cJSON_Delete(ops); cJSON_Delete(write); cJSON_Delete(request);
    fl_fail(&fail, "internal", "out of memory"); *error = fl_fail_json(&fail);
    return NULL;
}

static cJSON *tool_evaluate_scene(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"scene", "time", NULL};
    Fl_Fail fail; Fl_Project *p; Fl_Tick tick = 0;
    if (!agent_only_keys(args, &fail, "evaluate_scene", keys) || !agent_get_seconds(args, "time", false, &tick, NULL, &fail, "evaluate_scene")) {*error = fl_fail_json(&fail); return NULL;}
    const Fl_Scene *s = scene_arg(a, args, &p, &fail, error);
    if (!s) return NULL;
    if (tick < 0 || tick > fl_scene_length(s)) {
        fl_fail(&fail, "invalid_argument", "time must be within 0..%g seconds", agent_ticks_to_seconds(fl_scene_length(s)));
        *error = fl_fail_json(&fail); return NULL;
    }
    Fl_SceneState state = {0};
    if (!fl_scene_state_update(&state, s, tick)) {fl_fail(&fail,"evaluation_failed","%s",state.error[0] ? state.error : "Cannot evaluate scene"); fl_scene_state_clear(&state); *error=fl_fail_json(&fail); return NULL;}
    s=fl_scene_state_scene(&state,s);
    cJSON *o = cJSON_CreateObject(), *list = o ? cJSON_AddArrayToObject(o, "entities") : NULL;
    if (o) {cJSON_AddItemToObject(o, "scene", agent_id_json(s->id)); cJSON_AddNumberToObject(o, "time", agent_ticks_to_seconds(tick));}
    if (o && state.error[0]) cJSON_AddStringToObject(o,"diagnostic",state.error);
    if (o) cJSON_AddNumberToObject(o,"unresolved_overrides",(double)fl_composition_unresolved(state.composition));
    for (size_t i = 0; list && i < s->entity_count; ++i) {
        const Fl_Entity *e = &s->entities[i];
        const Fl_EntityState *v = &state.entities[i];
        const Fl_WidgetDef *def = fl_widget_for_entity(e);
        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddItemToObject(item, "id", agent_id_json(e->id));
        cJSON_AddStringToObject(item, "name", e->name);
        const Fl_Origin *origin=fl_composition_origin(state.composition,i);
        if (origin && origin->generated) {
            cJSON *provenance=cJSON_AddObjectToObject(item,"origin"), *path=cJSON_AddArrayToObject(provenance,"path");
            cJSON_AddItemToObject(provenance,"owner",agent_id_json(origin->owner)); cJSON_AddItemToObject(provenance,"component",agent_id_json(origin->component)); cJSON_AddItemToObject(provenance,"entity",agent_id_json(origin->entity));
            for (size_t j=0;j<origin->depth;++j) cJSON_AddItemToArray(path,agent_id_json(origin->path[j]));
        }
        cJSON_AddNumberToObject(item, "x", v->transform.x); cJSON_AddNumberToObject(item, "y", v->transform.y);
        cJSON_AddNumberToObject(item, "width", v->transform.width); cJSON_AddNumberToObject(item, "height", v->transform.height);
        cJSON_AddNumberToObject(item, "rotation", v->transform.rotation * 180.0 / 3.14159265358979323846);
        cJSON_AddNumberToObject(item, "scale_x", v->transform.scale_x); cJSON_AddNumberToObject(item, "scale_y", v->transform.scale_y);
        cJSON_AddNumberToObject(item, "opacity", v->opacity);
        if (def && def->has_fill) agent_color_json(item, "fill", v->appearance.fill);
        if (def && def->has_stroke) {agent_color_json(item, "stroke", v->appearance.stroke); cJSON_AddNumberToObject(item, "stroke_width", v->appearance.stroke_width);}
        if (def && def->has_text) {
            cJSON_AddNumberToObject(item, "font_size", v->appearance.font_size);
            const Fl_Component *text = fl_entity_component(e, FL_COMPONENT_TEXT);
            cJSON *content = v->text_override ? cJSON_CreateString(v->text) : NULL;
            if (!v->text_override && text) {
                char *copy = malloc(text->size + 1);
                if (copy) {memcpy(copy, text->data, text->size); copy[text->size] = 0; content = cJSON_CreateString(copy); free(copy);}
            }
            if (content) cJSON_AddItemToObject(item, "text", content);
        }
        if (fl_entity_component(e, FL_COMPONENT_IMAGE) || fl_entity_component(e, FL_COMPONENT_VIDEO)) {
            cJSON *crop = cJSON_AddArrayToObject(item, "crop");
            for (size_t c = 0; crop && c < 4; ++c) cJSON_AddItemToArray(crop,cJSON_CreateNumber(v->crop[c]));
        }
        const char *script_error = fl_scene_state_entity_error(&state,i);
        if (script_error && script_error[0]) cJSON_AddStringToObject(item, "script_error", script_error);
        cJSON_AddItemToArray(list, item);
    }
    fl_scene_state_clear(&state);
    return o;
}

static cJSON *tool_get_reference(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    (void)a; (void)args; (void)error;
    size_t size = 1;
    for (size_t i = 0; FL_AGENT_REFERENCE[i]; ++i) size += strlen(FL_AGENT_REFERENCE[i]);
    char *text = malloc(size), *w = text;
    if (!text) return NULL;
    for (size_t i = 0; FL_AGENT_REFERENCE[i]; ++i) {size_t n = strlen(FL_AGENT_REFERENCE[i]); memcpy(w, FL_AGENT_REFERENCE[i], n); w += n;}
    *w = 0;
    cJSON *o = cJSON_CreateObject();
    if (o) cJSON_AddStringToObject(o, "reference", text);
    free(text);
    return o;
}

/* True for POSIX absolute paths, drive-letter paths and UNC paths. */
static bool absolute_path(const char *path)
{
    return path[0] == '/' || (path[0] && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) || (path[0] == '\\' && path[1] == '\\');
}

static cJSON *tool_import_asset(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"path", "name", NULL};
    Fl_Fail fail; const char *path, *name = NULL;
    Fl_Project *p = project_or_fail(a, error);
    if (!p) return NULL;
    if (!agent_only_keys(args, &fail, "import_asset", keys) || !agent_get_string(args, "path", true, &path, &fail, "import_asset") ||
        !agent_get_string(args, "name", false, &name, &fail, "import_asset")) {*error = fl_fail_json(&fail); return NULL;}
    if (!absolute_path(path)) {fl_fail(&fail, "invalid_argument", "import_asset: path must be absolute"); *error = fl_fail_json(&fail); return NULL;}
    Fl_StoredAsset stored;
    Fl_ImportStatus status = fl_asset_store_file(fl_project_directory(p), path, &stored);
    if (status == FL_IMPORT_UNSUPPORTED || status == FL_IMPORT_FAILED) {
        fl_fail(&fail, status == FL_IMPORT_UNSUPPORTED ? "unsupported" : "failed", status == FL_IMPORT_UNSUPPORTED ?
                "Flow cannot import that file: supported are Lua, Flow prototype, image, video, audio and font files" : "The file could not be read or stored");
        *error = fl_fail_json(&fail); return NULL;
    }
    char display[256];
    const char *base=path;
    for (const char *part=path;*part;++part) if (*part=='/' || *part=='\\') base=part+1;
    snprintf(display,sizeof(display),"%s",name && name[0] ? name : base);
    char *dot=name ? NULL : strrchr(display,'.'); if (dot && dot!=display) *dot=0;
    Fl_Id id=0;
    status=fl_asset_record(p,&stored,display,&id);
    if (status!=FL_IMPORT_ADDED && status!=FL_IMPORT_EXISTING) {
        char store[FL_PATH_MAX];
        if (stored.created && !fl_project_asset_by_hash(p,stored.hash) && fl_asset_store_path(fl_project_directory(p),stored.hash,stored.extension,store,sizeof(store))) fl_fs_remove(store);
        fl_fail(&fail,"rejected","The project refused the asset"); *error=fl_fail_json(&fail); return NULL;
    }
    const Fl_Asset *asset=fl_project_asset(p,id); bool added=status==FL_IMPORT_ADDED;
    cJSON *o = agent_asset_json(asset, fl_project_asset_uses(p, asset->id));
    if (o) cJSON_AddBoolToObject(o, "already_in_project", !added);
    return o;
}

/* Undo or redo the newest changeset. */
static cJSON *history_step(Fl_Agent *a, bool undo, cJSON **error)
{
    Fl_Fail fail; char label[FL_HISTORY_LABEL_MAX]; bool conflict;
    Fl_Project *p = project_or_fail(a, error);
    if (!p) return NULL;
    bool ok = undo ? fl_history_undo(fl_project_history(p), p, label, &conflict) : fl_history_redo(fl_project_history(p), p, label, &conflict);
    if (!ok) {
        if (conflict) fl_fail(&fail, "conflict", "The project was edited after that changeset, so %s would overwrite those edits. Re-read the project and make the change you want directly.", undo ? "undoing it" : "redoing it");
        else fl_fail(&fail, "nothing_to_do", "Nothing to %s", undo ? "undo" : "redo");
        *error = fl_fail_json(&fail); return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, undo ? "undone" : "redone", label);
    cJSON_AddItemToObject(o, "history", history_json(fl_project_history(p)));
    cJSON *scenes = cJSON_AddArrayToObject(o, "scenes");
    for (size_t i = 0; scenes && i < fl_project_scene_count(p); ++i) cJSON_AddItemToArray(scenes, agent_scene_summary(fl_project_scene_at(p, i)));
    return o;
}
static cJSON *tool_undo(Fl_Agent *a, const cJSON *args, cJSON **error) {(void)args; return history_step(a, true, error);}
static cJSON *tool_redo(Fl_Agent *a, const cJSON *args, cJSON **error) {(void)args; return history_step(a, false, error);}

static cJSON *tool_save_project(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    (void)args;
    Fl_Project *p = project_or_fail(a, error);
    if (!p) return NULL;
    if (!fl_project_save(p)) {
        Fl_Fail fail; fl_fail(&fail, "failed", "Flow could not write the project to disk"); *error = fl_fail_json(&fail); return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    if (o) cJSON_AddBoolToObject(o, "saved", true);
    return o;
}

static cJSON *tool_render_frame(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"scene", "time", "times", "width", NULL};
    Fl_Fail fail; Fl_Project *p; double width = 1024, seconds[FL_AGENT_MAX_FRAMES] = {0}, number;
    size_t count = 1; bool present;
    if (!agent_only_keys(args, &fail, "render_frame", keys) || !agent_get_number(args, "width", false, &width, NULL, &fail, "render_frame") ||
        !agent_get_number(args, "time", false, &number, &present, &fail, "render_frame")) {*error = fl_fail_json(&fail); return NULL;}
    if (present) seconds[0] = number;
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(args, "times");
    if (times) {
        if (present || !cJSON_IsArray(times) || !cJSON_GetArraySize(times) || (unsigned)cJSON_GetArraySize(times) > FL_AGENT_MAX_FRAMES) {
            fl_fail(&fail, "invalid_argument", "render_frame: give either \"time\" or \"times\" (1..%u numbers)", FL_AGENT_MAX_FRAMES);
            *error = fl_fail_json(&fail); return NULL;
        }
        count = 0;
        for (const cJSON *t = times->child; t; t = t->next) {
            if (!cJSON_IsNumber(t) || !isfinite(t->valuedouble)) {fl_fail(&fail, "invalid_argument", "render_frame: times must be numbers"); *error = fl_fail_json(&fail); return NULL;}
            seconds[count++] = t->valuedouble;
        }
    }
    const Fl_Scene *s = scene_arg(a, args, &p, &fail, error);
    if (!s) return NULL;
    if (!a->host.render) {fl_fail(&fail, "unavailable", "Rendering is not available in this build"); *error = fl_fail_json(&fail); return NULL;}
    uint32_t w, h;
    if (width < 16 || width != floor(width) || !fl_frame_dimensions(s, (uint32_t)width, &w, &h)) {
        fl_fail(&fail, "invalid_argument", "render_frame: width must be a whole number from 16 up to a frame of at most %u pixels", FL_RENDER_MAX_PIXELS);
        *error = fl_fail_json(&fail); return NULL;
    }
    cJSON *o = cJSON_CreateObject(), *list = o ? cJSON_AddArrayToObject(o, "frames") : NULL;
    drop_frames(a);
    for (size_t i = 0; list && i < count; ++i) {
        Fl_Tick tick;
        if (!agent_seconds_to_ticks(seconds[i], &tick) || tick < 0 || tick > fl_scene_length(s)) {
            fl_fail(&fail, "invalid_argument", "render_frame: time must be within 0..%g seconds", agent_ticks_to_seconds(fl_scene_length(s)));
            *error = fl_fail_json(&fail); cJSON_Delete(o); drop_frames(a); return NULL;
        }
        Fl_Frame frame;
        if (!a->host.render(a->host.user_data, p, s, tick, w, &frame)) {fl_fail(&fail, "internal", "the frame could not be rendered"); *error = fl_fail_json(&fail); cJSON_Delete(o); drop_frames(a); return NULL;}
        size_t size = 0;
        unsigned char *png = fl_png_encode_region(frame.rgba, frame.width, frame.height, 0, 0, frame.width, frame.height, 0, &size, NULL, NULL);
        fl_frame_free(&frame);
        cJSON *item = png ? cJSON_CreateObject() : NULL;
        if (!item) {free(png); fl_fail(&fail, "internal", "the frame could not be encoded"); *error = fl_fail_json(&fail); cJSON_Delete(o); drop_frames(a); return NULL;}
        a->frames.png[a->frames.count] = png; a->frames.size[a->frames.count++] = size;
        cJSON_AddNumberToObject(item, "time", agent_ticks_to_seconds(tick));
        cJSON_AddNumberToObject(item, "width", w); cJSON_AddNumberToObject(item, "height", h); cJSON_AddNumberToObject(item, "bytes", (double)size);
        cJSON_AddItemToArray(list, item);
    }
    if (o) cJSON_AddItemToObject(o, "scene", agent_id_json(s->id));
    return o;
}

#define NO_ARGS "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
static const ToolDef TOOLS[] = {
    {"get_reference", "Manual for authoring in Flow: units, operation vocabulary, animation model and the Lua scripting API. Read this first.",
     NO_ARGS, true, tool_get_reference},
    {"get_project", "List the project's scenes (id, revision, size, length), assets and undo history.", NO_ARGS, true, tool_get_project},
    {"get_scene", "Full contents of one scene: canvas, beats, audio clips, every entity with its properties, keyframe tracks and script info. "
     "Entities are listed in draw order (later draws on top).",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"include_scripts\":{\"type\":\"boolean\",\"description\":\"Include Lua source text\"}},\"required\":[\"scene\"],\"additionalProperties\":false}",
     true, tool_get_scene},
    {"get_asset", "Read an asset's metadata and current accepted source bytes, including unsaved Lua or Flow prototype edits.",
     "{\"type\":\"object\",\"properties\":{\"asset\":{\"type\":[\"integer\",\"string\"]}},\"required\":[\"asset\"],\"additionalProperties\":false}", true, tool_get_asset},
    {"get_script", "Read the project's Lua assets. Without script it lists every script (id, name, size, entities running it); "
     "with script (an id or the exact name) it returns the Lua source too.",
     "{\"type\":\"object\",\"properties\":{\"script\":{\"type\":[\"integer\",\"string\"]}},\"additionalProperties\":false}",
     true, tool_get_script},
    {"set_script", "Create or edit a Lua asset in one undoable step. script is an id or exact name: if it exists it is updated, "
     "if it is a new name (with source) it is created. Give source to replace the text, or edits [{old,new}] to patch it (each old must match exactly once). "
     "name renames. attach lists entity ids that should run it (enabled defaults to true). Invalid Lua syntax, attached declarations or parameter values reject the changeset.",
     "{\"type\":\"object\",\"properties\":{\"script\":{\"type\":[\"integer\",\"string\"]},\"name\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},"
     "\"edits\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}},\"required\":[\"old\",\"new\"]}},"
     "\"attach\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},\"enabled\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
     false, tool_set_script},
    {"evaluate_scene", "Evaluate every entity of a scene at a time (seconds): resolved position, size, colours and opacity after keyframes and Lua scripts, plus script errors.",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"time\":{\"type\":\"number\",\"description\":\"Seconds from scene start; default 0\"}},\"required\":[\"scene\"],\"additionalProperties\":false}",
     true, tool_evaluate_scene},
    {"apply_ops", "Apply a batch of document operations atomically (all succeed or none do) as one undoable changeset. "
     "Operations: scene.add/remove/rename/move/set, entity.create/set/remove/reparent/reorder, key.set/remove, track.remove, beat.add/update/remove/move/split, "
     "clip.add/update/remove, prototype.capture/instantiate/overrides, asset.source/rename/remove, script.create/update/remove/attach/detach. Name created objects with \"as\":\"$name\" and reuse \"$name\" as an id later in the batch. "
     "Pass expect:{\"<scene id>\": revision} to refuse the batch if a scene changed since you read it. Full field lists are in get_reference.",
     "{\"type\":\"object\",\"properties\":{\"ops\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"},\"as\":{\"type\":\"string\"}},\"required\":[\"op\"]}},"
     "\"label\":{\"type\":\"string\",\"description\":\"Short name shown for undo\"},\"expect\":{\"type\":\"object\",\"additionalProperties\":{\"type\":\"integer\"}},\"dry_run\":{\"type\":\"boolean\"}},\"required\":[\"ops\"],\"additionalProperties\":false}",
     false, agent_tool_apply_ops},
    {"render_frame", "Render a scene at one or more times (seconds) offline and return PNG images, exactly as the project would look in the video: no editor grid or selection, any resolution, independent of what the editor is showing. "
     "Use it to check your work. Give time or times (up to 6, for comparing moments). width is the image width in pixels (default 1024, at most a frame of 8.8 megapixels); height follows the scene's aspect.",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"time\":{\"type\":\"number\"},\"times\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"maxItems\":6},\"width\":{\"type\":\"integer\"}},\"required\":[\"scene\"],\"additionalProperties\":false}",
     true, tool_render_frame},
    {"import_asset", "Import a local Lua, Flow prototype, image, audio, video or font file (absolute path) as an undoable asset edit.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}", false, tool_import_asset},
    {"undo", "Undo the newest apply_ops changeset. Refused if the project was edited since.", NO_ARGS, false, tool_undo},
    {"redo", "Redo the changeset most recently undone.", NO_ARGS, false, tool_redo},
    {"save_project", "Write the project to disk now. Flow also autosaves shortly after every change.", NO_ARGS, false, tool_save_project},
};

cJSON *fl_agent_call(Fl_Agent *a, const char *tool, const cJSON *arguments, cJSON **error)
{
    cJSON *empty = NULL;
    *error = NULL;
    if (!arguments) arguments = empty = cJSON_CreateObject();
    cJSON *result = NULL;
    bool found = false;
    for (size_t i = 0; a && tool && i < sizeof(TOOLS) / sizeof(*TOOLS); ++i) if (!strcmp(TOOLS[i].name, tool)) {
        found = true; result = TOOLS[i].run(a, arguments, error); break;
    }
    cJSON_Delete(empty);
    if (a) drop_frames(a);
    if (!found || (!result && !*error)) {
        Fl_Fail fail;
        if (!found) fl_fail(&fail, "unknown_tool", "No tool named \"%s\"", tool ? tool : ""); else fl_fail(&fail, "internal", "%s failed", tool);
        *error = fl_fail_json(&fail);
    }
    return result;
}

/* One registered tool and the agent it serves. */
typedef struct Binding { Fl_Agent *agent; const ToolDef *def; } Binding;

/* MCP binding: run the synchronous tool on the owner thread and complete the call. */
static void run_tool(Fl_McpCall *call, const cJSON *arguments, void *data)
{
    const Binding *binding = data;
    cJSON *error = NULL;
    Fl_Agent *agent = binding->agent;
    cJSON *result = binding->def->run(agent, arguments, &error);
    if (result && agent->frames.count) {
        Fl_McpImage images[FL_AGENT_MAX_FRAMES];
        for (size_t i = 0; i < agent->frames.count; ++i) images[i] = (Fl_McpImage){agent->frames.png[i], agent->frames.size[i], "image/png"};
        fl_mcp_return_images(call, result, images, agent->frames.count);
        drop_frames(agent);
    }
    else if (result) fl_mcp_return_json(call, result);
    else if (error) fl_mcp_return_error_object(call, error);
    else fl_mcp_return_error(call, "internal", "%s failed", binding->def->name);
}

bool fl_agent_register(Fl_Agent *a, Fl_Mcp *mcp)
{
    if (!a || !mcp || a->bindings) return false;
    size_t count = sizeof(TOOLS) / sizeof(*TOOLS);
    Binding *bindings = calloc(count, sizeof(*bindings));
    if (!bindings) return false;
    a->bindings = bindings;
    for (size_t i = 0; i < count; ++i) {
        bindings[i] = (Binding){a, &TOOLS[i]};
        if (!fl_mcp_register(mcp, &(Fl_McpTool){TOOLS[i].name, TOOLS[i].description, TOOLS[i].schema, TOOLS[i].read_only, run_tool, &bindings[i]})) return false;
    }
    return true;
}
