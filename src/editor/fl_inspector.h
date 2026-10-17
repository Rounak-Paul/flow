#pragma once
#include "fl_scene_edit.h"
#include "text/fl_fonts.h"
#include <ca_dock.h>
typedef struct Fl_Inspector Fl_Inspector;
/* Create a selection-driven Inspector borrowing edit, fonts and bus until destroy. */
Fl_Inspector *fl_inspector_create(Fl_SceneEdit *edit, Fl_Fonts *fonts, Fl_EventBus *bus);
/* Release retained drafts and subscriptions after dock destruction. */
void fl_inspector_destroy(Fl_Inspector *inspector);
/* Dock callbacks build schema-controlled fields and detach transient UI handles. */
void fl_inspector_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_inspector_teardown(void *data);
