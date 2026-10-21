#include "fl_transform.h"
#include <math.h>
#include <string.h>

Fl_Pose fl_pose_identity(void) {return (Fl_Pose){.sx = 1, .sy = 1, .sz = 1};}

static void put_doubles(unsigned char *out, const double *values, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint64_t bits; memcpy(&bits, &values[i], 8);
        for (unsigned b = 0; b < 8; ++b) out[i * 8 + b] = (unsigned char)(bits >> (b * 8));
    }
}

static bool get_doubles(const unsigned char *in, double *values, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint64_t bits = 0;
        for (unsigned b = 0; b < 8; ++b) bits |= (uint64_t)in[i * 8 + b] << (b * 8);
        memcpy(&values[i], &bits, 8);
        if (!isfinite(values[i])) return false;
    }
    return true;
}

Fl_Mat4 fl_pose_model(const Fl_Pose *pose)
{
    Fl_Pose p = pose ? *pose : fl_pose_identity();
    Fl_Mat4 t = fl_mat4_translation(p.x, p.y, p.z);
    Fl_Mat4 r = fl_mat4_mul(fl_mat4_rotation_z(p.rz), fl_mat4_mul(fl_mat4_rotation_y(p.ry), fl_mat4_rotation_x(p.rx)));
    Fl_Mat4 s = fl_mat4_scale(p.sx, p.sy, p.sz);
    Fl_Mat4 pivot = fl_mat4_translation(-p.px, -p.py, -p.pz);
    return fl_mat4_mul(t, fl_mat4_mul(r, fl_mat4_mul(s, pivot)));
}

Fl_Mat4 fl_mesh_model(const Fl_Pose *pose, const Fl_Bounds *bounds)
{
    Fl_Pose p = pose ? *pose : fl_pose_identity();
    Fl_Bounds b = bounds ? *bounds : (Fl_Bounds){1, 1, 1};
    Fl_Mat4 t = fl_mat4_translation(p.x, p.y, p.z);
    Fl_Mat4 r = fl_mat4_mul(fl_mat4_rotation_z(p.rz), fl_mat4_mul(fl_mat4_rotation_y(p.ry), fl_mat4_rotation_x(p.rx)));
    Fl_Mat4 s = fl_mat4_scale(p.sx * b.width, p.sy * b.height, p.sz * b.depth);
    Fl_Mat4 pivot = fl_mat4_translation(-p.px, -p.py, -p.pz);
    return fl_mat4_mul(t, fl_mat4_mul(r, fl_mat4_mul(s, pivot)));
}

void fl_pose_encode(const Fl_Pose *pose, unsigned char out[FL_POSE_BYTES])
{
    Fl_Pose p = pose ? *pose : fl_pose_identity();
    const double values[12] = {p.x, p.y, p.z, p.rx, p.ry, p.rz, p.sx, p.sy, p.sz, p.px, p.py, p.pz};
    put_doubles(out, values, 12);
}

bool fl_pose_decode(const Fl_Component *c, Fl_Pose *out)
{
    if (!c || !out || c->type != FL_COMPONENT_POSE || c->version != 1 || c->size != FL_POSE_BYTES || !c->data) return false;
    double v[12];
    if (!get_doubles(c->data, v, 12)) return false;
    *out = (Fl_Pose){v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]};
    return true;
}

void fl_bounds_encode(const Fl_Bounds *bounds, unsigned char out[FL_BOUNDS_BYTES])
{
    Fl_Bounds b = bounds ? *bounds : (Fl_Bounds){0};
    const double values[3] = {b.width, b.height, b.depth};
    put_doubles(out, values, 3);
}

bool fl_bounds_decode(const Fl_Component *c, Fl_Bounds *out)
{
    if (!c || !out || c->type != FL_COMPONENT_BOUNDS || c->version != 1 || c->size != FL_BOUNDS_BYTES || !c->data) return false;
    double v[3];
    if (!get_doubles(c->data, v, 3) || v[0] < 0 || v[1] < 0 || v[2] < 0) return false;
    *out = (Fl_Bounds){v[0], v[1], v[2]};
    return true;
}

Fl_Bounds fl_entity_bounds(const Fl_Entity *entity)
{
    Fl_Bounds bounds = {0};
    fl_bounds_decode(fl_entity_component(entity, FL_COMPONENT_BOUNDS), &bounds);
    return bounds;
}
