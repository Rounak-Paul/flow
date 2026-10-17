#pragma once
#include "doc/fl_scene_geometry.h"

typedef struct Fl_SceneEdit Fl_SceneEdit;
#define FL_EVENT_SELECTION_CHANGED "editor.selection_changed"
/* Selection notifications carry the edit controller pointer. */
typedef struct Fl_SelectionChange { Fl_SceneEdit *edit; } Fl_SelectionChange;
typedef enum Fl_EditGesture { FL_GESTURE_MOVE, FL_GESTURE_NW, FL_GESTURE_NE, FL_GESTURE_SE, FL_GESTURE_SW, FL_GESTURE_ROTATE } Fl_EditGesture;
/* Create the shared selection/gesture owner, borrowing project and bus. */
Fl_SceneEdit *fl_scene_edit_create(Fl_Project *project, Fl_EventBus *bus);
/* Release selection and gesture state after all borrowing panels are destroyed. */
void fl_scene_edit_destroy(Fl_SceneEdit *edit);
/* Read borrowed project, active scene, primary selection and selection membership. */
Fl_Project *fl_scene_edit_project(const Fl_SceneEdit *edit);
Fl_Id fl_scene_edit_scene(const Fl_SceneEdit *edit);
Fl_Id fl_scene_edit_primary(const Fl_SceneEdit *edit);
size_t fl_scene_edit_count(const Fl_SceneEdit *edit);
bool fl_scene_edit_contains(const Fl_SceneEdit *edit, Fl_Id entity);
/* Select a scene/entity; additive toggles membership. Entity zero clears selection. */
bool fl_scene_edit_select(Fl_SceneEdit *edit, Fl_Id scene, Fl_Id entity, bool additive);
/* Borrow current resolved scene geometry, or NULL on allocation failure. */
const Fl_SceneGeometry *fl_scene_edit_geometry(Fl_SceneEdit *edit);
/* Capture a gesture at scene x/y; selected descendants move with selected ancestors once. */
bool fl_scene_edit_begin(Fl_SceneEdit *edit, Fl_EditGesture mode, double x, double y);
/* Apply the captured gesture at scene x/y as one validated transform operation. */
bool fl_scene_edit_update(Fl_SceneEdit *edit, double x, double y);
/* End a gesture; cancel restores its initial transforms if no external mutation intervened. */
void fl_scene_edit_end(Fl_SceneEdit *edit, bool cancel);
/* Create a widget at a scene-local point under parent and select it. */
bool fl_scene_edit_insert(Fl_SceneEdit *edit, Fl_WidgetKind kind, Fl_Id parent, double x, double y);
/* Create an image of asset fitted inside the scene at a parent-local point (its centre when centred,
   else its top-left corner) and select it. */
bool fl_scene_edit_insert_image(Fl_SceneEdit *edit, Fl_Id asset, Fl_Id parent, double x, double y, bool centred);
