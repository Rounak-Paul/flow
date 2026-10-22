#pragma once
#include "doc/fl_scene_geometry.h"
#include "core/fl_job.h"

typedef struct Fl_SceneEdit Fl_SceneEdit;
#define FL_EVENT_SELECTION_CHANGED "editor.selection_changed"
#define FL_EVENT_TIME_CHANGED "editor.time_changed"
#define FL_EVENT_SNAP_CHANGED "editor.snap_changed"
#define FL_SCENE_GRID_SPACING 64.0
typedef struct Fl_SnapChange { Fl_SceneEdit *edit; } Fl_SnapChange;
/* Selection notifications carry the edit controller pointer. */
typedef struct Fl_SelectionChange { Fl_SceneEdit *edit; } Fl_SelectionChange;
/* Current-time notifications carry the controller and the new scene time. */
typedef struct Fl_TimeChange { Fl_SceneEdit *edit; Fl_Tick tick; } Fl_TimeChange;
typedef enum Fl_EditGesture {
    FL_GESTURE_MOVE, FL_GESTURE_NW, FL_GESTURE_NE, FL_GESTURE_SE, FL_GESTURE_SW, FL_GESTURE_ROTATE,
    FL_GESTURE_AXIS_X, FL_GESTURE_AXIS_Y, FL_GESTURE_AXIS_Z,
    FL_GESTURE_RING_X, FL_GESTURE_RING_Y, FL_GESTURE_RING_Z
} Fl_EditGesture;
/* Create the shared selection/gesture owner. jobs enables asynchronous scripted previews; NULL samples inline. */
Fl_SceneEdit *fl_scene_edit_create(Fl_Project *project, Fl_EventBus *bus, Fl_JobSystem *jobs);
/* Release selection and gesture state after all borrowing panels are destroyed. */
void fl_scene_edit_destroy(Fl_SceneEdit *edit);
/* Read borrowed project, active scene, primary selection and selection membership. */
Fl_Project *fl_scene_edit_project(const Fl_SceneEdit *edit);
Fl_Id fl_scene_edit_scene(const Fl_SceneEdit *edit);
Fl_Id fl_scene_edit_primary(const Fl_SceneEdit *edit);
size_t fl_scene_edit_count(const Fl_SceneEdit *edit);
bool fl_scene_edit_contains(const Fl_SceneEdit *edit, Fl_Id entity);
/* Read or change grid snapping for authoring gestures and insertion; does not dirty the project. */
bool fl_scene_edit_snap(const Fl_SceneEdit *edit);
void fl_scene_edit_set_snap(Fl_SceneEdit *edit, bool enabled);
/* Select a scene/entity; additive toggles membership. Entity zero clears selection. */
bool fl_scene_edit_select(Fl_SceneEdit *edit, Fl_Id scene, Fl_Id entity, bool additive);
/* Current scene time; edits of animated properties are keyed here. */
Fl_Tick fl_scene_edit_time(const Fl_SceneEdit *edit);
/* Move the current time (clamped to the active scene's length), publishing FL_EVENT_TIME_CHANGED on change. */
void fl_scene_edit_set_time(Fl_SceneEdit *edit, Fl_Tick tick);
/* Borrow the active scene evaluated at the current time, or NULL on allocation failure. */
const Fl_SceneState *fl_scene_edit_state(Fl_SceneEdit *edit);
/* Whether scripted preview work is pending; the last complete frame remains available. */
bool fl_scene_edit_evaluating(const Fl_SceneEdit *edit);
/* Borrow current resolved scene geometry at the current time, or NULL on allocation failure. */
const Fl_SceneGeometry *fl_scene_edit_geometry(Fl_SceneEdit *edit);
/* Capture a gesture at scene x/y; selected descendants move with selected ancestors once. */
bool fl_scene_edit_begin(Fl_SceneEdit *edit, Fl_EditGesture mode, double x, double y);
/* Apply the captured gesture at scene x/y as one validated transform operation. */
bool fl_scene_edit_update(Fl_SceneEdit *edit, double x, double y);
/* Apply an axis or ring drag. world_delta is the world translation from the press. A ring uses world_axis and radians, measured from the press. */
bool fl_scene_edit_update_spatial(Fl_SceneEdit *edit, Fl_Vec3 world_delta, Fl_Vec3 world_axis, double radians);
/* End a gesture; cancel restores its initial transforms if no external mutation intervened. */
void fl_scene_edit_end(Fl_SceneEdit *edit, bool cancel);
/* Create a widget at a scene-local point under parent and select it. */
bool fl_scene_edit_insert(Fl_SceneEdit *edit, Fl_WidgetKind kind, Fl_Id parent, double x, double y);
/* Create an image or video entity of asset fitted inside the scene at a parent-local point (its centre when
   centred, else its top-left corner) and select it. */
bool fl_scene_edit_insert_media(Fl_SceneEdit *edit, Fl_Id asset, Fl_Id parent, double x, double y, bool centred);

/* Hit-test evaluated content and resolve generated children to their persistent instance owner. */
Fl_Id fl_scene_edit_pick(Fl_SceneEdit *edit, double x, double y, double tolerance);
/* Hit-test along a world ray through the same content. */
Fl_Id fl_scene_edit_pick_ray(Fl_SceneEdit *edit, Fl_Vec3 origin, Fl_Vec3 direction);

/* Borrow the editor's notification-enabled worker pool for document tools. */
Fl_JobSystem *fl_scene_edit_jobs(const Fl_SceneEdit *edit);

/* Focus a displayed generated child while retaining its persistent owner for hierarchy operations. */
Fl_Id fl_scene_edit_focus(const Fl_SceneEdit *edit);
/* Borrow the focused native local timeline, or the authored root timeline. */
const Fl_Scene *fl_scene_edit_timeline(Fl_SceneEdit *edit);
/* Read focused scope time or translate a local seek back through its parent clocks. */
Fl_Tick fl_scene_edit_local_time(Fl_SceneEdit *edit);
bool fl_scene_edit_root_time(Fl_SceneEdit *edit, Fl_Tick local_tick, Fl_Tick *root_tick);
/* Apply a root or scoped native timeline operation through shared document history. */
bool fl_scene_edit_timeline_apply(Fl_SceneEdit *edit, const Fl_Op *operation, Fl_Id *created);
