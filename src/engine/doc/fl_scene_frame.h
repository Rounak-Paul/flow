#pragma once
#include "project/fl_project.h"

/* Camera output rectangle in scene coordinates. */
typedef struct Fl_SceneFrame { double x, y, width, height; } Fl_SceneFrame;

/* Resolve scene's target ratio and persisted camera offset; NULL/invalid dimensions return an empty frame. */
Fl_SceneFrame fl_scene_frame(const Fl_Scene *scene);
