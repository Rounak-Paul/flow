#pragma once
#include "project/fl_project.h"
#include <causality.h>
typedef struct Fl_ScenePreview Fl_ScenePreview;
/* Create a GPU scene preview borrowing project until destroy; NULL on allocation failure. */
Fl_ScenePreview *fl_scene_preview_create(Fl_Project *project);
/* Select scene by stable id and resolve inherited transforms; call after document changes. */
bool fl_scene_preview_set_scene(Fl_ScenePreview *preview, Fl_Id scene);
/* Record the selected scene into viewport; data is the owning preview controller. */
void fl_scene_preview_render(Ca_Viewport *viewport, void *data);
/* Wait for outstanding GPU use and free cached transforms/pipelines; NULL is safe. */
void fl_scene_preview_destroy(Fl_ScenePreview *preview);
