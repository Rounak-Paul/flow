#include "fl_script_sync.h"
#include "components/fl_script.h"
#include "core/fl_clock.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_process.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "scripts"
#define POLL_SECONDS 0.5
#define POLL_NS UINT64_C(450000000)

/* One script open in the system editor and the last text both sides agreed on. */
typedef struct Fl_ScriptSession {
    Fl_Id scene, script;
    char path[FL_PATH_MAX];
    int64_t modified;
    char *source;
    size_t source_size;
} Fl_ScriptSession;

struct Fl_ScriptSync {
    Fl_Project *project;
    Fl_EventBus *bus;
    Ca_Instance *instance;
    Ca_Effect *poller;
    Fl_ScriptSession *sessions;
    size_t count, capacity;
    uint64_t last_poll_ns;
};

/* Replace a session's remembered text with a copy of source. */
static bool remember(Fl_ScriptSession *session, const char *source, size_t size)
{
    char *copy = malloc(size + 1);
    if (!copy) return false;
    memcpy(copy, source, size); copy[size] = 0;
    free(session->source);
    session->source = copy; session->source_size = size;
    return true;
}

/* Write the session's remembered text to its file and record the file's new modification time. */
static bool write_session(Fl_ScriptSession *session)
{
    return fl_fs_write_atomic(session->path, session->source, session->source_size) && fl_fs_modified_ns(session->path, &session->modified);
}

/* End one session and remove its file. */
static void drop(Fl_ScriptSync *s, size_t index)
{
    fl_fs_remove(s->sessions[index].path);
    free(s->sessions[index].source);
    memmove(s->sessions + index, s->sessions + index + 1, (s->count - index - 1) * sizeof(*s->sessions));
    --s->count;
}

/* Reconcile every session with the document: write back changed sources, drop sessions whose script is gone. */
static bool project_changed(const Fl_Event *event, void *data)
{
    (void)event;
    Fl_ScriptSync *s = data;
    for (size_t i = s->count; i-- > 0; ) {
        Fl_ScriptSession *session = &s->sessions[i];
        const Fl_ScriptSource *script = fl_scene_script(fl_project_scene(s->project, session->scene), session->script);
        if (!script) {drop(s, i); continue;}
        if (script->source_size == session->source_size && !memcmp(script->source, session->source, session->source_size)) continue;
        if (remember(session, script->source, script->source_size) && !write_session(session)) FL_LOG_WARN(LOG_TAG, "could not update %s", session->path);
    }
    return false;
}

/* Turn a saved edit in the system editor into one document operation. */
static void pull(Fl_ScriptSync *s, Fl_ScriptSession *session)
{
    int64_t modified;
    if (!fl_fs_modified_ns(session->path, &modified) || modified == session->modified) return;
    session->modified = modified;
    size_t size = 0;
    char *text = fl_fs_read_file(session->path, FL_SCRIPT_SOURCE_LIMIT, &size);
    if (!text) return;
    bool changed = fl_script_source_valid(text, size) && (size != session->source_size || memcmp(text, session->source, size));
    if (changed) {
        char *previous = session->source; size_t previous_size = session->source_size;
        session->source = text; session->source_size = size; text = NULL;
        const Fl_Id scene = session->scene, script = session->script;
        bool ok = fl_project_apply(s->project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene, .script = script,
                                   .source = session->source, .source_size = size}, NULL);
        if (!ok) FL_LOG_WARN(LOG_TAG, "the saved script was rejected and not applied");
        if (ok) free(previous);
        else {free(session->source); session->source = previous; session->source_size = previous_size;}
    }
    free(text);
}

/* Frame effect: check the files of open sessions twice a second, then keep the loop awake while any remain. */
static void poll(void *data)
{
    Fl_ScriptSync *s = data;
    if (!s->count) return;
    uint64_t now = fl_clock_monotonic_ns();
    if (now - s->last_poll_ns >= POLL_NS) {
        s->last_poll_ns = now;
        for (size_t i = s->count; i-- > 0; ) if (i < s->count) pull(s, &s->sessions[i]);
    }
    if (s->count) ca_instance_request_frame_after(s->instance, POLL_SECONDS);
}

Fl_ScriptSync *fl_script_sync_create(Fl_Project *project, Fl_EventBus *bus, Ca_Instance *instance)
{
    if (!project || !bus || !instance) return NULL;
    Fl_ScriptSync *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->project = project; s->bus = bus; s->instance = instance;
    s->poller = ca_frame_effect(instance, poll, s);
    bool subscribed = fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_PROJECT_CHANGED),
                                         .fn = project_changed, .user_data = s, .owner = s}).generation != 0;
    if (!s->poller || !subscribed) {fl_script_sync_destroy(s); return NULL;}
    return s;
}

void fl_script_sync_destroy(Fl_ScriptSync *s)
{
    if (!s) return;
    fl_event_unsubscribe_owner(s->bus, s);
    ca_effect_destroy(s->poller);
    while (s->count) drop(s, s->count - 1);
    free(s->sessions);
    free(s);
}

/* File name for a script: its id, then its name with anything unsafe replaced. */
static void file_name(const Fl_ScriptSource *script, char *out, size_t size)
{
    size_t n = (size_t)snprintf(out, size, "%llu-", (unsigned long long)script->id);
    for (const char *c = script->name; *c && n + 5 < size; ++c) out[n++] = isalnum((unsigned char)*c) || *c == '-' || *c == '_' ? *c : '_';
    snprintf(out + n, size - n, ".lua");
}

bool fl_script_sync_open(Fl_ScriptSync *s, Fl_Id scene_id, Fl_Id script_id, char *error, size_t error_size)
{
    const Fl_ScriptSource *script = s ? fl_scene_script(fl_project_scene(s->project, scene_id), script_id) : NULL;
    if (!script) {snprintf(error, error_size, "That script no longer exists."); return false;}
    Fl_ScriptSession *session = NULL;
    for (size_t i = 0; i < s->count; ++i) if (s->sessions[i].script == script_id && s->sessions[i].scene == scene_id) session = &s->sessions[i];
    if (!session) {
        if (s->count == s->capacity) {
            size_t capacity = s->capacity ? s->capacity * 2 : 8;
            Fl_ScriptSession *grown = realloc(s->sessions, capacity * sizeof(*grown));
            if (!grown) {snprintf(error, error_size, "Out of memory."); return false;}
            s->sessions = grown; s->capacity = capacity;
        }
        char folder[FL_PATH_MAX], leaf[FL_PATH_MAX], dot_flow[FL_PATH_MAX];
        Fl_ScriptSession fresh = {.scene = scene_id, .script = script_id};
        file_name(script, leaf, sizeof(leaf));
        if (!fl_fs_join(dot_flow, sizeof(dot_flow), fl_project_directory(s->project), ".flow") || !fl_fs_join(folder, sizeof(folder), dot_flow, "scripts") ||
            !fl_fs_mkdir_p(folder) || !fl_fs_join(fresh.path, sizeof(fresh.path), folder, leaf) || !remember(&fresh, script->source, script->source_size) ||
            !write_session(&fresh)) {
            free(fresh.source);
            snprintf(error, error_size, "Could not write the script file.");
            return false;
        }
        session = &s->sessions[s->count++];
        *session = fresh;
    }
    if (!fl_open_path(session->path)) {snprintf(error, error_size, "Could not open the script file in an editor."); return false;}
    s->last_poll_ns = fl_clock_monotonic_ns();
    ca_instance_request_frame_after(s->instance, POLL_SECONDS);
    return true;
}
