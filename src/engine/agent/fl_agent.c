#include "fl_agent_internal.h"
#include "components/fl_effect.h"
#include "time/fl_time.h"
#include "time/fl_composition.h"
#include "doc/fl_json.h"
#include "schema/fl_properties.h"
#include "core/fl_log.h"
#include "media/fl_png.h"
#include "platform/fl_fs.h"
#include "project/fl_asset_import.h"
#include <math.h>
#include <stdarg.h>
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
           "Lua scripting API. Build with apply_ops (atomic, undoable batches), then verify with review_scene. "
           "render_frame returns one small contact sheet and is for a procedural background, not every edit. "
           "evaluate_scene sparse:true reads numbers. Times are in seconds, rotations in degrees. "
           "The manuscript is the spoken script: read it with get_manuscript and edit it with phase.add/update/remove/move and voice.set.";
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
    fl_bake_destroy(a->bake);
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

static cJSON *tool_get_manuscript(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"scene", NULL};
    Fl_Fail fail; Fl_Project *p;
    if (!agent_only_keys(args, &fail, "get_manuscript", keys)) {*error = fl_fail_json(&fail); return NULL;}
    const Fl_Scene *s = scene_arg(a, args, &p, &fail, error);
    return s ? agent_manuscript_json(s) : NULL;
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
    static const char *const keys[] = {"scene", "time", "sparse", "entities", "since", NULL};
    Fl_Fail fail; Fl_Project *p; Fl_Tick tick = 0;
    bool sparse = false, since = false;
    if (!agent_only_keys(args, &fail, "evaluate_scene", keys) || !agent_get_seconds(args, "time", false, &tick, NULL, &fail, "evaluate_scene") ||
        !agent_get_bool(args, "sparse", &sparse, NULL, &fail, "evaluate_scene") || !agent_get_bool(args, "since", &since, NULL, &fail, "evaluate_scene")) {*error = fl_fail_json(&fail); return NULL;}
    const cJSON *only = cJSON_GetObjectItemCaseSensitive(args, "entities");
    if (only && !cJSON_IsArray(only)) {fl_fail(&fail, "invalid_argument", "evaluate_scene: entities must be an array of ids"); *error = fl_fail_json(&fail); return NULL;}
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
    if (o && since && !a->touched_count) cJSON_AddStringToObject(o, "note", "the last apply_ops did not name any entities");
    for (size_t i = 0; list && i < s->entity_count; ++i) {
        const Fl_Entity *e = &s->entities[i];
        const Fl_EntityState *v = &state.entities[i];
        if (since) {
            size_t n = 0;
            for (; n < a->touched_count; ++n) if (a->touched[n] == e->id) break;
            if (n == a->touched_count) continue;
        }
        if (only) {
            bool match = false;
            for (const cJSON *idj = only->child; idj && !match; idj = idj->next) {
                Fl_Id id = 0; Fl_Fail ignore = {0};
                match = agent_id_value(idj, NULL, &id, &ignore, "evaluate_scene") && id == e->id;
            }
            if (!match) continue;
        }
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
            cJSON *components=cJSON_AddArrayToObject(provenance,"components");
            for (size_t j=0;components && j<e->component_count;++j) {
                Fl_Id local;
                if (!fl_composition_component_local(state.composition,i,e->components[j].id,&local)) continue;
                cJSON *address=cJSON_CreateObject();
                if (!address) break;
                cJSON_AddItemToObject(address,"runtime",agent_id_json(e->components[j].id)); cJSON_AddItemToObject(address,"local",agent_id_json(local)); cJSON_AddItemToArray(components,address);
            }
        }
        cJSON *writers=cJSON_AddObjectToObject(item,"writers");
        for (int property=0;writers && property<=FL_PROP_COUNT;++property) {
            Fl_Id component; int32_t layer;
            if (!fl_scene_state_writer(&state,i,property,&component,&layer)) continue;
            cJSON *writer=cJSON_AddObjectToObject(writers,property==FL_PROP_COUNT ? "text" : fl_property_name((Fl_AnimProperty)property));
            if (writer) {cJSON_AddItemToObject(writer,"component",agent_id_json(component)); cJSON_AddNumberToObject(writer,"layer",layer);}
        }
        cJSON_AddNumberToObject(item, "x", v->pose.x); cJSON_AddNumberToObject(item, "y", v->pose.y); cJSON_AddNumberToObject(item, "z", v->pose.z);
        cJSON_AddNumberToObject(item, "width", v->bounds.width); cJSON_AddNumberToObject(item, "height", v->bounds.height); cJSON_AddNumberToObject(item, "depth", v->bounds.depth);
        cJSON_AddNumberToObject(item, "rotation", v->pose.rz * 180.0 / 3.14159265358979323846);
        cJSON_AddNumberToObject(item, "rx", v->pose.rx * 180.0 / 3.14159265358979323846); cJSON_AddNumberToObject(item, "ry", v->pose.ry * 180.0 / 3.14159265358979323846);
        cJSON_AddNumberToObject(item, "scale_x", v->pose.sx); cJSON_AddNumberToObject(item, "scale_y", v->pose.sy); cJSON_AddNumberToObject(item, "scale_z", v->pose.sz);
        cJSON_AddNumberToObject(item, "opacity", v->opacity);
        if (!sparse) {
            double values[FL_PROP_COUNT][4]; fl_entity_state_values(v,values);
            cJSON *properties=cJSON_AddObjectToObject(item,"properties");
            for (int property=0;properties && property<FL_PROP_COUNT;++property) {
                const Fl_PropertyDef *definition=fl_property_definition((Fl_AnimProperty)property);
                cJSON_AddItemToObject(properties,definition->name,definition->channels==1 ? cJSON_CreateNumber(values[property][0]) : cJSON_CreateDoubleArray(values[property],(int)definition->channels));
            }
        }
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
    size_t size = 1 + (size_t)FL_PROP_COUNT * 320;
    for (size_t i = 0; FL_AGENT_REFERENCE[i]; ++i) size += strlen(FL_AGENT_REFERENCE[i]);
    char *text = malloc(size), *w = text, *end = text ? text + size : NULL;
    if (!text) return NULL;
    for (size_t i = 0; FL_AGENT_REFERENCE[i]; ++i) {size_t n = strlen(FL_AGENT_REFERENCE[i]); memcpy(w, FL_AGENT_REFERENCE[i], n); w += n;}
    int wrote = snprintf(w, (size_t)(end - w), "\nChannels. key.set property and entity.set use the name. Lua writes group.field. Ranges are inclusive.\n");
    if (wrote > 0 && w + wrote < end) w += wrote;
    for (int property = 0; property < FL_PROP_COUNT; ++property) {
        const Fl_PropertyDef *definition = fl_property_definition((Fl_AnimProperty)property);
        if (!definition || w + 1 >= end) continue;
        wrote = snprintf(w, (size_t)(end - w), "- %s (%s.%s) %s, %u channel%s, %.4g..%.4g\n", definition->name, definition->lua_group ? definition->lua_group : "", definition->lua_field ? definition->lua_field : "", definition->label ? definition->label : "", definition->channels, definition->channels == 1 ? "" : "s", definition->minimum, definition->maximum);
        if (wrote > 0 && w + wrote < end) w += wrote;
    }
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

