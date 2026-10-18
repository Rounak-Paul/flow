#pragma once
#include "fl_component.h"

/* Local position, extent, rotation in radians and scale relative to the parent. */
typedef struct Fl_Transform { double x, y, width, height, rotation, scale_x, scale_y; } Fl_Transform;

/* Encode transform into 56 canonical little-endian bytes. */
void fl_transform_encode(const Fl_Transform *transform, unsigned char out[56]);
/* Decode a version-one transform component into out; reject invalid values. */
bool fl_transform_decode(const Fl_Component *component, Fl_Transform *out);
