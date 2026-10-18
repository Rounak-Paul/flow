#include "fl_editor.h"

#include "fl_menubar.h"
#include "fl_workspace.h"
#include "fl_launcher.h"
#include "fl_mcp_service.h"
#include "core/fl_clock.h"
#include "style.h"

#include "core/fl_atomic.h"
#include "core/fl_log.h"
#include "core/fl_thread.h"
#include "platform/fl_crash.h"
#include "platform/fl_fs.h"
#include "platform/fl_log_file.h"
#include "platform/fl_paths.h"
#include "platform/fl_signal.h"

#include <causality.h>

#include <stdlib.h>
#include <string.h>

#define LOG_TAG "editor"

struct Fl_Editor {
    Fl_EventBus      *bus;
    Fl_JobSystem     *jobs;
    Fl_Fonts         *fonts;
    Ca_Instance      *instance;
    Ca_Stylesheet    *stylesheet;
    Ca_Window        *window;
    Fl_Workspace     *workspace;
    Fl_MenuBar       *menubar;
    Fl_Launcher *launcher;
    Fl_McpService *mcp;
    Fl_Project *project;
    uint64_t save_after;
    bool show_projects;
    Ca_Effect        *bus_pump;
    Fl_EventType      quit_event;
    Fl_EventType      log_event;
    Fl_LogSink        log_bridge;
    volatile uint32_t ui_ready;     /* set once causality can be woken */
    bool              crash_installed;
    bool              termination_watched;
};

/*
 * Bus wake hook: interrupts causality's blocking event wait so queued events
 * are delivered on the next tick. Thread-safe; ignored until the UI exists.
 *
 * user_data  Owning editor.
 */
static void wake_ui_loop(void *user_data)
{
    Fl_Editor *editor = user_data;
    if (fl_atomic_load_u32(&editor->ui_ready)) ca_instance_wake();
}

/* Build a project window after a launcher selection on the UI thread. */
static bool open_project_window(Fl_Editor *editor);

/* Pump operations and debounce project saves; window transitions occur outside UI callbacks. */
static void pump_bus(void *user_data)
{
    Fl_Editor *editor = user_data;
    fl_event_bus_dispatch(editor->bus);
    if (editor->show_projects && editor->window) {
        editor->show_projects = false;
        if (!fl_project_save(editor->project)) {
            FL_LOG_ERROR(LOG_TAG, "cannot close project: save failed"); return;
        }
        editor->launcher = fl_launcher_create(editor->instance, editor->bus);
        if (editor->launcher) ca_window_destroy(editor->window);
    }
    if (editor->launcher) {
        Fl_Project *selected = fl_launcher_take_project(editor->launcher);
        if (selected) {
            editor->project = selected;
            if (open_project_window(editor)) {
                fl_launcher_destroy(editor->launcher); editor->launcher = NULL;
            } else {
                if (editor->window) ca_window_destroy(editor->window);
                else { fl_project_destroy(editor->project); editor->project = NULL; }
                FL_LOG_ERROR(LOG_TAG, "could not open project workspace");
            }
        }
    }
    if (editor->project && fl_project_dirty(editor->project) &&
        fl_clock_monotonic_ns() >= editor->save_after) {
        if (!fl_project_save(editor->project)) FL_LOG_ERROR(LOG_TAG, "automatic save failed; retrying in five seconds");
        editor->save_after = fl_clock_monotonic_ns() + UINT64_C(5000000000);
        if (fl_project_dirty(editor->project)) ca_instance_request_frame_after(editor->instance, 5.0);
    }
}

/* Observe model mutations and schedule one save after edits settle. */
static bool on_project_changed(const Fl_Event *event, void *user_data)
{
    (void)event; Fl_Editor *editor = user_data;
    editor->save_after = fl_clock_monotonic_ns() + UINT64_C(750000000);
    ca_instance_request_frame_after(editor->instance, 0.75);
    return false;
}

/* Defer project manager navigation until the menu callback has returned. */
static void request_projects(void *data)
{
    Fl_Editor *editor = data; editor->show_projects = true;
    ca_instance_request_frame_after(editor->instance, 0);
}

