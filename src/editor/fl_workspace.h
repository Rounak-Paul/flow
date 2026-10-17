#pragma once

#include "core/fl_event.h"

#include <ca_dock.h>
#include <causality.h>

#include <stdbool.h>

/*
 * Editor workspace — the dockable panel layout of the main window.
 *
 * Owns the causality dock, registers Flow's panels, seeds the default
 * arrangement and persists the user's layout. Dock changes are published on
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
    Ca_Window   *window;        /* main window; required                     */
    Fl_EventBus *bus;           /* application bus; required                 */
    const char  *layout_path;   /* persisted layout file, or NULL to disable */
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
