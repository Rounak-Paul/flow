#pragma once
#include <stdbool.h>

/* Column-major 4x4. Spatial code uses this as the only hierarchy transform. */
typedef struct Fl_Vec3 { double x, y, z; } Fl_Vec3;
typedef struct Fl_Mat4 { double m[16]; } Fl_Mat4;

Fl_Mat4 fl_mat4_identity(void);
Fl_Mat4 fl_mat4_mul(Fl_Mat4 a, Fl_Mat4 b);
Fl_Vec3 fl_mat4_point(Fl_Mat4 m, Fl_Vec3 p);
Fl_Vec3 fl_mat4_vector(Fl_Mat4 m, Fl_Vec3 v);
bool fl_mat4_inverse(Fl_Mat4 m, Fl_Mat4 *out);
Fl_Mat4 fl_mat4_translation(double x, double y, double z);
Fl_Mat4 fl_mat4_scale(double x, double y, double z);
Fl_Mat4 fl_mat4_rotation_x(double radians);
Fl_Mat4 fl_mat4_rotation_y(double radians);
Fl_Mat4 fl_mat4_rotation_z(double radians);
void fl_mat4_float(Fl_Mat4 m, float out[16]);

/* Y grows downward. NDC matches Vulkan (y = -1 at the top, depth 0..1). */
Fl_Mat4 fl_mat4_ortho(double left, double right, double top, double bottom, double near_z, double far_z);
/* Camera looks down local -Z. fov_y is radians. */
Fl_Mat4 fl_mat4_perspective(double fov_y, double aspect, double near_z, double far_z);
