#pragma once
#include "doc/fl_scene_frame.h"

/* Visible world rectangle, viewport-centered target in world coordinates, and pixels per scene unit. */
typedef struct Fl_CanvasMapping { Fl_SceneFrame world, target; double scale; } Fl_CanvasMapping;

/* Fit the persisted target camera into width/height with margin; invalid inputs return zero. */
Fl_CanvasMapping fl_canvas_mapping(Fl_SceneFrame target, double width, double height, double margin);
