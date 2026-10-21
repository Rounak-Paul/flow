#pragma once
#include "fl_component.h"
#include "doc/fl_math.h"

#define FL_POSE_BYTES 96u
#define FL_BOUNDS_BYTES 24u

/* Local translation, Euler radians (XYZ, applied Z then Y then X so rz is the planar spin), scale and pivot.
   Pivot is in the same local units as the vertices: pixels for flat shapes, -0.5..0.5 for meshes. */
typedef struct Fl_Pose {
    double x, y, z;
    double rx, ry, rz;
    double sx, sy, sz;
    double px, py, pz;
} Fl_Pose;

/* Box extent. Flat shapes use width and height as the local pixel box. Meshes scale a unit primitive by all three. */
typedef struct Fl_Bounds { double width, height, depth; } Fl_Bounds;

Fl_Pose fl_pose_identity(void);
/* Map local pixels (top-left origin) into parent space. */
Fl_Mat4 fl_pose_model(const Fl_Pose *pose);
/* Map a centred unit mesh (-0.5..0.5) into parent space, scaled by bounds. */
Fl_Mat4 fl_mesh_model(const Fl_Pose *pose, const Fl_Bounds *bounds);

void fl_pose_encode(const Fl_Pose *pose, unsigned char out[FL_POSE_BYTES]);
bool fl_pose_decode(const Fl_Component *component, Fl_Pose *out);
void fl_bounds_encode(const Fl_Bounds *bounds, unsigned char out[FL_BOUNDS_BYTES]);
bool fl_bounds_decode(const Fl_Component *component, Fl_Bounds *out);
/* Read bounds, or a zero box when the component is absent. */
Fl_Bounds fl_entity_bounds(const Fl_Entity *entity);
