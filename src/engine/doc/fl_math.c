#include "fl_math.h"
#include <math.h>
#include <string.h>

Fl_Mat4 fl_mat4_identity(void)
{
    Fl_Mat4 m = {0};
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1;
    return m;
}

Fl_Mat4 fl_mat4_mul(Fl_Mat4 a, Fl_Mat4 b)
{
    Fl_Mat4 o = {0};
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
        double s = 0;
        for (int k = 0; k < 4; ++k) s += a.m[k * 4 + r] * b.m[c * 4 + k];
        o.m[c * 4 + r] = s;
    }
    return o;
}

Fl_Vec3 fl_mat4_point(Fl_Mat4 m, Fl_Vec3 p)
{
    double x = m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12];
    double y = m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13];
    double z = m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14];
    double w = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15];
    if (w != 0 && w != 1) {x /= w; y /= w; z /= w;}
    return (Fl_Vec3){x, y, z};
}

Fl_Vec3 fl_mat4_vector(Fl_Mat4 m, Fl_Vec3 v)
{
    return (Fl_Vec3){m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z,
                     m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
                     m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z};
}

bool fl_mat4_inverse(Fl_Mat4 src, Fl_Mat4 *out)
{
    if (!out) return false;
    double a[4][8];
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
        a[r][c] = src.m[c * 4 + r];
        a[r][c + 4] = r == c ? 1 : 0;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) if (fabs(a[r][col]) > fabs(a[pivot][col])) pivot = r;
        if (fabs(a[pivot][col]) < 1e-12) return false;
        if (pivot != col) for (int k = 0; k < 8; ++k) {double t = a[col][k]; a[col][k] = a[pivot][k]; a[pivot][k] = t;}
        double div = a[col][col];
        for (int k = 0; k < 8; ++k) a[col][k] /= div;
        for (int r = 0; r < 4; ++r) if (r != col) {
            double f = a[r][col];
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[col][k];
        }
    }
    Fl_Mat4 inv = {0};
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
        inv.m[c * 4 + r] = a[r][c + 4];
        if (!isfinite(inv.m[c * 4 + r])) return false;
    }
    *out = inv;
    return true;
}

Fl_Mat4 fl_mat4_translation(double x, double y, double z)
{
    Fl_Mat4 m = fl_mat4_identity();
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

Fl_Mat4 fl_mat4_scale(double x, double y, double z)
{
    Fl_Mat4 m = {0};
    m.m[0] = x; m.m[5] = y; m.m[10] = z; m.m[15] = 1;
    return m;
}

static Fl_Mat4 rotation_axis(int axis, double radians)
{
    double c = cos(radians), s = sin(radians);
    Fl_Mat4 m = fl_mat4_identity();
    int a = (axis + 1) % 3, b = (axis + 2) % 3;
    m.m[a * 4 + a] = c; m.m[b * 4 + a] = -s;
    m.m[a * 4 + b] = s; m.m[b * 4 + b] = c;
    return m;
}

Fl_Mat4 fl_mat4_rotation_x(double radians) {return rotation_axis(0, radians);}
Fl_Mat4 fl_mat4_rotation_y(double radians) {return rotation_axis(1, radians);}
Fl_Mat4 fl_mat4_rotation_z(double radians) {return rotation_axis(2, radians);}

void fl_mat4_float(Fl_Mat4 m, float out[16])
{
    for (int i = 0; i < 16; ++i) out[i] = (float)m.m[i];
}

Fl_Mat4 fl_mat4_ortho(double left, double right, double top, double bottom, double near_z, double far_z)
{
    Fl_Mat4 m = {0};
    double w = right - left, h = bottom - top, d = far_z - near_z;
    m.m[0] = w != 0 ? 2 / w : 0;
    m.m[5] = h != 0 ? 2 / h : 0;
    m.m[10] = d != 0 ? 1 / d : 0;
    m.m[12] = w != 0 ? -1 - left * 2 / w : 0;
    m.m[13] = h != 0 ? -1 - top * 2 / h : 0;
    m.m[14] = d != 0 ? -near_z / d : 0;
    m.m[15] = 1;
    return m;
}

Fl_Mat4 fl_mat4_perspective(double fov_y, double aspect, double near_z, double far_z)
{
    Fl_Mat4 m = {0};
    double f = 1 / tan(fov_y * 0.5);
    m.m[0] = aspect != 0 ? f / aspect : 0;
    m.m[5] = f;
    m.m[10] = far_z / (near_z - far_z);
    m.m[11] = -1;
    m.m[14] = near_z * far_z / (near_z - far_z);
    return m;
}
