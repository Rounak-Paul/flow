#pragma once
#include "fl_scene_edit.h"
#include "fl_playback.h"
#include "core/fl_job.h"
#include "text/fl_fonts.h"
#include <ca_dock.h>
typedef struct Fl_ScenePanel Fl_ScenePanel;
/* Insertion point resolved from a drop: viewport points are scene coordinates, hierarchy rows name a parent. */
typedef struct Fl_SceneDropTarget {
    Fl_Id scene, parent;
    double x, y;
    bool at_point;      /* x/y is the pointer position in parent space; false for hierarchy drops */
} Fl_SceneDropTarget;
/* Create a scene workspace controller borrowing edit, fonts, jobs and bus until destroy. */
Fl_ScenePanel *fl_scene_panel_create(Fl_SceneEdit *edit, Fl_Fonts *fonts, Fl_JobSystem *jobs, Fl_EventBus *bus);
/* Show the scene camera in the viewport while playback is running. */
void fl_scene_panel_set_playback(Fl_ScenePanel *panel, Fl_Playback *playback);
/* Unsubscribe and free panel state after its dock has been destroyed. */
void fl_scene_panel_destroy(Fl_ScenePanel *panel);
/* Dock callbacks: build the three scene regions and detach borrowed UI handles. */
void fl_scene_panel_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_scene_panel_teardown(void *data);

/* Resolve global screen coordinates over the hierarchy or artboard; false elsewhere. */
bool fl_scene_panel_drop_target(Fl_ScenePanel *panel, double screen_x, double screen_y, Fl_SceneDropTarget *out);
/* Drop a widget into this panel using global screen coordinates; false outside valid targets. */
bool fl_scene_panel_drop_widget(Fl_ScenePanel *panel, Fl_WidgetKind kind, double screen_x, double screen_y);
