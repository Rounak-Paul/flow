#pragma once
#include "project/fl_project.h"
#include "render/gpu/fl_exporter.h"
#include <ca_dock.h>

/*
 * Export panel. Exposes every video-export parameter, then follows the shared exporter with the
 * encoded frames, a full-width progress bar and cancellation. The panel docks with the workspace
 * and can be dragged out into its own window. Closing it leaves a running export alone.
 */
#define FL_EXPORT_PANEL_ID "export"

typedef struct Fl_ExportWindow Fl_ExportWindow;

/* Create panel state bound to project and its shared exporter. NULL on invalid arguments or failure. */
Fl_ExportWindow *fl_export_window_create(Ca_Instance *instance, Fl_Project *project, Fl_Exporter *exporter);
/* Release the panel state. Does not cancel an export. NULL is safe. Call after the dock is gone. */
void fl_export_window_destroy(Fl_ExportWindow *window);

/* Dock callbacks. Build emits the panel; teardown drops widget handles while they still exist. */
void fl_export_panel_build(Ca_Dock *dock, Ca_Window *window, void *user_data);
void fl_export_panel_teardown(void *user_data);
