#include "fl_workspace.h"

#include "fl_console.h"
#include "style.h"

#include "core/fl_clock.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"

#include <stdlib.h>
#include <string.h>

#define LOG_TAG             "workspace"
#define LAYOUT_FILE_MAX     (1u << 20)
#define SAVE_SETTLE_SECONDS 0.75
#define SAVE_SETTLE_NS      750000000ull
#define SCENE_PANEL_ID      "scene"

struct Fl_Workspace {
    Ca_Instance *instance;
    Fl_EventBus *bus;
    Ca_Dock     *dock;
    Fl_Console  *console;
    Ca_Effect   *saver;
    char        *layout_path;
    char        *saved_layout;      /* read by the dock during mount */
    Fl_EventType opened_event;
    Fl_EventType closed_event;
    Fl_EventType activated_event;
    Fl_EventType changed_event;
    bool         mounted;
    bool         save_pending;
    uint64_t     save_after_ns;
};

/*
 * Scene panel body: the stage surface the scene renderer will draw into.
 *
 * dock       Owning dock.
 * window     Window hosting the panel.
 * user_data  Unused.
 */
static void build_scene_panel(Ca_Dock *dock, Ca_Window *window, void *user_data)
{
    (void)dock;
    (void)window;
    (void)user_data;
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = FL_CLASS_SCENE });
    ca_div_end();
}

/*
 * Factory arrangement: the scene is the central group, so it alone absorbs
 * window resizes and keeps its area when emptied; the console sits below it.
 *
 * layout     Cleared layout with every panel registered.
 * user_data  Unused.
 */
static void seed_default_layout(Ca_DockLayout *layout, void *user_data)
{
    (void)user_data;
    const int scene = ca_dock_layout_new_group(layout);
    const int console = ca_dock_layout_new_group(layout);
    ca_dock_layout_add_tab(layout, scene, ca_dock_layout_find(layout, SCENE_PANEL_ID), -1);
    ca_dock_layout_add_tab(layout, console, ca_dock_layout_find(layout, FL_CONSOLE_PANEL_ID), -1);
    const int root = ca_dock_layout_new_split(layout, false, 0.75f, scene, console);
    ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, root);
    ca_dock_layout_set_central(layout, scene);
    ca_dock_layout_set_split_size(layout, root, FL_STYLE_CONSOLE_HEIGHT);
}

/*
 * Dock observer: republish dock changes on the application bus.
 *
 * dock       Dock that changed.
 * event      Dock change.
 * user_data  Owning workspace.
 */
static void forward_dock_event(Ca_Dock *dock, const Ca_DockEvent *event, void *user_data)
{
    (void)dock;
    Fl_Workspace *workspace = user_data;
    Fl_EventType type = FL_EVENT_TYPE_INVALID;
    switch (event->kind) {
    case CA_DOCK_EVENT_PANEL_OPENED:    type = workspace->opened_event; break;
    case CA_DOCK_EVENT_PANEL_CLOSED:    type = workspace->closed_event; break;
    case CA_DOCK_EVENT_PANEL_ACTIVATED: type = workspace->activated_event; break;
    case CA_DOCK_EVENT_LAYOUT_CHANGED:  type = workspace->changed_event; break;
    }
    Fl_DockEventPayload payload = {0};
    if (event->panel_id) {
        size_t len = strlen(event->panel_id);
        if (len >= sizeof(payload.panel_id)) len = sizeof(payload.panel_id) - 1u;
        memcpy(payload.panel_id, event->panel_id, len);
    }
    fl_event_publish(workspace->bus, type, &payload, sizeof(payload));
}

/*
 * Persistence observer: schedule a save once the layout has settled.
 *
 * event      FL_EVENT_DOCK_LAYOUT_CHANGED.
 * user_data  Owning workspace.
 * Returns    false; other observers also receive the change.
 */
static bool on_layout_changed(const Fl_Event *event, void *user_data)
{
    (void)event;
    Fl_Workspace *workspace = user_data;
    if (!workspace->layout_path) return false;
    workspace->save_pending = true;
    workspace->save_after_ns = fl_clock_monotonic_ns() + SAVE_SETTLE_NS;
    ca_instance_request_frame_after(workspace->instance, SAVE_SETTLE_SECONDS);
    return false;
}

/*
 * Per-tick check that performs a scheduled save once it is due.
 *
 * user_data  Owning workspace.
 */
static void flush_pending_save(void *user_data)
{
    Fl_Workspace *workspace = user_data;
    if (workspace->save_pending && fl_clock_monotonic_ns() >= workspace->save_after_ns)
        fl_workspace_save(workspace);
}

static bool register_events(Fl_Workspace *workspace)
{
    const size_t size = sizeof(Fl_DockEventPayload);
    workspace->opened_event = fl_event_type_register(workspace->bus, FL_EVENT_DOCK_PANEL_OPENED, size);
    workspace->closed_event = fl_event_type_register(workspace->bus, FL_EVENT_DOCK_PANEL_CLOSED, size);
    workspace->activated_event = fl_event_type_register(workspace->bus, FL_EVENT_DOCK_PANEL_ACTIVATED, size);
    workspace->changed_event = fl_event_type_register(workspace->bus, FL_EVENT_DOCK_LAYOUT_CHANGED, size);
    if (workspace->opened_event == FL_EVENT_TYPE_INVALID || workspace->closed_event == FL_EVENT_TYPE_INVALID ||
        workspace->activated_event == FL_EVENT_TYPE_INVALID || workspace->changed_event == FL_EVENT_TYPE_INVALID)
        return false;
    return fl_event_subscribe(workspace->bus, &(Fl_SubscribeDesc){
        .type = workspace->changed_event, .fn = on_layout_changed, .user_data = workspace,
        .owner = workspace,
    }).generation != 0;
}