/*
 * Termination watcher callback (watcher thread): forward the request to the
 * UI thread as an event.
 *
 * signal_number  Signal that requested termination.
 * user_data      Owning editor.
 */
static void on_termination(int signal_number, void *user_data)
{
    Fl_Editor *editor = user_data;
    const int32_t sig = signal_number;
    fl_event_post(editor->bus, editor->quit_event, &sig, sizeof(sig));
}

/*
 * Quit-request observer: close the main window so the UI loop ends cleanly.
 *
 * event      FL_EVENT_APP_QUIT_REQUESTED.
 * user_data  Owning editor.
 * Returns    false; other observers may also react.
 */
static bool on_quit_requested(const Fl_Event *event, void *user_data)
{
    Fl_Editor *editor = user_data;
    const int32_t *sig = event->payload;
    FL_LOG_INFO(LOG_TAG, "quit requested (signal %d)", sig ? *sig : 0);
    if (editor->window) ca_window_close(editor->window);
    else if (editor->launcher) { fl_launcher_destroy(editor->launcher); editor->launcher = NULL; }
    return false;
}

/*
 * Log sink bridging records into FL_EVENT_LOG_RECORD events for in-app
 * observers. Runs on the logging thread.
 *
 * record     Log record.
 * user_data  Owning editor.
 */
static void bridge_log_record(const Fl_LogRecord *record, void *user_data)
{
    Fl_Editor *editor = user_data;
    Fl_LogEvent event = { .sequence = record->sequence, .level = (int32_t)record->level };
    size_t tag_len = strlen(record->tag);
    if (tag_len >= sizeof(event.tag)) tag_len = sizeof(event.tag) - 1u;
    memcpy(event.tag, record->tag, tag_len);
    size_t message_len = record->message_length;
    if (message_len >= sizeof(event.message)) message_len = sizeof(event.message) - 1u;
    memcpy(event.message, record->message, message_len);
    fl_event_post(editor->bus, editor->log_event, &event, sizeof(event));
}

/*
 * Worker lifecycle hooks: make job threads crash-reportable.
 */
static void on_worker_start(uint32_t worker_index, void *user_data)
{
    (void)worker_index;
    (void)user_data;
    fl_crash_thread_attach();
}

static void on_worker_exit(uint32_t worker_index, void *user_data)
{
    (void)worker_index;
    (void)user_data;
    fl_crash_thread_detach();
}

/*
 * Main window close notification: secondary windows must not outlive it.
 *
 * window     Main window being closed.
 * user_data  Owning editor.
 */
static void on_main_window_close(Ca_Window *window, void *user_data)
{
    Fl_Editor *editor = user_data;
    fl_mcp_service_detach(editor->mcp);
    /* Menus and the workspace (which closes its floating windows) go while the main window exists. */
    fl_menubar_destroy(editor->menubar);
    editor->menubar = NULL;
    fl_workspace_destroy(editor->workspace);
    editor->workspace = NULL;
    if (!editor->launcher) ca_instance_destroy_other_windows(editor->instance, window);
    fl_project_destroy(editor->project); editor->project = NULL;
    editor->window = NULL;
}

/* Refuse to close a project when its current changes cannot be saved. */
static bool on_project_close_request(Ca_Window *window, void *data)
{
    (void)window; Fl_Editor *editor = data;
    if (!editor->project || fl_project_save(editor->project)) return true;
    ca_popup_show(editor->instance, &(Ca_PopupDesc){.title = "Project not saved",
        .message = "Saving failed. The project remains open so you can retry."});
    return false;
}

/*
 * Build the main window's UI tree: the app root hosting the dock workspace.
 *
 * editor   Owning editor with a live main window.
 * Returns  false when the workspace cannot be created or mounted.
 */
