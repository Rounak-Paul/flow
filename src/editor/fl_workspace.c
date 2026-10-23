#include "fl_workspace.h"

#include "fl_asset_panel.h"
#include "fl_console.h"
#include "fl_scene_panel.h"
#include "fl_widget_library.h"
#include "fl_inspector.h"
#include "fl_script_panel.h"
#include "fl_manuscript_panel.h"
#include "fl_speech.h"
#include "fl_playback.h"
#include "fl_timeline_panel.h"
#include "fl_export_window.h"
#include "style.h"

#include "core/fl_clock.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG             "workspace"
#define LAYOUT_FILE_MAX     (1u << 20)
#define SAVE_SETTLE_SECONDS 0.75
#define SAVE_SETTLE_NS      750000000ull
#define SCENE_PANEL_ID      "scene"
#define ASSETS_PANEL_ID     "assets"
#define SCRIPTS_PANEL_ID    "scripts"
#define TIMELINE_PANEL_ID   "timeline"

struct Fl_Workspace {
    Ca_Instance *instance;
    Fl_EventBus *bus;
    Ca_Dock     *dock;
    Fl_Console  *console;
    Fl_SceneEdit *edit;
    Fl_ScenePanel *scene;
    Fl_WidgetLibrary *widgets;
    Fl_Inspector *inspector;
    Fl_AssetImporter *importer;
    Fl_AssetPanel *assets;
    Fl_ScriptPanel *scripts;
    Fl_ManuscriptPanel *manuscript;
    Fl_Speech *speech;
    Fl_AudioPlayer *player;
    Fl_Playback *playback;
    Fl_TimelinePanel *timeline;
    Fl_ExportWindow *export_panel;
    Fl_Fonts    *fonts;
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
    uint64_t     document_poll_ns;
    char         document_error[256];
};

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
    ca_dock_layout_add_tab(layout, console, ca_dock_layout_find(layout, TIMELINE_PANEL_ID), -1);
    ca_dock_layout_add_tab(layout, console, ca_dock_layout_find(layout, FL_CONSOLE_PANEL_ID), -1);
    const int export_panel = ca_dock_layout_find(layout, FL_EXPORT_PANEL_ID);
    if (export_panel != CA_DOCK_NONE) {
        const int active = ca_dock_layout_tab_index(layout, ca_dock_layout_active_panel(layout, console));
        ca_dock_layout_add_tab(layout, console, export_panel, -1);
        if (active >= 0) ca_dock_layout_set_active(layout, console, active);
    }
    const int manuscript = ca_dock_layout_find(layout, FL_MANUSCRIPT_PANEL_ID);
    if (manuscript != CA_DOCK_NONE) {
        const int active = ca_dock_layout_tab_index(layout, ca_dock_layout_active_panel(layout, console));
        ca_dock_layout_add_tab(layout, console, manuscript, -1);
        if (active >= 0) ca_dock_layout_set_active(layout, console, active);
    }
    const int root = ca_dock_layout_new_split(layout, false, 0.75f, scene, console);
    ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, root);
    ca_dock_layout_set_central(layout, scene);
    ca_dock_layout_set_split_size(layout, root, FL_STYLE_TIMELINE_HEIGHT);
    int widgets=ca_dock_layout_find(layout,"widgets"),inspector=ca_dock_layout_find(layout,"inspector");
    ca_dock_layout_dock_root(layout,CA_DOCK_MAIN_WINDOW,widgets,CA_DOCK_ZONE_LEFT,.16f);
    ca_dock_layout_set_split_size(layout,ca_dock_layout_window_root(layout,CA_DOCK_MAIN_WINDOW),FL_STYLE_WIDGETS_W);
    ca_dock_layout_add_tab(layout,ca_dock_layout_group_of(layout,widgets),ca_dock_layout_find(layout,ASSETS_PANEL_ID),-1);
    ca_dock_layout_add_tab(layout,ca_dock_layout_group_of(layout,widgets),ca_dock_layout_find(layout,SCRIPTS_PANEL_ID),-1);
    ca_dock_layout_dock_root(layout,CA_DOCK_MAIN_WINDOW,inspector,CA_DOCK_ZONE_RIGHT,.22f);
    ca_dock_layout_set_split_size(layout,ca_dock_layout_window_root(layout,CA_DOCK_MAIN_WINDOW),FL_STYLE_INSPECTOR_W);
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
    uint64_t now=fl_clock_monotonic_ns();
    if (now-workspace->document_poll_ns>=UINT64_C(500000000)) {
        workspace->document_poll_ns=now;
        Fl_Project *project=fl_scene_edit_project(workspace->edit);
        Fl_ReloadResult result=fl_project_reload(project);
        const char *error=fl_project_diagnostic(project);
        if (workspace->speech) fl_speech_pump(workspace->speech);
        if ((result==FL_RELOAD_INVALID || result==FL_RELOAD_CONFLICT) && strcmp(error,workspace->document_error)) {
            FL_LOG_WARN(LOG_TAG,"%s",error); snprintf(workspace->document_error,sizeof(workspace->document_error),"%s",error);
        } else if (result==FL_RELOAD_APPLIED) workspace->document_error[0]=0;
    }
    ca_instance_request_frame_after(workspace->instance,0.5);
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

