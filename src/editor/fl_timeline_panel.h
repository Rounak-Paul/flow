#pragma once
#include "fl_playback.h"
#include <ca_dock.h>

/*
 * Timeline panel: transport, ruler with playhead, beat lane, one lane per
 * animated property of the selected objects and one per audio clip. Explicit
 * property keying and per-track navigation edit only their target channel. Edits go through
 * timeline operations; time moves through the playback controller.
 */
typedef struct Fl_TimelinePanel Fl_TimelinePanel;

/* Create the panel controller borrowing edit, playback and bus. NULL on failure. */
Fl_TimelinePanel *fl_timeline_panel_create(Fl_SceneEdit *edit, Fl_Playback *playback, Fl_EventBus *bus);
/* Free subscriptions and the rename window after dock destruction. NULL is safe. */
void fl_timeline_panel_destroy(Fl_TimelinePanel *panel);
/* Dock callbacks: build the timeline and detach borrowed window handles. */
void fl_timeline_panel_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_timeline_panel_teardown(void *data);
/* Place audio asset on the active scene at tick. */
bool fl_timeline_panel_add_clip(Fl_TimelinePanel *panel, Fl_Id asset, Fl_Tick tick);
/* Place audio asset at the time under global screen coordinates; false outside the lanes. */
bool fl_timeline_panel_drop_asset(Fl_TimelinePanel *panel, Fl_Id asset, double screen_x, double screen_y);
