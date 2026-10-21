#include "fl_render_host.h"
#include "fl_mcp_service.h"
#include "doc/fl_json.h"
#include "time/fl_composition.h"
#include "render/gpu/fl_exporter.h"
#include "render/gpu/fl_scene_renderer.h"
#include "agent/fl_agent.h"
#include "agent/fl_mcp.h"
#include "agent/fl_mcp_http.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_random.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "mcp"
#define PREFERRED_PORT 7788u
#define TOKEN_BYTES 32u

/* A unit of work posted from a network thread to the UI thread. */
typedef struct Fl_McpTask { void (*run)(void *data); void *data; } Fl_McpTask;

struct Fl_McpService {
    Fl_EventBus *bus;
    Ca_Instance *instance;
    Fl_EventType task_event;
    Fl_Mcp *mcp;
    Fl_McpHttp *http;
    Fl_Agent *agent;
    Fl_SceneRenderer *renderer;
    Fl_Exporter *exporter;
    Fl_Project *project;
    Ca_Window *window;
    Fl_Workspace *workspace;
    char token[TOKEN_BYTES * 2 + 1];
    char config_path[FL_PATH_MAX];
};

/* ---------- plumbing ---------- */

/* UI-thread delivery of a posted task. */
static bool run_task(const Fl_Event *event, void *data)
{
    (void)data;
    const Fl_McpTask *task = event->payload;
    task->run(task->data);
    return true;
}

/* Post from any thread; the bus wakes the UI loop. */
static bool post_task(void (*run)(void *), void *data, void *user)
{
    Fl_McpService *s = user;
    Fl_McpTask task = {run, data};
    return fl_event_post(s->bus, s->task_event, &task, sizeof(task));
}

static Fl_Project *current_project(void *user) { return ((Fl_McpService *)user)->project; }

/* Offscreen GPU render for the agent's render_frame tool. */
static bool render_scene(void *user, const Fl_Project *project, const Fl_Scene *scene, Fl_Tick tick, uint32_t width, Fl_Frame *out)
{
    return fl_scene_renderer_render(((Fl_McpService *)user)->renderer, project, scene, tick, width, out);
}

static void hex(const unsigned char *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {out[i * 2] = digits[bytes[i] >> 4]; out[i * 2 + 1] = digits[bytes[i] & 15];}
    out[n * 2] = 0;
}

/* Write mcp.json readable by the user only. */
static bool write_config(const Fl_McpService *s, unsigned port)
{
    cJSON *config = cJSON_CreateObject();
    if (!config) return false;
    cJSON_AddNumberToObject(config, "port", port);
    cJSON_AddStringToObject(config, "token", s->token);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/mcp", port);
    cJSON_AddStringToObject(config, "url", url);
    char *text = cJSON_Print(config);
    cJSON_Delete(config);
    bool ok = text && fl_fs_write_atomic(s->config_path, text, strlen(text)) && fl_fs_make_private(s->config_path);
    free(text);
    return ok;
}

/* Load the saved port and token, or mint a new token. */
static bool load_config(Fl_McpService *s, unsigned *port)
{
    char dir[FL_PATH_MAX];
    if (!fl_paths_user_dir(FL_PATH_CONFIG, dir, sizeof(dir)) || !fl_fs_mkdir_p(dir) || !fl_fs_join(s->config_path, sizeof(s->config_path), dir, "mcp.json")) return false;
    *port = PREFERRED_PORT;
    size_t size = 0;
    char *text = fl_fs_read_file(s->config_path, 4096, &size);
    cJSON *config = text ? cJSON_ParseWithLength(text, size) : NULL;
    free(text);
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(config, "token"), *saved = cJSON_GetObjectItemCaseSensitive(config, "port");
    bool valid = cJSON_IsString(token) && strlen(token->valuestring) == TOKEN_BYTES * 2;
    if (valid) memcpy(s->token, token->valuestring, sizeof(s->token));
    if (valid && cJSON_IsNumber(saved) && saved->valuedouble >= 1024 && saved->valuedouble <= 65535) *port = (unsigned)saved->valuedouble;
    cJSON_Delete(config);
    if (valid) return true;
    unsigned char random[TOKEN_BYTES];
    if (!fl_random_bytes(random, sizeof(random))) return false;
    hex(random, sizeof(random), s->token);
    return true;
}

