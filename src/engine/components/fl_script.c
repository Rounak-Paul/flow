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
    if (!script || !out || !script->script) return false;
    unsigned char *bytes = malloc(FL_SCRIPT_BYTES);
    if (!bytes) return false;
    put_u64(bytes, script->script); put_u64(bytes + 8, script->enabled);
    *out = (Fl_Component){FL_COMPONENT_SCRIPT, FL_SCRIPT_VERSION, FL_SCRIPT_BYTES, bytes};
    return true;
}

bool fl_script_decode(const Fl_Component *c, Fl_Script *out)
{
    if (!c || !out || c->type != FL_COMPONENT_SCRIPT || c->version != FL_SCRIPT_VERSION || c->size != FL_SCRIPT_BYTES || !c->data) return false;
    uint64_t script = get_u64(c->data), enabled = get_u64(c->data + 8);
    if (!script || enabled > 1) return false;
    *out = (Fl_Script){script, enabled != 0};
    return true;
}

bool fl_script_source_valid(const char *source, size_t size)
{
    return source && size && size <= FL_SCRIPT_SOURCE_LIMIT && !memchr(source, 0, size);
}

bool fl_script_name_valid(const char *name)
{
    size_t size = name ? strlen(name) : 0;
    if (!size || size > FL_SCRIPT_NAME_MAX) return false;
    for (size_t i = 0; i < size; ++i) if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f) return false;
    return true;
}

const char *fl_property_name(Fl_AnimProperty property)
{
    static const char *names[FL_PROP_COUNT] = {"x", "y", "width", "height", "rotation", "scale_x", "scale_y",
        "opacity", "fill", "stroke", "stroke_width", "font_size", "crop_x", "crop_y", "crop_w", "crop_h"};
    return property >= 0 && property < FL_PROP_COUNT ? names[property] : NULL;
}
