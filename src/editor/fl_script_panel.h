#pragma once
#include "fl_scene_edit.h"
#include <ca_dock.h>

#define FL_EVENT_SCRIPT_OPEN "scripts.open"
/* Request to show a script of a scene in the Scripts panel. */
typedef struct Fl_ScriptOpen { Fl_Id scene, script; } Fl_ScriptOpen;

typedef struct Fl_ScriptPanel Fl_ScriptPanel;
/* Create the Scripts panel controller borrowing edit and bus until destroy. */
Fl_ScriptPanel *fl_script_panel_create(Fl_SceneEdit *edit, Fl_EventBus *bus);
/* Release subscriptions and the file browser after dock destruction. */
void fl_script_panel_destroy(Fl_ScriptPanel *panel);
/* Dock callbacks build the script repository view and detach borrowed window handles. */
void fl_script_panel_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_script_panel_teardown(void *data);
/* Ask the panel (through the bus) to select script of scene and come to the front. */
bool fl_script_panel_request(Fl_EventBus *bus, Fl_Id scene, Fl_Id script);
