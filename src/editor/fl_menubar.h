#pragma once

#include "fl_workspace.h"

#include "core/fl_event.h"

#include <causality.h>

/*
 * Title-bar menu strip of the main window.
 *
 * The View menu lists every workspace panel; choosing one toggles it, and a
 * check mark shows which panels are open. The menu rebuilds itself whenever
 * the workspace publishes FL_EVENT_DOCK_PANEL_OPENED / _CLOSED, so it stays
 * correct however a panel was opened or closed.
 */
typedef struct Fl_MenuBar Fl_MenuBar;

/*
 * Install the menu strip on a window.
 *
 * window     Window whose custom title bar receives the menus.
 * bus        Application bus carrying the workspace's dock events.
 * workspace  Workspace whose panels the View menu toggles.
 * project       Borrowed open project for File > Save.
 * projects      Callback requesting deferred project-manager navigation.
 * data          Context passed to projects.
 * export_video  Callback that opens the Export panel.
 * export_data   Context passed to export_video.
 * Returns       The menu bar, or NULL on invalid arguments or failure.
 */
Fl_MenuBar *fl_menubar_create(Ca_Window *window, Fl_EventBus *bus, Fl_Workspace *workspace,
                                 Fl_Project *project, void (*projects)(void *), void *data,
                                 void (*export_video)(void *), void *export_data);

/*
 * Stop observing, remove the menus from the window if it is still open, and
 * free the menu bar. NULL is a no-op.
 */
void fl_menubar_destroy(Fl_MenuBar *menubar);