static bool build_ui(Fl_Editor *editor)
{
    char config[FL_PATH_MAX], layout[FL_PATH_MAX];
    const bool persist = fl_paths_user_dir(FL_PATH_CONFIG, config, sizeof(config)) &&
                         fl_fs_join(layout, sizeof(layout), config, "workspace.json");
    if (!persist) FL_LOG_WARN(LOG_TAG, "config directory unavailable; workspace layout will not persist");

    editor->workspace = fl_workspace_create(&(Fl_WorkspaceDesc){
        .window = editor->window, .bus = editor->bus, .jobs = editor->jobs, .fonts = editor->fonts,
        .project = editor->project, .layout_path = persist ? layout : NULL,
    });
    if (!editor->workspace) return false;

    ca_ui_begin(editor->window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = FL_CLASS_APP_ROOT,
    });
    const bool mounted = fl_workspace_mount(editor->workspace);
    ca_ui_end();
    if (!mounted) return false;
    editor->menubar = fl_menubar_create(editor->window, editor->bus, editor->workspace, editor->project, request_projects, editor);
    return editor->menubar != NULL;
}

/*
 * Start logging and crash capture. Failures degrade gracefully: the app can
 * run with console-only logging and without crash reports.
 */
static void start_diagnostics(Fl_Editor *editor)
{
#if defined(NDEBUG)
    const Fl_LogLevel level = FL_LOG_INFO;
#else
    const Fl_LogLevel level = FL_LOG_DEBUG;
#endif
    fl_log_init(&(Fl_LogDesc){ .level = level, .console = true });
    FL_LOG_INFO(LOG_TAG, "Flow %s starting", FLOW_VERSION);

    char dir[FL_PATH_MAX];
    if (fl_paths_user_dir(FL_PATH_LOGS, dir, sizeof(dir)) &&
        fl_log_file_open(&(Fl_LogFileDesc){ .dir = dir, .min_level = FL_LOG_TRACE })) {
        char path[FL_PATH_MAX];
        if (fl_log_file_path(path, sizeof(path))) FL_LOG_INFO(LOG_TAG, "log file: %s", path);
    } else {
        FL_LOG_WARN(LOG_TAG, "file logging unavailable; continuing with console only");
    }

    if (!fl_paths_user_dir(FL_PATH_CRASHES, dir, sizeof(dir))) {
        FL_LOG_WARN(LOG_TAG, "crash report directory unavailable; crash capture disabled");
        return;
    }
    char previous[FL_PATH_MAX];
    if (fl_crash_take_previous_report(dir, previous, sizeof(previous)))
        FL_LOG_WARN(LOG_TAG, "Flow crashed during the previous session; report: %s", previous);
    editor->crash_installed = fl_crash_install(&(Fl_CrashDesc){
        .report_dir = dir, .app_name = "Flow", .app_version = FLOW_VERSION,
    });
    if (editor->crash_installed) FL_LOG_INFO(LOG_TAG, "crash reports: %s", dir);
    else FL_LOG_WARN(LOG_TAG, "crash handler installation failed");
}

/*
 * Create the bus, its application events and the termination watcher. The
 * watcher must start before any other thread so they inherit its mask.
 */
static bool start_events(Fl_Editor *editor)
{
    editor->bus = fl_event_bus_create(&(Fl_EventBusDesc){ .wake = wake_ui_loop, .wake_data = editor });
    if (!editor->bus) {
        FL_LOG_ERROR(LOG_TAG, "failed to create event bus");
        return false;
    }
    editor->quit_event = fl_event_type_register(editor->bus, FL_EVENT_APP_QUIT_REQUESTED, sizeof(int32_t));
    editor->log_event = fl_event_type_register(editor->bus, FL_EVENT_LOG_RECORD, sizeof(Fl_LogEvent));
    if (editor->quit_event == FL_EVENT_TYPE_INVALID || editor->log_event == FL_EVENT_TYPE_INVALID) {
        FL_LOG_ERROR(LOG_TAG, "failed to register application events");
        return false;
    }
    if (!fl_event_subscribe(editor->bus, &(Fl_SubscribeDesc){
            .type = editor->quit_event, .fn = on_quit_requested, .user_data = editor,
            .owner = editor }).generation) {
        FL_LOG_ERROR(LOG_TAG, "failed to subscribe to quit requests");
        return false;
    }

    editor->termination_watched = fl_termination_watch_start(on_termination, editor);
    if (!editor->termination_watched)
        FL_LOG_WARN(LOG_TAG, "termination watcher unavailable; signals use default handling");

    editor->log_bridge = fl_log_add_sink(&(Fl_LogSinkDesc){
        .write = bridge_log_record, .user_data = editor, .min_level = FL_LOG_INFO,
    });
    if (!editor->log_bridge.id) FL_LOG_WARN(LOG_TAG, "log event bridge unavailable");
    return true;
}