/* ---------- editor tools ---------- */

/* Read an optional or required numeric argument; completes the call with an error and returns false on a bad value. */
static bool number_arg(Fl_McpCall *call, const cJSON *args, const char *key, bool required, double *out, bool *present)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);
    if (present) *present = false;
    if (!item || cJSON_IsNull(item)) {
        if (required) fl_mcp_return_error(call, "missing_field", "\"%s\" is required", key);
        return !required;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble != item->valuedouble || item->valuedouble > 1e12 || item->valuedouble < -1e12) {
        fl_mcp_return_error(call, "invalid_argument", "\"%s\" must be a finite number", key);
        return false;
    }
    *out = item->valuedouble;
    if (present) *present = true;
    return true;
}

/* Reject keys outside the tool's schema, as the document tools do. */
static bool only_keys(Fl_McpCall *call, const cJSON *args, const char *const *allowed)
{
    for (const cJSON *item = args ? args->child : NULL; item; item = item->next) {
        bool known = false;
        for (size_t i = 0; allowed[i]; ++i) known |= !strcmp(allowed[i], item->string);
        if (!known) {fl_mcp_return_error(call, "unknown_field", "no argument named \"%s\"", item->string); return false;}
    }
    return true;
}

static Fl_SceneEdit *edit_of(const Fl_McpService *s) { return s->workspace ? fl_workspace_edit(s->workspace) : NULL; }

/* Report that the editor UI is not available, e.g. while the project launcher is showing. */
static bool require_editor(Fl_McpService *s, Fl_McpCall *call)
{
    if (s->project && s->window && edit_of(s)) return true;
    fl_mcp_return_error(call, "no_project", "No project is open in Flow");
    return false;
}

/* Export tools: start, observe and cancel a video render. */
static cJSON *export_json(const Fl_ExportStatus *status)
{
    static const char *const states[] = {"idle", "running", "done", "failed", "cancelled"};
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "state", states[status->state]);
    cJSON_AddNumberToObject(o, "frames_done", (double)status->frames_done);
    cJSON_AddNumberToObject(o, "frames_total", (double)status->frames_total);
    if (status->frames_total) cJSON_AddNumberToObject(o, "progress", (double)status->frames_done / (double)status->frames_total);
    cJSON_AddNumberToObject(o, "width", status->width); cJSON_AddNumberToObject(o, "height", status->height); cJSON_AddNumberToObject(o, "fps", status->fps);
    cJSON_AddNumberToObject(o, "elapsed_seconds", status->elapsed_seconds);
    cJSON_AddStringToObject(o, "path", status->path);
    if (status->message[0]) cJSON_AddStringToObject(o, "message", status->message);
    return o;
}

