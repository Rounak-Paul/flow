#include "fl_style.h"
#include "project/fl_project.h"
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

void fl_style_encode(const Fl_Style *s, unsigned char out[FL_STYLE_BYTES])
{
    put_double(out, s->radius); put_double(out + 8, s->gradient_angle); put_double(out + 16, s->shadow_blur);
    put_double(out + 24, s->shadow_x); put_double(out + 32, s->shadow_y);
    memcpy(out + 40, s->gradient_to, 4); memcpy(out + 44, s->shadow_color, 4);
    out[48] = s->gradient ? 1 : 0; out[49] = (unsigned char)s->align;
    for (unsigned i = 0; i < 4; ++i) put_double(out + 50 + i * 8, s->crop[i]);
}

bool fl_style_decode(const Fl_Component *c, Fl_Style *out)
{
    if (!c || !out || c->type != FL_COMPONENT_STYLE || c->version != 1 || c->size != FL_STYLE_BYTES || !c->data) return false;
    Fl_Style s = {.radius = get_double(c->data), .gradient_angle = get_double(c->data + 8), .shadow_blur = get_double(c->data + 16),
                  .shadow_x = get_double(c->data + 24), .shadow_y = get_double(c->data + 32), .gradient = c->data[48] == 1,
                  .align = (Fl_TextAlign)c->data[49]};
    memcpy(s.gradient_to, c->data + 40, 4); memcpy(s.shadow_color, c->data + 44, 4);
    for (unsigned i = 0; i < 4; ++i) s.crop[i] = get_double(c->data + 50 + i * 8);
    if (c->data[48] > 1 || c->data[49] >= FL_ALIGN_COUNT || !isfinite(s.radius) || s.radius < 0 || s.radius > 65536 ||
        !isfinite(s.gradient_angle) || fabs(s.gradient_angle) > 1000 || !isfinite(s.shadow_blur) || s.shadow_blur < 0 || s.shadow_blur > 4096 ||
        !isfinite(s.shadow_x) || !isfinite(s.shadow_y) || fabs(s.shadow_x) > 65536 || fabs(s.shadow_y) > 65536) return false;
    for (unsigned i = 0; i < 4; ++i) if (!isfinite(s.crop[i])) return false;
    if (s.crop[0] < 0 || s.crop[1] < 0 || s.crop[2] < 0.001 || s.crop[3] < 0.001 || s.crop[0] + s.crop[2] > 1 + 1e-9 || s.crop[1] + s.crop[3] > 1 + 1e-9) return false;
    *out = s;
    return true;
}

Fl_Style fl_style_default(void)
{
    return (Fl_Style){.crop = {0, 0, 1, 1}};
}

Fl_Style fl_entity_style(const Fl_Entity *e)
{
    Fl_Style s = fl_style_default();
    fl_style_decode(fl_entity_component(e, FL_COMPONENT_STYLE), &s);
    return s;
}
