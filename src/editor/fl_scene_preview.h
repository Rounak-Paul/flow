#pragma once
#include "fl_scene_edit.h"
#include "core/fl_job.h"
#include "text/fl_fonts.h"
#include <causality.h>
typedef struct Fl_ScenePreview Fl_ScenePreview;
/* Request a redraw of the viewport hosting the preview (e.g. after an image finished decoding). */
typedef void (*Fl_PreviewInvalidateFn)(void *user_data);
typedef struct Fl_ScenePreviewDesc {
    Fl_SceneEdit *edit;             /* required; borrowed */
    Fl_Fonts *fonts;                /* required; borrowed */
    Fl_JobSystem *jobs;             /* required; image decoding runs here */
    Fl_EventBus *bus;               /* required; job completion is observed here */
    Fl_PreviewInvalidateFn invalidate;
    void *invalidate_data;
} Fl_ScenePreviewDesc;
/* Create a preview borrowing every described service until destroy. */
Fl_ScenePreview *fl_scene_preview_create(const Fl_ScenePreviewDesc *desc);
/* Record scene primitives and selection handles into viewport; data is the preview. */
void fl_scene_preview_render(Ca_Viewport *viewport, void *data);
/* Truck the editor camera in the view plane. dx/dy are logical pixels; positive dx moves the view right. */
void fl_scene_preview_pan(Fl_ScenePreview *preview, Ca_Viewport *viewport, double dx, double dy);
/* Dolly the editor camera. Positive dy moves closer. Ignored while a shot camera is showing. */
void fl_scene_preview_zoom(Fl_ScenePreview *preview, Ca_Viewport *viewport, double dy);
/* Yaw and pitch the editor camera. dx/dy are logical pixels. */
void fl_scene_preview_look(Fl_ScenePreview *preview, Ca_Viewport *viewport, double dx, double dy);
/* Fly the editor camera. forward, right and up are axes in camera space, typically -1, 0 or 1. */
void fl_scene_preview_fly(Fl_ScenePreview *preview, Ca_Viewport *viewport, double forward, double right, double up);
/* While shot is set, the viewport shows the scene's camera instead of the editor camera. */
void fl_scene_preview_set_shot(Fl_ScenePreview *preview, bool shot);
/* True while playback is showing the scene camera. */
bool fl_scene_preview_shot(const Fl_ScenePreview *preview);
/* Put the editor camera back in front of the output gate. */
void fl_scene_preview_reset_view(Fl_ScenePreview *preview);
/* Convert window pixels to scene pixels on the z = 0 plane; allow_outside supports dragging beyond the artboard. */
bool fl_scene_preview_point(Fl_ScenePreview *preview, Ca_Viewport *viewport, double window_x, double window_y,
                            bool allow_outside, double *scene_x, double *scene_y);
/* Hit-test the picture under a window pixel, through the camera when one is active. */
Fl_Id fl_scene_preview_pick(Fl_ScenePreview *preview, Ca_Viewport *viewport, double window_x, double window_y);
/* Return a handle gesture at window x/y, or FL_GESTURE_MOVE when no handle is hit. */
Fl_EditGesture fl_scene_preview_handle(Fl_ScenePreview *preview, Ca_Viewport *viewport, double window_x, double window_y);
/* Wait for decoding and GPU work, then release renderer-owned resources before edit/instance destruction. */
void fl_scene_preview_destroy(Fl_ScenePreview *preview);