/* Add newly introduced panels to existing layouts without resetting prior splits/floating windows. */
static void migrate_authoring_layout(Fl_Workspace *workspace)
{
    if (!workspace->saved_layout) return;
    Ca_DockLayout *layout=ca_dock_layout_create();if (!layout) return;
    const char *ids[9];
    size_t id_count = 0;
    ids[id_count++] = "scene"; ids[id_count++] = "console"; ids[id_count++] = "widgets"; ids[id_count++] = "inspector";
    ids[id_count++] = ASSETS_PANEL_ID; ids[id_count++] = TIMELINE_PANEL_ID; ids[id_count++] = SCRIPTS_PANEL_ID;
    if (workspace->export_panel) ids[id_count++] = FL_EXPORT_PANEL_ID;
    ids[id_count++] = FL_MANUSCRIPT_PANEL_ID;
    bool valid=true;
    for (size_t i=0;i<id_count;++i) valid &= ca_dock_layout_register(layout,ids[i])!=CA_DOCK_NONE;
    bool changed=false;
    if (valid&&ca_dock_layout_load(layout,workspace->saved_layout)) {
        int timeline=ca_dock_layout_find(layout,TIMELINE_PANEL_ID);
        int console_group=ca_dock_layout_group_of(layout,ca_dock_layout_find(layout,FL_CONSOLE_PANEL_ID));
        if (ca_dock_layout_group_of(layout,timeline)==CA_DOCK_NONE&&ca_dock_layout_home_of(layout,timeline)==CA_DOCK_NONE&&
            console_group!=CA_DOCK_NONE&&ca_dock_layout_add_tab(layout,console_group,timeline,0)) changed=true;
        for (size_t i=2;i<5;++i) {
            int panel=ca_dock_layout_find(layout,ids[i]);
            if (ca_dock_layout_group_of(layout,panel)!=CA_DOCK_NONE||ca_dock_layout_home_of(layout,panel)!=CA_DOCK_NONE) continue;
            int beside=i==4 ? ca_dock_layout_group_of(layout,ca_dock_layout_find(layout,"widgets")) : CA_DOCK_NONE;
            if (beside!=CA_DOCK_NONE) { ca_dock_layout_add_tab(layout,beside,panel,-1); changed=true; continue; }
            bool left=i!=3;
            if (ca_dock_layout_dock_root(layout,CA_DOCK_MAIN_WINDOW,panel,left ? CA_DOCK_ZONE_LEFT : CA_DOCK_ZONE_RIGHT,.2f)) {
                ca_dock_layout_set_split_size(layout,ca_dock_layout_window_root(layout,CA_DOCK_MAIN_WINDOW),
                                              i==3 ? FL_STYLE_INSPECTOR_W : i==4 ? FL_STYLE_ASSETS_W : FL_STYLE_WIDGETS_W);
                changed=true;
            }
        }
        const int scripts=ca_dock_layout_find(layout,SCRIPTS_PANEL_ID),library=ca_dock_layout_group_of(layout,ca_dock_layout_find(layout,"widgets"));
        if (ca_dock_layout_group_of(layout,scripts)==CA_DOCK_NONE&&ca_dock_layout_home_of(layout,scripts)==CA_DOCK_NONE&&library!=CA_DOCK_NONE&&
            ca_dock_layout_add_tab(layout,library,scripts,-1)) changed=true;
        const int export_panel=workspace->export_panel ? ca_dock_layout_find(layout,FL_EXPORT_PANEL_ID) : CA_DOCK_NONE;
        int bottom=ca_dock_layout_group_of(layout,timeline);
        if (bottom==CA_DOCK_NONE) bottom=console_group;
        if (export_panel!=CA_DOCK_NONE&&ca_dock_layout_group_of(layout,export_panel)==CA_DOCK_NONE&&
            ca_dock_layout_home_of(layout,export_panel)==CA_DOCK_NONE&&bottom!=CA_DOCK_NONE) {
            const int active=ca_dock_layout_tab_index(layout,ca_dock_layout_active_panel(layout,bottom));
            if (ca_dock_layout_add_tab(layout,bottom,export_panel,-1)) {
                if (active>=0) ca_dock_layout_set_active(layout,bottom,active);
                changed=true;
            }
        }
        const int manuscript = ca_dock_layout_find(layout, FL_MANUSCRIPT_PANEL_ID);
        if (manuscript != CA_DOCK_NONE && ca_dock_layout_group_of(layout, manuscript) == CA_DOCK_NONE &&
            ca_dock_layout_home_of(layout, manuscript) == CA_DOCK_NONE && bottom != CA_DOCK_NONE) {
            const int active = ca_dock_layout_tab_index(layout, ca_dock_layout_active_panel(layout, bottom));
            if (ca_dock_layout_add_tab(layout, bottom, manuscript, -1)) {
                if (active >= 0) ca_dock_layout_set_active(layout, bottom, active);
                changed = true;
            }
        }
        if (changed) {
            size_t size=ca_dock_layout_save(layout,NULL,0);char *text=size ? malloc(size+1) : NULL;
            if (text&&ca_dock_layout_save(layout,text,size+1)==size) {free(workspace->saved_layout);workspace->saved_layout=text;}
            else free(text);
        }
    }
    ca_dock_layout_destroy(layout);
}

