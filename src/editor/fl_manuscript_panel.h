#pragma once
#include "fl_scene_edit.h"
#include "fl_speech.h"
#include <ca_dock.h>
#include <stdbool.h>

#define FL_MANUSCRIPT_PANEL_ID "manuscript"

typedef struct Fl_ManuscriptPanel Fl_ManuscriptPanel;

Fl_ManuscriptPanel *fl_manuscript_panel_create(Fl_SceneEdit *edit, Fl_Speech *speech, Fl_EventBus *bus);
void fl_manuscript_panel_destroy(Fl_ManuscriptPanel *panel);
void fl_manuscript_panel_build(Ca_Dock *dock, Ca_Window *window, void *panel);
void fl_manuscript_panel_teardown(void *panel);
/* True when the drop landed on the voice well, including a file this panel rejected. */
bool fl_manuscript_panel_drop(Fl_ManuscriptPanel *panel, const char *const *paths, int count, float x, float y);
