#pragma once
#include "fl_scene_panel.h"
typedef struct Fl_WidgetLibrary Fl_WidgetLibrary;
/* Create a library borrowing the edit controller and scene drop target. */
Fl_WidgetLibrary *fl_widget_library_create(Fl_SceneEdit *edit, Fl_ScenePanel *scene, Fl_EventBus *bus);
/* Free event subscriptions and retained search/action state after dock destruction. */
void fl_widget_library_destroy(Fl_WidgetLibrary *library);
/* Dock callbacks build the searchable registry and detach borrowed window handles. */
void fl_widget_library_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_widget_library_teardown(void *data);
