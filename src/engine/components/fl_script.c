#include "fl_script.h"
#include <stdlib.h>
#include <string.h>

/* Read/write the script header's canonical unsigned integer. */
static uint64_t get_u64(const unsigned char *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void put_u64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

bool fl_script_encode(const Fl_Script *script, Fl_Component *out)
{
    if (!script || !out || !script->source || !script->source_size || script->source_size > FL_SCRIPT_SOURCE_LIMIT ||
        memchr(script->source, 0, script->source_size)) return false;
    unsigned char *bytes = malloc(FL_SCRIPT_HEADER + script->source_size);
    if (!bytes) return false;
    put_u64(bytes, script->enabled);
    memcpy(bytes + FL_SCRIPT_HEADER, script->source, script->source_size);
    *out = (Fl_Component){FL_COMPONENT_SCRIPT, FL_SCRIPT_VERSION, FL_SCRIPT_HEADER + script->source_size, bytes};
    return true;
}

bool fl_script_decode(const Fl_Component *c, Fl_Script *out)
{
    if (!c||!out||c->type!=FL_COMPONENT_SCRIPT||c->version!=FL_SCRIPT_VERSION||!c->data) return false;
    if (c->size<=FL_SCRIPT_HEADER||c->size>FL_SCRIPT_HEADER+FL_SCRIPT_SOURCE_LIMIT) return false;
    uint64_t enabled=get_u64(c->data);
    if (enabled>1||memchr(c->data+FL_SCRIPT_HEADER,0,c->size-FL_SCRIPT_HEADER)) return false;
    *out=(Fl_Script){(const char *)c->data+FL_SCRIPT_HEADER,c->size-FL_SCRIPT_HEADER,enabled!=0};return true;
}

const char *fl_property_name(Fl_AnimProperty property)
{
    static const char *names[FL_PROP_COUNT] = {"x", "y", "width", "height", "rotation", "scale_x", "scale_y",
        "opacity", "fill", "stroke", "stroke_width", "font_size"};
    return property >= 0 && property < FL_PROP_COUNT ? names[property] : NULL;
}