static void tool_export_video(Fl_McpCall *call, const cJSON *args, void *data)
{
    static const char *const keys[] = {"path", "width", "fps", "crf", "scenes", "crossfade", NULL};
    Fl_McpService *s = data;
    if (!only_keys(call, args, keys) || !require_editor(s, call)) return;
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path"), *scenes = cJSON_GetObjectItemCaseSensitive(args, "scenes");
    double width = 1920, fps = 60, crf = 0, crossfade = 0;
    if (!cJSON_IsString(path) || !path->valuestring[0]) {fl_mcp_return_error(call, "missing_field", "\"path\" is required (a file name goes to the project's exports folder)"); return;}
    if (!number_arg(call, args, "width", false, &width, NULL) || !number_arg(call, args, "fps", false, &fps, NULL) || !number_arg(call, args, "crf", false, &crf, NULL) ||
        !number_arg(call, args, "crossfade", false, &crossfade, NULL)) return;
    if (width < 2 || width > 16384 || width != floor(width) || fps<1 || fps>120 || fps!=floor(fps) || crf<0 || crf>51 || crf!=floor(crf)) {fl_mcp_return_error(call, "invalid_argument", "width, fps and crf are whole numbers"); return;}
    Fl_Id ids[256];
    size_t count = 0;
    if (scenes) {
        if (!cJSON_IsArray(scenes) || !cJSON_GetArraySize(scenes) || cJSON_GetArraySize(scenes) > 256) {fl_mcp_return_error(call, "invalid_argument", "\"scenes\" is 1..256 scene ids"); return;}
        for (const cJSON *id = scenes->child; id; id = id->next) {
            Fl_Json context={0};
            if (!fl_json_uint(&context,id,&ids[count]) || !ids[count]) {fl_mcp_return_error(call,"invalid_argument","Scene IDs must be exact nonzero integers or decimal strings"); return;}
            ++count;
        }
    }
    char full[FL_PATH_MAX], error[256];
    const char *p = path->valuestring;
    bool absolute = p[0] == '/' || p[0] == '\\' || (p[1] == ':' && (p[2] == '/' || p[2] == '\\'));
    char folder[FL_PATH_MAX];
    if (absolute) snprintf(full, sizeof(full), "%s", p);
    else if (!fl_fs_join(folder, sizeof(folder), fl_project_directory(s->project), "exports") || !fl_fs_mkdir_p(folder) || !fl_fs_join(full, sizeof(full), folder, p)) {
        fl_mcp_return_error(call, "invalid_argument", "that path is too long"); return;
    }
    if (!fl_exporter_start(s->exporter, s->project, &(Fl_ExportDesc){.path = full, .width = (uint32_t)width, .fps = (uint32_t)fps, .crf = (uint32_t)crf,
                                                                     .scenes = scenes ? ids : NULL, .scene_count = count, .crossfade = crossfade}, error, sizeof(error))) {
        fl_mcp_return_error(call, "export_failed", "%s", error); return;
    }
    Fl_ExportStatus status = fl_exporter_status(s->exporter);
    fl_mcp_return_json(call, export_json(&status));
}

static void tool_get_export_status(Fl_McpCall *call, const cJSON *args, void *data)
{
    static const char *const keys[] = {NULL};
    Fl_McpService *s = data;
    if (!only_keys(call, args, keys)) return;
    Fl_ExportStatus status = fl_exporter_status(s->exporter);
    fl_mcp_return_json(call, export_json(&status));
}

static void tool_cancel_export(Fl_McpCall *call, const cJSON *args, void *data)
{
    static const char *const keys[] = {NULL};
    Fl_McpService *s = data;
    if (!only_keys(call, args, keys)) return;
    fl_exporter_cancel(s->exporter);
    Fl_ExportStatus status = fl_exporter_status(s->exporter);
    fl_mcp_return_json(call, export_json(&status));
}


static void tool_get_editor_state(Fl_McpCall *call, const cJSON *args, void *data)
{
    static const char *const keys[] = {NULL};
    Fl_McpService *s = data;
    if (!only_keys(call, args, keys) || !require_editor(s, call)) return;
    Fl_SceneEdit *edit = edit_of(s);
    const Fl_Scene *scene = fl_project_scene(s->project, fl_scene_edit_scene(edit));
    cJSON *o = cJSON_CreateObject(), *selected = o ? cJSON_AddArrayToObject(o, "selected") : NULL;
    if (!selected) {cJSON_Delete(o); fl_mcp_return_error(call, "internal", "out of memory"); return;}
    cJSON_AddItemToObject(o,"scene",fl_json_u64(fl_scene_edit_scene(edit)));
    cJSON_AddNumberToObject(o, "time", (double)fl_scene_edit_time(edit) / FL_TICKS_PER_SECOND);
    cJSON_AddBoolToObject(o, "playing", fl_playback_playing(fl_workspace_playback(s->workspace)));
    Fl_Id focus=fl_scene_edit_focus(edit),owner=fl_scene_edit_primary(edit);
    if (focus && focus!=owner) {cJSON_AddItemToArray(selected,fl_json_u64(focus)); cJSON_AddItemToObject(o,"selection_owner",fl_json_u64(owner));}
    else for (size_t i=0;scene && i<scene->entity_count;++i) if (fl_scene_edit_contains(edit,scene->entities[i].id)) cJSON_AddItemToArray(selected,fl_json_u64(scene->entities[i].id));
    fl_mcp_return_json(call, o);
}