static bool start_jobs(Fl_Editor *editor)
{
    editor->jobs = fl_jobs_create(&(Fl_JobSystemDesc){
        .bus = editor->bus,
        .on_thread_start = on_worker_start,
        .on_thread_exit = on_worker_exit,
    });
    if (!editor->jobs) {
        FL_LOG_ERROR(LOG_TAG, "failed to start job system");
        return false;
    }
    FL_LOG_INFO(LOG_TAG, "job system: %u workers", fl_jobs_worker_count(editor->jobs));
    editor->fonts = fl_fonts_create(&(Fl_FontsDesc){.bus = editor->bus, .jobs = editor->jobs, .scan_system = true});
    if (!editor->fonts) {
        FL_LOG_ERROR(LOG_TAG, "failed to start the font service");
        return false;
    }
    return true;
}

/* Assemble independent style sections without exceeding C's literal-size limit. */
static Ca_Stylesheet *create_stylesheet(void)
{
    const char *parts[] = {FL_STYLE_CSS_PALETTE, FL_STYLE_CSS_SYSTEM, FL_STYLE_CSS_COMPONENTS, FL_STYLE_CSS_SCENE, FL_STYLE_CSS_SCENE_PANES, FL_STYLE_CSS_TREE, FL_STYLE_CSS_AUTHORING, FL_STYLE_CSS_ASSETS, FL_STYLE_CSS_SCRIPTS, FL_STYLE_CSS_FILE_BROWSER, FL_STYLE_CSS_TIMELINE, FL_STYLE_CSS_TIMELINE_BEATS, FL_STYLE_CSS_MOTION};
    size_t size = 1;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) size += strlen(parts[i]);
    char *css = malloc(size);
    if (!css) return NULL;
    css[0] = 0;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) strcat(css, parts[i]);
    Ca_Stylesheet *sheet = ca_css_parse(css); free(css); return sheet;
}

static bool start_ui(Fl_Editor *editor)
{
    editor->instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = FL_STYLE_APP_NAME,
        .prefer_dedicated_gpu = true,
        .default_ui_scale     = FL_STYLE_UI_SCALE,
    });
    if (!editor->instance) {
        FL_LOG_ERROR(LOG_TAG, "failed to create causality instance");
        return false;
    }
    fl_atomic_store_u32(&editor->ui_ready, 1u);

    editor->stylesheet = create_stylesheet();
    if (!editor->stylesheet) {
        FL_LOG_ERROR(LOG_TAG, "failed to parse the %s stylesheet", FL_THEME_NAME);
        return false;
    }
    ca_instance_set_stylesheet(editor->instance, editor->stylesheet);
    FL_LOG_INFO(LOG_TAG, "theme: %s", FL_THEME_NAME);

    editor->launcher = fl_launcher_create(editor->instance, editor->bus);
    if (!editor->launcher) return false;
    Fl_EventType changed = fl_event_type_register(editor->bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange));
    if (changed == FL_EVENT_TYPE_INVALID || !fl_event_subscribe(editor->bus, &(Fl_SubscribeDesc){
            .type = changed, .fn = on_project_changed, .user_data = editor, .owner = editor}).generation) return false;

    editor->bus_pump = ca_frame_effect(editor->instance, pump_bus, editor);
    if (!editor->bus_pump) {
        FL_LOG_ERROR(LOG_TAG, "failed to register event bus pump");
        return false;
    }
    editor->mcp = fl_mcp_service_create(editor->bus, editor->instance, editor->fonts, editor->jobs);
    return true;
}