static bool create_dock(Fl_Workspace *workspace, Ca_Window *window)
{
    migrate_authoring_layout(workspace);
    workspace->dock = ca_dock_create(window, &(Ca_DockDesc){
        .default_layout = seed_default_layout,
        .saved_layout   = workspace->saved_layout,
        .on_event       = forward_dock_event,
        .event_data     = workspace,
    });
    if (!workspace->dock) return false;
    const bool scene = ca_dock_register_panel(workspace->dock, &(Ca_DockPanelDesc){
        .id = SCENE_PANEL_ID, .title = "Scene", .build = fl_scene_panel_build, .teardown = fl_scene_panel_teardown,
        .user_data = workspace->scene,
    });
    const bool widgets=scene&&ca_dock_register_panel(workspace->dock,&(Ca_DockPanelDesc){
        .id="widgets",.title="Widgets",.dock_beside=SCENE_PANEL_ID,.build=fl_widget_library_build,
        .teardown=fl_widget_library_teardown,.user_data=workspace->widgets});
    const bool assets=widgets&&ca_dock_register_panel(workspace->dock,&(Ca_DockPanelDesc){
        .id=ASSETS_PANEL_ID,.title="Assets",.dock_beside="widgets",.build=fl_asset_panel_build,
        .teardown=fl_asset_panel_teardown,.user_data=workspace->assets});
    const bool script_panel=assets&&ca_dock_register_panel(workspace->dock,&(Ca_DockPanelDesc){
        .id="scripts",.title="Scripts",.dock_beside=ASSETS_PANEL_ID,.build=fl_script_panel_build,
        .teardown=fl_script_panel_teardown,.user_data=workspace->scripts});
    const bool inspector=script_panel&&ca_dock_register_panel(workspace->dock,&(Ca_DockPanelDesc){
        .id="inspector",.title="Inspector",.dock_beside=SCENE_PANEL_ID,.build=fl_inspector_build,
        .teardown=fl_inspector_teardown,.user_data=workspace->inspector});
    const bool timeline = inspector && ca_dock_register_panel(workspace->dock, &(Ca_DockPanelDesc){
        .id = TIMELINE_PANEL_ID, .title = "Timeline", .dock_beside = SCENE_PANEL_ID, .build = fl_timeline_panel_build,
        .teardown = fl_timeline_panel_teardown, .user_data = workspace->timeline,
    });
    const bool console = timeline && ca_dock_register_panel(workspace->dock, &(Ca_DockPanelDesc){
        .id = FL_CONSOLE_PANEL_ID, .title = "Console", .dock_beside = SCENE_PANEL_ID,
        .build = fl_console_build_panel, .teardown = fl_console_teardown_panel,
        .user_data = workspace->console,
    });
    const bool export_panel = console && (!workspace->export_panel || ca_dock_register_panel(workspace->dock, &(Ca_DockPanelDesc){
        .id = FL_EXPORT_PANEL_ID, .title = "Export", .dock_beside = TIMELINE_PANEL_ID,
        .build = fl_export_panel_build, .teardown = fl_export_panel_teardown,
        .user_data = workspace->export_panel,
    }));
    const bool manuscript = export_panel && ca_dock_register_panel(workspace->dock, &(Ca_DockPanelDesc){
        .id = FL_MANUSCRIPT_PANEL_ID, .title = "Manuscript", .dock_beside = TIMELINE_PANEL_ID,
        .build = fl_manuscript_panel_build, .teardown = fl_manuscript_panel_teardown,
        .user_data = workspace->manuscript,
    });
    if (!manuscript) FL_LOG_ERROR(LOG_TAG, "failed to register workspace panels");
    return manuscript;
}

/*
 * OS drop observer: every window of the instance forwards dropped files to
 * the Assets panel, which imports them and places images dropped on the scene.
 */