static bool create_dock(Fl_Workspace *workspace, Ca_Window *window)
{
    workspace->dock = ca_dock_create(window, &(Ca_DockDesc){
        .default_layout = seed_default_layout,
        .saved_layout   = workspace->saved_layout,
        .on_event       = forward_dock_event,
        .event_data     = workspace,
    });
    if (!workspace->dock) return false;
    const bool scene = ca_dock_register_panel(workspace->dock, &(Ca_DockPanelDesc){
        .id = SCENE_PANEL_ID, .title = "Scene", .build = build_scene_panel,
    });
    const bool console = scene && ca_dock_register_panel(workspace->dock, &(Ca_DockPanelDesc){
        .id = FL_CONSOLE_PANEL_ID, .title = "Console", .dock_beside = SCENE_PANEL_ID,
        .build = fl_console_build_panel, .teardown = fl_console_teardown_panel,
        .user_data = workspace->console,
    });
    if (!console) FL_LOG_ERROR(LOG_TAG, "failed to register workspace panels");
    return console;
}

Fl_Workspace *fl_workspace_create(const Fl_WorkspaceDesc *desc)
{
    if (!desc || !desc->window || !desc->bus) return NULL;
    Fl_Workspace *workspace = calloc(1, sizeof(*workspace));
    if (!workspace) return NULL;
    workspace->instance = ca_window_instance(desc->window);
    workspace->bus = desc->bus;

    if (desc->layout_path) {
        const size_t len = strlen(desc->layout_path);
        workspace->layout_path = malloc(len + 1u);
        if (!workspace->layout_path) {
            fl_workspace_destroy(workspace);
            return NULL;
        }
        memcpy(workspace->layout_path, desc->layout_path, len + 1u);
        workspace->saved_layout = fl_fs_read_file(workspace->layout_path, LAYOUT_FILE_MAX, NULL);
    }
    if (!register_events(workspace)) {
        FL_LOG_ERROR(LOG_TAG, "failed to register workspace events");
        fl_workspace_destroy(workspace);
        return NULL;
    }
    workspace->console = fl_console_create(workspace->bus, workspace->instance);
    if (!workspace->console) {
        FL_LOG_ERROR(LOG_TAG, "failed to create the console");
        fl_workspace_destroy(workspace);
        return NULL;
    }
    if (!create_dock(workspace, desc->window)) {
        FL_LOG_ERROR(LOG_TAG, "failed to create the dock");
        fl_workspace_destroy(workspace);
        return NULL;
    }
    workspace->saver = ca_frame_effect(workspace->instance, flush_pending_save, workspace);
    if (!workspace->saver) {
        FL_LOG_ERROR(LOG_TAG, "failed to register layout persistence");
        fl_workspace_destroy(workspace);
        return NULL;
    }
    return workspace;
}

bool fl_workspace_mount(Fl_Workspace *workspace)
{
    if (!workspace || !workspace->dock) return false;
    const bool restored = workspace->saved_layout != NULL;
    const bool mounted = ca_dock_mount(workspace->dock);
    free(workspace->saved_layout);
    workspace->saved_layout = NULL;
    if (!mounted) {
        FL_LOG_ERROR(LOG_TAG, "failed to mount the dock");
        return false;
    }
    workspace->mounted = true;
    FL_LOG_INFO(LOG_TAG, "%s layout", restored ? "restored saved" : "using default");
    return true;
}

bool fl_workspace_save(Fl_Workspace *workspace)
{
    if (!workspace || !workspace->mounted) return false;
    workspace->save_pending = false;
    if (!workspace->layout_path) return true;

    const size_t needed = ca_dock_save(workspace->dock, NULL, 0);
    char *text = needed ? malloc(needed + 1u) : NULL;
    const bool written = text && ca_dock_save(workspace->dock, text, needed + 1u) == needed &&
                         fl_fs_write_atomic(workspace->layout_path, text, needed);
    free(text);
    if (!written) FL_LOG_WARN(LOG_TAG, "failed to save layout to %s", workspace->layout_path);
    return written;
}

void fl_workspace_destroy(Fl_Workspace *workspace)
{
    if (!workspace) return;
    if (workspace->mounted) fl_workspace_save(workspace);
    ca_effect_destroy(workspace->saver);
    ca_dock_destroy(workspace->dock);
    fl_console_destroy(workspace->console);
    fl_event_unsubscribe_owner(workspace->bus, workspace);
    free(workspace->saved_layout);
    free(workspace->layout_path);
    free(workspace);
}

int fl_workspace_panel_count(const Fl_Workspace *workspace)
{
    return workspace ? ca_dock_panel_count(workspace->dock) : 0;
}

const char *fl_workspace_panel_id(const Fl_Workspace *workspace, int index)
{
    return workspace ? ca_dock_panel_id(workspace->dock, index) : NULL;
}

const char *fl_workspace_panel_title(const Fl_Workspace *workspace, int index)
{
    return workspace ? ca_dock_panel_title(workspace->dock, index) : NULL;
}

bool fl_workspace_panel_visible(const Fl_Workspace *workspace, const char *id)
{
    return workspace && ca_dock_panel_visible(workspace->dock, id);
}

bool fl_workspace_toggle_panel(Fl_Workspace *workspace, const char *id)
{
    return workspace && workspace->mounted && ca_dock_toggle_panel(workspace->dock, id);
}
