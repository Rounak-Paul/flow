#include "fl_transform.h"
#include <math.h>
#include <string.h>

void fl_transform_encode(const Fl_Transform *t, unsigned char out[56])
{
    const double values[7] = {t->x, t->y, t->width, t->height, t->rotation, t->scale_x, t->scale_y};
    for (size_t i = 0; i < 7; ++i) {
        uint64_t bits; memcpy(&bits, &values[i], 8);
        for (unsigned b = 0; b < 8; ++b) out[i * 8 + b] = (unsigned char)(bits >> (b * 8));
    }
}

bool fl_transform_decode(const Fl_Component *c, Fl_Transform *out)
{
    if (!c || !out || c->type != FL_COMPONENT_TRANSFORM || c->version != 1 || c->size != 56 || !c->data) return false;
    double v[7];
    for (size_t i = 0; i < 7; ++i) {
        uint64_t bits = 0;
        for (unsigned b = 0; b < 8; ++b) bits |= (uint64_t)c->data[i * 8 + b] << (b * 8);
        memcpy(&v[i], &bits, 8);
        if (!isfinite(v[i])) return false;
    }
    if (v[2] < 0 || v[3] < 0) return false;
    *out = (Fl_Transform){v[0], v[1], v[2], v[3], v[4], v[5], v[6]}; return true;
}