static void tool_set_editor_state(Fl_McpCall *call, const cJSON *args, void *data)
{
    static const char *const keys[] = {"scene", "time", "select", NULL};
    Fl_McpService *s = data;
    if (!only_keys(call, args, keys) || !require_editor(s, call)) return;
    Fl_SceneEdit *edit = edit_of(s);
    Fl_Json context={0}; Fl_Id scene_id=fl_scene_edit_scene(edit); double seconds=0; bool has_time=false;
    const cJSON *scene_value=cJSON_GetObjectItemCaseSensitive(args,"scene"), *select=cJSON_GetObjectItemCaseSensitive(args,"select");
    if (scene_value && !fl_json_uint(&context,scene_value,&scene_id)) {fl_mcp_return_error(call,"invalid_argument","Scene must be an exact ID"); return;}
    if (!number_arg(call,args,"time",false,&seconds,&has_time)) return;
    Fl_Tick tick=0;
    if (has_time && !fl_time_from_seconds(seconds,&tick)) {fl_mcp_return_error(call,"invalid_argument","Time exceeds native tick range"); return;}
    if (select && (!cJSON_IsArray(select) || cJSON_GetArraySize(select)>100000)) {fl_mcp_return_error(call,"invalid_argument","select must be a bounded array of exact entity IDs"); return;}
    const Fl_Scene *scene=fl_project_scene(s->project,scene_id);
    if (!scene) {fl_mcp_return_error(call,"not_found","Scene is missing"); return;}
    Fl_SceneState candidate={0}; const Fl_Scene *flat=scene;
    for (const cJSON *item=select ? select->child : NULL;item;item=item->next) {
        Fl_Id id;
        if (!fl_json_uint(&context,item,&id) || !id) {fl_scene_state_clear(&candidate); fl_mcp_return_error(call,"invalid_argument","Selection requires exact nonzero IDs"); return;}
        if (!fl_scene_entity(scene,id) && !candidate.valid) {
            if (!fl_scene_state_update(&candidate,scene,has_time ? tick : fl_scene_edit_time(edit))) {fl_scene_state_clear(&candidate); fl_mcp_return_error(call,"evaluation_failed","Cannot resolve generated selection"); return;}
            flat=fl_scene_state_scene(&candidate,scene);
        }
        if (!fl_scene_entity(flat,id) || (!fl_scene_entity(scene,id) && cJSON_GetArraySize(select)!=1)) {fl_scene_state_clear(&candidate); fl_mcp_return_error(call,"not_found","Generated selection requires one valid displayed object"); return;}
    }
    fl_scene_state_clear(&candidate);
    if (scene_value || select) fl_scene_edit_select(edit,scene->id,0,false);
    if (has_time) fl_scene_edit_set_time(edit,tick);
    for (const cJSON *item=select ? select->child : NULL;item;item=item->next) {
        Fl_Id id=0; fl_json_uint(&context,item,&id);
        if (!fl_scene_edit_select(edit,scene->id,id,true)) {fl_mcp_return_error(call,"evaluation_failed","Cannot publish the requested selection"); return;}
    }
    tool_get_editor_state(call, NULL, data);
}

/* ---------- lifecycle ---------- */

void fl_mcp_service_detach(Fl_McpService *s)
{
    if (!s) return;
    fl_exporter_abort(s->exporter, "the project was closed during the export");
    s->project = NULL; s->window = NULL; s->workspace = NULL;
}

void fl_mcp_service_attach(Fl_McpService *s, Fl_Project *project, Ca_Window *window, Fl_Workspace *workspace)
{
    if (!s) return;
    fl_mcp_service_detach(s);
    s->project = project; s->window = window; s->workspace = workspace;
}

unsigned fl_mcp_service_port(const Fl_McpService *s) { return s ? fl_mcp_http_port(s->http) : 0; }

void fl_mcp_service_destroy(Fl_McpService *s)
{
    if (!s) return;
    fl_mcp_shutdown(s->mcp);
    fl_mcp_http_stop(s->http);
    fl_event_unsubscribe_owner(s->bus, s);
    fl_mcp_service_detach(s);
    fl_mcp_destroy(s->mcp);
    fl_agent_destroy(s->agent);
    fl_exporter_destroy(s->exporter);
    fl_scene_renderer_destroy(s->renderer);
    free(s);
}