#define SHEET_TIMES 4u

static void issue_add(char *buf, size_t cap, const char *fmt, ...)
{
    size_t n = strlen(buf);
    if (n && n + 1 < cap) buf[n++] = '\n';
    if (n >= cap) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
}

static void screen_box(const Fl_EntityState *v, double *l, double *t, double *r, double *b)
{
    double sx = fabs(v->pose.sx) > 1e-6 ? fabs(v->pose.sx) : 1, sy = fabs(v->pose.sy) > 1e-6 ? fabs(v->pose.sy) : 1;
    *l = v->pose.x - v->pose.px * sx; *t = v->pose.y - v->pose.py * sy;
    *r = *l + v->bounds.width * sx; *b = *t + v->bounds.height * sy;
}

static uint64_t fnv_pixels(const unsigned char *p, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) {h ^= p[i]; h *= 1099511628211ull;}
    return h;
}

static cJSON *tool_review_scene(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"scene", "time", NULL};
    Fl_Fail fail; Fl_Project *p; Fl_Tick tick = 0;
    if (!agent_only_keys(args, &fail, "review_scene", keys) || !agent_get_seconds(args, "time", false, &tick, NULL, &fail, "review_scene")) {*error = fl_fail_json(&fail); return NULL;}
    const Fl_Scene *authored = scene_arg(a, args, &p, &fail, error);
    if (!authored) return NULL;
    if (tick < 0 || tick > fl_scene_length(authored)) {fl_fail(&fail, "invalid_argument", "time must be within 0..%g seconds", agent_ticks_to_seconds(fl_scene_length(authored))); *error = fl_fail_json(&fail); return NULL;}
    Fl_SceneState state = {0};
    if (!fl_scene_state_update(&state, authored, tick)) {fl_fail(&fail, "evaluation_failed", "%s", state.error[0] ? state.error : "Cannot evaluate scene"); fl_scene_state_clear(&state); *error = fl_fail_json(&fail); return NULL;}
    const Fl_Scene *s = fl_scene_state_scene(&state, authored);
    char issues[4096] = "";
    if (state.error[0]) issue_add(issues, sizeof issues, "script: %s", state.error);
    double canvas_w = (double)s->width, canvas_h = (double)s->height;
    int moving = 0;
    Fl_Tick later_tick = tick + (Fl_Tick)(FL_TICKS_PER_SECOND / 10);
    Fl_SceneState later = {0};
    bool have_later = later_tick <= fl_scene_length(authored) && fl_scene_state_update(&later, authored, later_tick);
    const Fl_Scene *later_scene = have_later ? fl_scene_state_scene(&later, authored) : NULL;
    for (size_t i = 0; i < s->entity_count; ++i) {
        const Fl_Entity *e = &s->entities[i];
        const Fl_EntityState *v = &state.entities[i];
        const Fl_WidgetDef *def = fl_widget_for_entity(e);
        const char *script_error = fl_scene_state_entity_error(&state, i);
        if (script_error && script_error[0]) issue_add(issues, sizeof issues, "%s: %s", e->name, script_error);
        if (!e->visible || v->opacity < 0.02 || !def || def->kind == FL_WIDGET_ENTITY) continue;
        if (v->has_path && v->trim_start >= v->trim_end) issue_add(issues, sizeof issues, "%s: path trim_start is not before trim_end", e->name);
        if (v->has_effect && v->effect_kind == FL_EFFECT_CUSTOM && !v->effect_source_size) issue_add(issues, sizeof issues, "%s: custom effect has no GLSL source", e->name);
        double l, t, r, b; screen_box(v, &l, &t, &r, &b);
        bool text = def->has_text;
        if (r < 0 || b < 0 || l > canvas_w || t > canvas_h) issue_add(issues, sizeof issues, "%s: entirely off the canvas", e->name);
        else if (text && (l < 8 || t < 8 || r > canvas_w - 8 || b > canvas_h - 8)) issue_add(issues, sizeof issues, "%s: text sits on the canvas edge", e->name);
        if (text && v->appearance.font_size * fabs(v->pose.sy) < 18) issue_add(issues, sizeof issues, "%s: type is under 18px", e->name);
        if (have_later && i < later.count && later_scene && i < later_scene->entity_count) {
            const Fl_EntityState *n = &later.entities[i];
            if (fabs(n->pose.x - v->pose.x) > 0.5 || fabs(n->pose.y - v->pose.y) > 0.5 || fabs(n->pose.sx - v->pose.sx) > 0.01 || fabs(n->pose.rz - v->pose.rz) > 0.01 || fabs(n->opacity - v->opacity) > 0.02) ++moving;
        }
        for (size_t j = i + 1; text && j < s->entity_count; ++j) {
            const Fl_WidgetDef *other = fl_widget_for_entity(&s->entities[j]);
            if (!other || !other->has_text || !s->entities[j].visible || state.entities[j].opacity < 0.02) continue;
            double l2, t2, r2, b2; screen_box(&state.entities[j], &l2, &t2, &r2, &b2);
            if (l < r2 && r > l2 && t < b2 && b > t2) issue_add(issues, sizeof issues, "%s overlaps %s", e->name, s->entities[j].name);
        }
    }
    if (moving >= 4) issue_add(issues, sizeof issues, "%d entities change together over the next tenth of a second", moving);
    Fl_Frame sample = {0};
    if (canvas_w > 1 && canvas_h > 1 && a->host.render && a->host.render(a->host.user_data, p, authored, tick, 160, &sample) && sample.rgba && sample.width && sample.height) {
        for (size_t i = 0; i < s->entity_count; ++i) {
            const Fl_WidgetDef *def = fl_widget_for_entity(&s->entities[i]);
            const Fl_EntityState *v = &state.entities[i];
            if (!def || !def->has_text || !s->entities[i].visible || v->opacity < 0.2 || !v->appearance.fill[3]) continue;
            double l, t, r, b; screen_box(v, &l, &t, &r, &b);
            int x = (int)(((l + r) * 0.5) / canvas_w * sample.width), y = (int)(((t + b) * 0.5) / canvas_h * sample.height);
            if (x < 2 || y < 2 || x >= (int)sample.width - 2 || y >= (int)sample.height - 2) continue;
            const unsigned char *px = sample.rgba + ((size_t)y * sample.width + (size_t)x) * 4;
            double back = 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
            double ink = 0.2126 * v->appearance.fill[0] + 0.7152 * v->appearance.fill[1] + 0.0722 * v->appearance.fill[2];
            if (fabs(ink - back) < 40) issue_add(issues, sizeof issues, "%s: text contrast against the pixels behind it is low", s->entities[i].name);
        }
    }
    fl_frame_free(&sample);
    fl_scene_state_clear(&later);
    fl_scene_state_clear(&state);
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddItemToObject(o, "scene", agent_id_json(authored->id));
    cJSON_AddNumberToObject(o, "time", agent_ticks_to_seconds(tick));
    bool changed = !a->review[0] || strcmp(a->review, issues) != 0;
    cJSON_AddBoolToObject(o, "changed", changed);
    snprintf(a->review, sizeof a->review, "%s", issues);
    cJSON *list = cJSON_AddArrayToObject(o, "issues");
    if (issues[0]) {
        char *copy = strdup(issues);
        for (char *line = copy; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            cJSON_AddItemToArray(list, cJSON_CreateString(line));
            line = nl ? nl + 1 : NULL;
        }
        free(copy);
    }
    return o;
}