/* Create the editor workspace only after a project is selected. */
static bool open_project_window(Fl_Editor *editor)
{
    editor->window = ca_window_create(editor->instance, &(Ca_WindowDesc){
        .title         = FL_STYLE_APP_NAME " " FLOW_VERSION,
        .width         = FL_STYLE_WINDOW_RESTORE_W,
        .height        = FL_STYLE_WINDOW_RESTORE_H,
        .on_close      = on_main_window_close,
        .on_close_data = editor,
        .on_close_request = on_project_close_request,
    });
    if (!editor->window) {
        FL_LOG_ERROR(LOG_TAG, "failed to create main window");
        return false;
    }
    ca_window_maximize(editor->window);
    if (!build_ui(editor)) {
        FL_LOG_ERROR(LOG_TAG, "failed to build the workspace");
        return false;
    }

    ca_window_set_title(editor->window, fl_project_name(editor->project));
    fl_mcp_service_attach(editor->mcp, editor->project, editor->window, editor->workspace);
    return true;
}

Fl_Editor *fl_editor_create(void)
{
    fl_thread_set_name("flow-main");
    Fl_Editor *editor = calloc(1, sizeof(*editor));
    if (!editor) return NULL;

    start_diagnostics(editor);
    if (!start_events(editor) || !start_jobs(editor) || !start_ui(editor)) {
        fl_editor_destroy(editor);
        return NULL;
    }
    FL_LOG_INFO(LOG_TAG, "ready");
    return editor;
}

bool fl_editor_open_project(Fl_Editor *editor, const char *directory)
{
    if (!editor || !directory || !editor->launcher || editor->project) return false;
    editor->project = fl_project_open(directory, editor->bus);
    if (!editor->project) {FL_LOG_ERROR(LOG_TAG, "cannot open project %s", directory); return false;}
    if (!open_project_window(editor)) {
        if (editor->window) ca_window_destroy(editor->window);
        else {fl_project_destroy(editor->project); editor->project = NULL;}
        return false;
    }
    fl_launcher_destroy(editor->launcher); editor->launcher = NULL;
    return true;
}

int fl_editor_run(Fl_Editor *editor)
{
    if (!editor || !editor->instance) return 1;
    while (ca_instance_tick(editor->instance)) {
    }
    return 0;
}

void fl_editor_destroy(Fl_Editor *editor)
{
    if (!editor) return;
    FL_LOG_INFO(LOG_TAG, "shutting down");

    /* Agents are cut off first so no tool call can reach a project that is about to close. */
    fl_mcp_service_destroy(editor->mcp);
    editor->mcp = NULL;
    /* Job clients (workspace importer/preview, fonts) wait on their own jobs before the pool stops. */
    fl_menubar_destroy(editor->menubar);
    editor->menubar = NULL;
    fl_workspace_destroy(editor->workspace);
    editor->workspace = NULL;
    fl_launcher_destroy(editor->launcher); editor->launcher = NULL;
    if (editor->project) fl_project_save(editor->project);
    fl_project_destroy(editor->project); editor->project = NULL;
    fl_fonts_destroy(editor->fonts);
    editor->fonts = NULL;

    if (editor->jobs && !fl_jobs_destroy(editor->jobs))
        FL_LOG_ERROR(LOG_TAG, "job system destroyed from a worker thread");
    editor->jobs = NULL;

    /* Stop every off-thread producer before the UI it wakes goes away. */
    if (editor->termination_watched) fl_termination_watch_stop();
    editor->termination_watched = false;
    if (editor->log_bridge.id) fl_log_remove_sink(editor->log_bridge);
    editor->log_bridge = (Fl_LogSink){0};
    ca_effect_destroy(editor->bus_pump);
    editor->bus_pump = NULL;
    fl_atomic_store_u32(&editor->ui_ready, 0u);
    if (editor->instance) ca_instance_destroy(editor->instance);
    editor->instance = NULL;
    /* Causality borrows the stylesheet; it may only go once the instance has. */
    ca_css_destroy(editor->stylesheet);
    editor->stylesheet = NULL;

    if (editor->bus && !fl_event_bus_destroy(editor->bus))
        FL_LOG_ERROR(LOG_TAG, "event bus destroyed from an invalid context");

    FL_LOG_INFO(LOG_TAG, "goodbye");
    if (editor->crash_installed) fl_crash_uninstall();
    fl_log_file_close();
    fl_log_shutdown();
    free(editor);
}

Fl_EventBus *fl_editor_bus(Fl_Editor *editor)
{
    return editor ? editor->bus : NULL;
}

Fl_JobSystem *fl_editor_jobs(Fl_Editor *editor)
{
    return editor ? editor->jobs : NULL;
}
