#include "fl_motion.h"
#include <math.h>
#include <string.h>

static void put_double(unsigned char *out, double value)
{
    uint64_t bits; memcpy(&bits, &value, 8);
    for (unsigned i = 0; i < 8; ++i) out[i] = (unsigned char)(bits >> (8 * i));
}
static double get_double(const unsigned char *in)
{
    uint64_t bits = 0; double value;
    for (unsigned i = 0; i < 8; ++i) bits |= (uint64_t)in[i] << (8 * i);
    memcpy(&value, &bits, 8);
    return value;
}

Fl_TextMotion fl_motion_default(void) { return (Fl_TextMotion){.duration = 0.35}; }

void fl_motion_encode(const Fl_TextMotion *motion, unsigned char out[FL_MOTION_BYTES])
{
    Fl_TextMotion m = motion ? *motion : fl_motion_default();
    put_double(out, m.tracking); put_double(out + 8, m.stagger); put_double(out + 16, m.rise);
    put_double(out + 24, m.fade); put_double(out + 32, m.duration);
}

bool fl_motion_decode(const Fl_Component *c, Fl_TextMotion *out)
{
    if (!c || !out || c->type != FL_COMPONENT_TEXT_MOTION || c->version != 1 || c->size != FL_MOTION_BYTES || !c->data) return false;
    Fl_TextMotion m = {get_double(c->data), get_double(c->data + 8), get_double(c->data + 16), get_double(c->data + 24), get_double(c->data + 32)};
    if (!isfinite(m.tracking) || fabs(m.tracking) > 512 || !isfinite(m.stagger) || m.stagger < 0 || m.stagger > 10 ||
        !isfinite(m.rise) || fabs(m.rise) > 2000 || !isfinite(m.fade) || m.fade < 0 || m.fade > 1 ||
        !isfinite(m.duration) || m.duration < 0 || m.duration > 30) return false;
    *out = m;
    return true;
}

bool fl_motion_active(const Fl_TextMotion *m)
{
    return m && (m->tracking != 0 || m->stagger != 0 || m->rise != 0 || m->fade != 0);
}