static cJSON *tool_render_frame(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"scene", "time", "times", "width", NULL};
    Fl_Fail fail; Fl_Project *p; double width = 480, seconds[SHEET_TIMES] = {0}, number;
    size_t count = 1; bool present = false, width_set = false;
    if (!agent_only_keys(args, &fail, "render_frame", keys) || !agent_get_number(args, "width", false, &width, &width_set, &fail, "render_frame") ||
        !agent_get_number(args, "time", false, &number, &present, &fail, "render_frame")) {*error = fl_fail_json(&fail); return NULL;}
    if (!width_set) width = 480;
    if (width > 720) width = 720;
    if (present) seconds[0] = number;
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(args, "times");
    if (times) {
        if (present || !cJSON_IsArray(times) || !cJSON_GetArraySize(times) || (unsigned)cJSON_GetArraySize(times) > SHEET_TIMES) {
            fl_fail(&fail, "invalid_argument", "render_frame: give either \"time\" or \"times\" (1..%u numbers)", SHEET_TIMES);
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
        fl_fail(&fail, "invalid_argument", "render_frame: width must be a whole number from 16 up to 720");
        *error = fl_fail_json(&fail); return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    drop_frames(a);
    unsigned char *strip = calloc((size_t)w * count * h, 4);
    if (!o || !strip) {free(strip); cJSON_Delete(o); fl_fail(&fail, "internal", "out of memory"); *error = fl_fail_json(&fail); return NULL;}
    for (size_t i = 0; i < count; ++i) {
        Fl_Tick tick;
        if (!agent_seconds_to_ticks(seconds[i], &tick) || tick < 0 || tick > fl_scene_length(s)) {
            fl_fail(&fail, "invalid_argument", "render_frame: time must be within 0..%g seconds", agent_ticks_to_seconds(fl_scene_length(s)));
            *error = fl_fail_json(&fail); cJSON_Delete(o); free(strip); drop_frames(a); return NULL;
        }
        Fl_Frame frame;
        if (!a->host.render(a->host.user_data, p, s, tick, w, &frame)) {fl_fail(&fail, "internal", "the frame could not be rendered"); *error = fl_fail_json(&fail); cJSON_Delete(o); free(strip); drop_frames(a); return NULL;}
        uint32_t copy_w = frame.width < w ? frame.width : w, copy_h = frame.height < h ? frame.height : h;
        for (uint32_t y = 0; y < copy_h; ++y) memcpy(strip + ((size_t)y * w * count + i * w) * 4, frame.rgba + (size_t)y * frame.width * 4, (size_t)copy_w * 4);
        fl_frame_free(&frame);
    }
    uint64_t hash = fnv_pixels(strip, (size_t)w * count * h * 4);
    if (hash == a->sheet_hash && a->sheet_scene == s->id) {
        free(strip);
        cJSON_AddBoolToObject(o, "unchanged", true);
        cJSON_AddItemToObject(o, "scene", agent_id_json(s->id));
        return o;
    }
    size_t size = 0;
    unsigned char *png = fl_png_encode_region(strip, w * (uint32_t)count, h, 0, 0, w * (uint32_t)count, h, 0, &size, NULL, NULL);
    free(strip);
    if (!png) {fl_fail(&fail, "internal", "the frame could not be encoded"); *error = fl_fail_json(&fail); cJSON_Delete(o); return NULL;}
    a->sheet_hash = hash; a->sheet_scene = s->id;
    a->frames.png[0] = png; a->frames.size[0] = size; a->frames.count = 1;
    cJSON_AddBoolToObject(o, "unchanged", false);
    cJSON_AddItemToObject(o, "scene", agent_id_json(s->id));
    cJSON_AddNumberToObject(o, "width", w * (double)count); cJSON_AddNumberToObject(o, "height", h); cJSON_AddNumberToObject(o, "times", (double)count);
    return o;
}

#define NO_ARGS "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
/* Start, inspect, cancel or apply the shared native bake job without blocking the editor owner thread. */
static cJSON *tool_bake_scene(Fl_Agent *a, const cJSON *args, cJSON **error)
{
    static const char *const keys[]={"action","scene","start","end","step","tolerance","token",NULL};
    Fl_Fail fail={0}; const char *action=NULL; Fl_Project *project=project_or_fail(a,error);
    if (!project) return NULL;
    if (!agent_only_keys(args,&fail,"bake_scene",keys) || !agent_get_string(args,"action",true,&action,&fail,"bake_scene")) goto failed;
    if (!strcmp(action,"start")) {
        Fl_Id scene=0; Fl_BakeOptions options={0};
        if (!agent_get_id(args,"scene",NULL,true,&scene,&fail,"bake_scene") ||
            !agent_get_seconds(args,"start",true,&options.start,NULL,&fail,"bake_scene") ||
            !agent_get_seconds(args,"end",true,&options.end,NULL,&fail,"bake_scene") ||
            !agent_get_seconds(args,"step",true,&options.step,NULL,&fail,"bake_scene") ||
            !agent_get_number(args,"tolerance",true,&options.tolerance,NULL,&fail,"bake_scene")) goto failed;
        if (!a->bake) a->bake=fl_bake_create(NULL);
        if (a->bake_token==UINT64_MAX || !fl_bake_start(a->bake,project,scene,&options)) {fl_fail(&fail,"rejected","Bake cannot start: invalid interval, active edit or running job"); goto failed;}
        ++a->bake_token;
    } else {
        Fl_Id token=0;
        if (!agent_get_id(args,"token",NULL,true,&token,&fail,"bake_scene")) goto failed;
        if (!a->bake || token!=a->bake_token) {fl_fail(&fail,"stale_job","Unknown bake token"); goto failed;}
        if (!strcmp(action,"cancel")) fl_bake_cancel(a->bake);
        else if (!strcmp(action,"apply")) {
            if (!fl_bake_apply(a->bake,project,NULL)) {fl_fail(&fail,"rejected","Bake is not ready or the project changed since capture"); goto failed;}
        } else if (strcmp(action,"status")) {fl_fail(&fail,"invalid_argument","Unknown bake action"); goto failed;}
    }
    cJSON *result=cJSON_CreateObject(); static const char *const statuses[]={"idle","running","ready","failed","cancelled","applied"};
    cJSON_AddItemToObject(result,"token",agent_id_json(a->bake_token));
    cJSON_AddStringToObject(result,"status",statuses[fl_bake_status(a->bake)]);
    cJSON_AddStringToObject(result,"diagnostic",fl_bake_error(a->bake)); return result;
failed:
    *error=fl_fail_json(&fail); return NULL;
}

static const ToolDef TOOLS[] = {
    {"bake_scene","Capture Lua and native animation into editable native sampled data. Start with explicit seconds interval, maximum step and numeric tolerance; poll token, then apply atomically. Nested scope clocks/media are preserved; changed projects reject stale output.","{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"status\",\"cancel\",\"apply\"]},\"scene\":{},\"token\":{},\"start\":{\"type\":\"number\"},\"end\":{\"type\":\"number\"},\"step\":{\"type\":\"number\"},\"tolerance\":{\"type\":\"number\"}},\"required\":[\"action\"],\"additionalProperties\":false}",false,tool_bake_scene},
    {"get_reference", "Manual for authoring in Flow: units, operation vocabulary, animation model and the Lua scripting API. Read this first.",
     NO_ARGS, true, tool_get_reference},
    {"get_project", "List the project's scenes (id, revision, size, length), assets and undo history.", NO_ARGS, true, tool_get_project},
    {"get_scene", "Full contents of one scene: canvas, beats, audio clips, the manuscript (spoken phases, voice and clone reference), every entity with its properties, keyframe tracks and script info. "
     "Entities are listed in draw order (later draws on top).",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"include_scripts\":{\"type\":\"boolean\",\"description\":\"Include Lua source text\"}},\"required\":[\"scene\"],\"additionalProperties\":false}",
     true, tool_get_scene},
    {"get_manuscript", "The spoken script of one scene: voice, language, clone reference, transcript, total words and minutes, and each phase with its text, beat, clip and reading time. About 150 words is one minute.",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]}},\"required\":[\"scene\"],\"additionalProperties\":false}",
     true, tool_get_manuscript},
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
    {"review_scene", "Text check after a batch of edits. Reports off-frame boxes, type on the edge or under 18px, overlapping text, low text contrast, path trim, a missing custom effect, simultaneous motion and script errors. Does not return an image. Call this instead of render_frame.",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"time\":{\"type\":\"number\",\"description\":\"Seconds from scene start; default 0\"}},\"required\":[\"scene\"],\"additionalProperties\":false}",
     true, tool_review_scene},
    {"evaluate_scene", "Numbers for a scene at a time (seconds). sparse:true omits the full channel dump. since:true keeps entities the last apply_ops touched. entities limits the list to those ids.",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"time\":{\"type\":\"number\"},\"sparse\":{\"type\":\"boolean\"},\"since\":{\"type\":\"boolean\"},\"entities\":{\"type\":\"array\"}},\"required\":[\"scene\"],\"additionalProperties\":false}",
     true, tool_evaluate_scene},
    {"apply_ops", "Apply a batch of document operations atomically (all succeed or none do) as one undoable changeset. "
     "Operations: scene.add/remove/rename/move/set, entity.create/set/remove/reparent/reorder, key.set/remove, track.remove, beat.add/update/remove/move/split, "
     "clip.add/update/remove, phase.add/update/remove/move, voice.set, prototype.capture/instantiate/overrides/detach, scope.edit, asset.source/rename/remove, script.create/update/remove/attach/detach. Name created objects with \"as\":\"$name\" and reuse \"$name\" as an id later in the batch. "
     "Pass expect:{\"<scene id>\": revision} to refuse the batch if a scene changed since you read it. Full field lists are in get_reference.",
     "{\"type\":\"object\",\"properties\":{\"ops\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"},\"as\":{\"type\":\"string\"}},\"required\":[\"op\"]}},"
     "\"label\":{\"type\":\"string\",\"description\":\"Short name shown for undo\"},\"expect\":{\"type\":\"object\",\"additionalProperties\":{\"type\":\"integer\"}},\"dry_run\":{\"type\":\"boolean\"}},\"required\":[\"ops\"],\"additionalProperties\":false}",
     false, agent_tool_apply_ops},
    {"render_frame", "One contact sheet for a procedural background, packed left to right. Default width 480, capped at 720. times holds at most 4 moments. The same pixels return {unchanged:true} and no image. Prefer review_scene after a batch.",
     "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"time\":{\"type\":\"number\"},\"times\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"maxItems\":4},\"width\":{\"type\":\"integer\"}},\"required\":[\"scene\"],\"additionalProperties\":false}",
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