/* Register the tools that need the editor window. */
static bool register_editor_tools(Fl_McpService *s)
{
    static const char *const empty = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
    const Fl_McpTool tools[] = {
        {"export_video", "Render the project's scenes in order to an H.264 MP4 with ffmpeg. path may be a file name (written to the project's exports folder) or an absolute path. "
         "width defaults to 1920 (height follows the aspect; both must be even), fps to 60, crf 0..51 (default 16, lower is better). scenes optionally lists scene ids. crossfade (seconds, default 0) dissolves each scene into the next by overlapping them. "
         "It runs in the background: poll get_export_status.",
         "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"width\":{\"type\":\"integer\"},\"fps\":{\"type\":\"integer\"},\"crf\":{\"type\":\"integer\"},\"crossfade\":{\"type\":\"number\"},\"scenes\":{\"type\":\"array\",\"items\":{\"type\":[\"integer\",\"string\"]}}},\"required\":[\"path\"],\"additionalProperties\":false}",
         false, tool_export_video, s},
        {"get_export_status", "Progress of the latest video export: state, frames done and total, elapsed time and output path.", empty, true, tool_get_export_status, s},
        {"cancel_export", "Stop the running video export.", empty, false, tool_cancel_export, s},
        {"get_editor_state", "The scene, time and selection currently shown in the editor.", empty, true, tool_get_editor_state, s},
        {"set_editor_state", "Show a scene, move the playhead (seconds) or select entities in the editor so the user can follow along.",
         "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":[\"integer\",\"string\"]},\"time\":{\"type\":\"number\"},\"select\":{\"type\":\"array\",\"items\":{\"type\":[\"integer\",\"string\"]}}},\"additionalProperties\":false}",
         false, tool_set_editor_state, s},
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(*tools); ++i) if (!fl_mcp_register(s->mcp, &tools[i])) return false;
    return true;
}

Fl_McpService *fl_mcp_service_create(Fl_EventBus *bus, Ca_Instance *instance, Fl_Fonts *fonts, Fl_JobSystem *jobs)
{
    if (!bus || !instance || !fonts || !jobs) return NULL;
    Fl_McpService *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->bus = bus; s->instance = instance;
    s->task_event = fl_event_type_register(bus, "mcp.task", sizeof(Fl_McpTask));
    unsigned port = PREFERRED_PORT;
    s->renderer = fl_scene_renderer_create(&(Fl_SceneRendererDesc){.gpu=fl_render_host_context(instance), .fonts = fonts, .jobs = jobs});
    s->exporter = s->renderer ? fl_exporter_create(bus, s->renderer) : NULL;
    s->agent = fl_agent_create(&(Fl_AgentHost){current_project, render_scene, s});
    s->mcp = fl_mcp_create(&(Fl_McpDesc){"flow", FLOW_VERSION, fl_agent_instructions(), post_task, s, 120000});
    bool ok = s->task_event != FL_EVENT_TYPE_INVALID && s->renderer && s->exporter && s->agent && s->mcp && load_config(s, &port) &&
        fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = s->task_event, .fn = run_task, .user_data = s, .owner = s}).generation &&
        fl_agent_register(s->agent, s->mcp) && register_editor_tools(s);
    if (ok) {
        s->http = fl_mcp_http_start(&(Fl_McpHttpDesc){s->mcp, (uint16_t)port, s->token});
        if (!s->http) s->http = fl_mcp_http_start(&(Fl_McpHttpDesc){s->mcp, 0, s->token});
        ok = s->http != NULL;
    }
    if (!ok) {
        FL_LOG_WARN(LOG_TAG, "MCP endpoint unavailable; agents cannot connect");
        fl_mcp_service_destroy(s);
        return NULL;
    }
    if (!write_config(s, fl_mcp_http_port(s->http))) FL_LOG_WARN(LOG_TAG, "could not save %s; the bearer token changes on restart", s->config_path);
    else FL_LOG_INFO(LOG_TAG, "agent connection details: %s", s->config_path);
    return s;
}
