#pragma once
#include "project/fl_project.h"

/* Camera output rectangle in scene coordinates. */
typedef struct Fl_SceneFrame { double x, y, width, height; } Fl_SceneFrame;

/* The first visible camera's picture, centered on the canvas. No camera aspect keeps the whole canvas. */
Fl_SceneFrame fl_scene_frame(const Fl_Scene *scene);
