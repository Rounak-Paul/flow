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
/* Convert window pixels to scene pixels; allow_outside supports dragging beyond the artboard. */
bool fl_scene_preview_point(Fl_ScenePreview *preview, Ca_Viewport *viewport, double window_x, double window_y,
                            bool allow_outside, double *scene_x, double *scene_y);
/* Return a handle gesture at scene x/y, or FL_GESTURE_MOVE when no handle is hit. */
Fl_EditGesture fl_scene_preview_handle(Fl_ScenePreview *preview, Ca_Viewport *viewport, double x, double y);
/* Wait for decoding and GPU work, then release renderer-owned resources before edit/instance destruction. */
void fl_scene_preview_destroy(Fl_ScenePreview *preview);
