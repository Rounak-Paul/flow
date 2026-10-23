#pragma once

#include "core/fl_event.h"
#include "core/fl_job.h"
#include "project/fl_project.h"
#include "text/fl_fonts.h"
#include "fl_scene_edit.h"
#include "fl_playback.h"

#include <ca_dock.h>
#include <causality.h>

#include <stdbool.h>

typedef struct Fl_Exporter Fl_Exporter;
typedef struct Fl_Speech Fl_Speech;

/*
 * Editor workspace — the dockable panel layout of the main window.
 *
 * Owns the causality dock and Flow's panels (Scene, the central group,
 * Widgets, Assets, Scripts, Inspector, Timeline, Manuscript, Console and Export), the project's asset importer and
 * the OS file-drop handler; seeds the default arrangement and persists the
 * user's layout. Dock changes are published on
 * the application bus (FL_EVENT_DOCK_*), and persistence itself observes
 * FL_EVENT_DOCK_LAYOUT_CHANGED, saving shortly after the layout settles.
 */
typedef struct Fl_Workspace Fl_Workspace;

/* Bus events; payload is Fl_DockEventPayload. */
#define FL_EVENT_DOCK_PANEL_OPENED    "dock.panel_opened"
#define FL_EVENT_DOCK_PANEL_CLOSED    "dock.panel_closed"
#define FL_EVENT_DOCK_PANEL_ACTIVATED "dock.panel_activated"
#define FL_EVENT_DOCK_LAYOUT_CHANGED  "dock.layout_changed"

typedef struct Fl_DockEventPayload {
    char panel_id[CA_DOCK_ID_MAX];   /* empty for FL_EVENT_DOCK_LAYOUT_CHANGED */
} Fl_DockEventPayload;

typedef struct Fl_WorkspaceDesc {
    Fl_Project  *project;       /* open project; required */
    Ca_Window   *window;        /* main window; required                     */
    Fl_EventBus *bus;           /* application bus; required                 */
    Fl_JobSystem *jobs;         /* worker pool for imports and decoding; required */
    Fl_Fonts    *fonts;         /* font service bound to project; required   */
    const char  *layout_path;   /* persisted layout file, or NULL to disable */
    Fl_Exporter *exporter;      /* shared video exporter, or NULL to omit Export */
} Fl_WorkspaceDesc;

/*
 * Create the workspace and register its panels and events. Must run on the
 * bus owner (UI) thread.
 *
 * Returns  The workspace, or NULL on invalid arguments or failure.
 */
Fl_Workspace *fl_workspace_create(const Fl_WorkspaceDesc *desc);

/*
 * Emit the dock into the current container of the main window's build
 * context, restoring the persisted layout when available.
 *
 * Returns  false when the dock cannot be mounted.
 */
bool fl_workspace_mount(Fl_Workspace *workspace);

/*
 * Persist the current layout immediately. No-op without a layout path.
 *
 * Returns  true when the layout was written (or persistence is disabled);
 *          false before the workspace is mounted or on write failure.
 */
bool fl_workspace_save(Fl_Workspace *workspace);

/*
 * Save, tear down the dock (closing floating windows) and free the
 * workspace. Must run while the main window is still alive. NULL is a no-op.
 */
void fl_workspace_destroy(Fl_Workspace *workspace);

/* Number of panels the workspace offers. */
int fl_workspace_panel_count(const Fl_Workspace *workspace);

/* Stable id of panel `index`, or NULL. */
const char *fl_workspace_panel_id(const Fl_Workspace *workspace, int index);

/* Display title of panel `index`, or NULL. */
const char *fl_workspace_panel_title(const Fl_Workspace *workspace, int index);

/* Report whether a panel is currently part of the layout. */
bool fl_workspace_panel_visible(const Fl_Workspace *workspace, const char *id);

/*
 * Show a hidden panel (back in its remembered place) or close a shown one.
 *
 * Returns  true when the panel is visible afterwards.
 */
bool fl_workspace_toggle_panel(Fl_Workspace *workspace, const char *id);

/*
 * Show a panel in its group, or dock it beside its default neighbour, and focus it.
 * Dragging its tab floats it into its own window.
 *
 * Returns  false when the workspace is not mounted or the panel is unknown.
 */
bool fl_workspace_show_panel(Fl_Workspace *workspace, const char *id);

/* Borrow the workspace's shared scene edit controller (selection, active scene, time). */
Fl_SceneEdit *fl_workspace_edit(const Fl_Workspace *workspace);
/* Borrow the playback controller. */
Fl_Playback *fl_workspace_playback(const Fl_Workspace *workspace);
/* Borrow the local Qwen3-TTS speech job, or NULL. */
Fl_Speech *fl_workspace_speech(const Fl_Workspace *workspace);
