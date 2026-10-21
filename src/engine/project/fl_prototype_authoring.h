#pragma once
#include "fl_project.h"

/* Capture an authored subtree as a reusable asset. Root placement becomes identity; local clocks/keys remain intact.
   Returns the new asset ID. Does not replace or modify the source hierarchy. */
bool fl_prototype_capture(Fl_Project *project, Fl_Id scene, Fl_Id root, const char *name, Fl_Id *asset);
/* Add a live instance under parent at scene-local x/y; returns its persistent owner entity ID. */
bool fl_prototype_instantiate(Fl_Project *project, Fl_Id scene, Fl_Id parent, Fl_Id asset, double x, double y, Fl_Id *entity);
/* Replace an instance's complete explicit override array atomically, retaining its asset, identity and clock. */
bool fl_prototype_set_overrides(Fl_Project *project, Fl_Id scene, Fl_Id entity, const char *overrides, size_t size);