static void on_files_dropped(Ca_Window *window, const char *const *paths, int count, float x, float y, void *user_data)
{
    (void)window;
    Fl_Workspace *workspace = user_data;
    if (fl_manuscript_panel_drop(workspace->manuscript, paths, count, x, y)) return;
    fl_asset_panel_drop_files(workspace->assets, paths, count, x, y);
}

Fl_Workspace *fl_workspace_create(const Fl_WorkspaceDesc *desc)
{
    if (!desc || !desc->window || !desc->bus || !desc->jobs || !desc->fonts || !desc->project) return NULL;
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
    workspace->fonts=desc->fonts;
    fl_fonts_set_project(workspace->fonts,desc->project);
    workspace->edit=fl_scene_edit_create(desc->project,desc->bus,desc->jobs);
    workspace->importer=fl_asset_importer_create(desc->project,desc->jobs,desc->bus);
    workspace->scene=fl_scene_panel_create(workspace->edit,desc->fonts,desc->jobs,desc->bus);
    workspace->widgets=fl_widget_library_create(workspace->edit,workspace->scene,desc->bus);
    workspace->player=fl_audio_player_create(&(Fl_AudioPlayerDesc){.bus=desc->bus});
    workspace->playback=workspace->edit ? fl_playback_create(&(Fl_PlaybackDesc){.instance=workspace->instance,.edit=workspace->edit,.bus=desc->bus}) : NULL;
    if (workspace->scene && workspace->playback) fl_scene_panel_set_playback(workspace->scene, workspace->playback);
    workspace->timeline=workspace->playback ? fl_timeline_panel_create(workspace->edit,workspace->playback,desc->bus) : NULL;
    workspace->assets=workspace->player&&workspace->timeline ? fl_asset_panel_create(workspace->edit,workspace->scene,workspace->importer,desc->jobs,
                                                                workspace->player,workspace->timeline,desc->bus) : NULL;
    workspace->scripts=workspace->edit ? fl_script_panel_create(workspace->edit,desc->bus) : NULL;
    workspace->speech=fl_speech_create(desc->project, desc->jobs, desc->bus);
    workspace->manuscript=workspace->speech && workspace->edit ? fl_manuscript_panel_create(workspace->edit, workspace->speech, workspace->player, workspace->importer, desc->bus) : NULL;
    workspace->inspector=workspace->playback ? fl_inspector_create(workspace->edit,desc->fonts,workspace->playback,desc->bus) : NULL;
    if (desc->exporter) {
        workspace->export_panel = fl_export_window_create(workspace->instance, desc->project, desc->exporter);
        if (!workspace->export_panel) {
            FL_LOG_ERROR(LOG_TAG, "failed to create the export panel");
            fl_workspace_destroy(workspace);
            return NULL;
        }
    }
    if (!workspace->edit||!workspace->importer||!workspace->scene||!workspace->widgets||!workspace->assets||!workspace->scripts||
        !workspace->manuscript||!workspace->speech||!workspace->inspector||!create_dock(workspace, desc->window)) {
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
    ca_instance_set_file_drop_handler(workspace->instance, on_files_dropped, workspace);
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
    if (workspace->instance) ca_instance_set_file_drop_handler(workspace->instance, NULL, NULL);
    ca_effect_destroy(workspace->saver);
    ca_dock_destroy(workspace->dock);
    fl_manuscript_panel_destroy(workspace->manuscript);
    fl_speech_destroy(workspace->speech);
    fl_export_window_destroy(workspace->export_panel);
    fl_widget_library_destroy(workspace->widgets);
    fl_asset_panel_destroy(workspace->assets);
    fl_script_panel_destroy(workspace->scripts);
    fl_audio_player_destroy(workspace->player);
    fl_timeline_panel_destroy(workspace->timeline);
    fl_inspector_destroy(workspace->inspector);
    fl_scene_panel_destroy(workspace->scene);
    fl_asset_importer_destroy(workspace->importer);
    fl_playback_destroy(workspace->playback);
    fl_scene_edit_destroy(workspace->edit);
    fl_fonts_set_project(workspace->fonts, NULL);
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

bool fl_workspace_show_panel(Fl_Workspace *workspace, const char *id)
{
    if (!workspace || !workspace->mounted || !id || !ca_dock_open_panel(workspace->dock, id)) return false;
    ca_dock_focus_panel(workspace->dock, id);
    return true;
}

Fl_SceneEdit *fl_workspace_edit(const Fl_Workspace *workspace) { return workspace ? workspace->edit : NULL; }
Fl_Playback *fl_workspace_playback(const Fl_Workspace *workspace) { return workspace ? workspace->playback : NULL; }
Fl_Speech *fl_workspace_speech(const Fl_Workspace *workspace) { return workspace ? workspace->speech : NULL; }
