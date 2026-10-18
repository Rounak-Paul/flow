#include "fl_mcp_service.h"
#include "fl_scene_renderer.h"
#include "agent/fl_agent.h"
#include "agent/fl_mcp.h"
#include "agent/fl_mcp_http.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_random.h"
#include <stdio.h>
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

static void tool_get_editor_state(Fl_McpCall *call, const cJSON *args, void *data)
{
    static const char *const keys[] = {NULL};
    Fl_McpService *s = data;
    if (!only_keys(call, args, keys) || !require_editor(s, call)) return;
    Fl_SceneEdit *edit = edit_of(s);
    const Fl_Scene *scene = fl_project_scene(s->project, fl_scene_edit_scene(edit));
    cJSON *o = cJSON_CreateObject(), *selected = o ? cJSON_AddArrayToObject(o, "selected") : NULL;
    if (!selected) {cJSON_Delete(o); fl_mcp_return_error(call, "internal", "out of memory"); return;}
    cJSON_AddNumberToObject(o, "scene", (double)fl_scene_edit_scene(edit));
    cJSON_AddNumberToObject(o, "time", (double)fl_scene_edit_time(edit) / FL_TICKS_PER_SECOND);
    cJSON_AddBoolToObject(o, "playing", fl_playback_playing(fl_workspace_playback(s->workspace)));
    for (size_t i = 0; scene && i < scene->entity_count; ++i)
        if (fl_scene_edit_contains(edit, scene->entities[i].id)) cJSON_AddItemToArray(selected, cJSON_CreateNumber((double)scene->entities[i].id));
    fl_mcp_return_json(call, o);
}

static void tool_set_editor_state(Fl_McpCall *call, const cJSON *args, void *data)
{
    static const char *const keys[] = {"scene", "time", "select", NULL};
    Fl_McpService *s = data;
    if (!only_keys(call, args, keys) || !require_editor(s, call)) return;
    Fl_SceneEdit *edit = edit_of(s);
    double scene_id = (double)fl_scene_edit_scene(edit), seconds = 0;
    bool has_scene, has_time;
    if (!number_arg(call, args, "scene", false, &scene_id, &has_scene) || !number_arg(call, args, "time", false, &seconds, &has_time)) return;
    const cJSON *select = cJSON_GetObjectItemCaseSensitive(args, "select");
    if (select && !cJSON_IsArray(select)) {fl_mcp_return_error(call, "invalid_argument", "\"select\" must be an array of entity ids"); return;}
    const Fl_Scene *scene = fl_project_scene(s->project, (Fl_Id)scene_id);
    if (!scene) {fl_mcp_return_error(call, "not_found", "No scene with id %.0f", scene_id); return;}
    if (has_scene || select) fl_scene_edit_select(edit, scene->id, 0, false);
    const cJSON *id;
    cJSON_ArrayForEach(id, select) {
        if (!cJSON_IsNumber(id) || !fl_scene_edit_select(edit, scene->id, (Fl_Id)id->valuedouble, true)) {
            fl_mcp_return_error(call, "not_found", "Entity %.0f is not in scene %llu", cJSON_IsNumber(id) ? id->valuedouble : 0, (unsigned long long)scene->id); return;
        }
    }
    if (has_time) fl_scene_edit_set_time(edit, (Fl_Tick)(seconds * FL_TICKS_PER_SECOND + (seconds < 0 ? -.5 : .5)));
    tool_get_editor_state(call, NULL, data);
}

/* ---------- lifecycle ---------- */

void fl_mcp_service_detach(Fl_McpService *s)
{
    if (!s) return;
    s->project = NULL; s->window = NULL; s->workspace = NULL;
    fl_agent_reset(s->agent);
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
    fl_scene_renderer_destroy(s->renderer);
    free(s);
}

/* Register the tools that need the editor window. */
static bool register_editor_tools(Fl_McpService *s)
{
    static const char *const empty = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
    const Fl_McpTool tools[] = {
        {"get_editor_state", "The scene, time and selection currently shown in the editor.", empty, true, tool_get_editor_state, s},
        {"set_editor_state", "Show a scene, move the playhead (seconds) or select entities in the editor so the user can follow along.",
         "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":\"integer\"},\"time\":{\"type\":\"number\"},\"select\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}},\"additionalProperties\":false}",
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
    s->renderer = fl_scene_renderer_create(&(Fl_SceneRendererDesc){.instance = instance, .fonts = fonts, .jobs = jobs});
    s->agent = fl_agent_create(&(Fl_AgentHost){current_project, render_scene, s});
    s->mcp = fl_mcp_create(&(Fl_McpDesc){"flow", FLOW_VERSION, fl_agent_instructions(), post_task, s, 0});
    bool ok = s->task_event != FL_EVENT_TYPE_INVALID && s->renderer && s->agent && s->mcp && load_config(s, &port) &&
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
