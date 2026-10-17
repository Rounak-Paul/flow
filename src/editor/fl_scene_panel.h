#pragma once
#include "project/fl_project.h"
#include <ca_dock.h>
typedef struct Fl_ScenePanel Fl_ScenePanel;
/* Create a scene workspace controller borrowing project and bus until destroy. */
Fl_ScenePanel *fl_scene_panel_create(Fl_Project *project, Fl_EventBus *bus);
/* Unsubscribe and free panel state after its dock has been destroyed. */
void fl_scene_panel_destroy(Fl_ScenePanel *panel);
/* Dock callbacks: build the three scene regions and detach borrowed UI handles. */
void fl_scene_panel_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_scene_panel_teardown(void *data);
