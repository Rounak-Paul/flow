#pragma once
#include "fl_playback.h"
#include "text/fl_fonts.h"
#include <ca_dock.h>
typedef struct Fl_Inspector Fl_Inspector;
/* Create a selection-driven Inspector borrowing edit, fonts, playback and bus until destroy.
   Fields show values at the current time; edits of animated properties are keyed there. */
Fl_Inspector *fl_inspector_create(Fl_SceneEdit *edit, Fl_Fonts *fonts, Fl_Playback *playback, Fl_EventBus *bus);
/* Release retained drafts and subscriptions after dock destruction. */
void fl_inspector_destroy(Fl_Inspector *inspector);
/* Dock callbacks build schema-controlled fields and detach transient UI handles. */
void fl_inspector_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_inspector_teardown(void *data);
