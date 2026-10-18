#pragma once
#include "fl_animation.h"

#define FL_SCRIPT_SOURCE_LIMIT (128u * 1024u)

/* Borrowed source and enabled state of an entity's Lua component. */
typedef struct Fl_Script {
    const char *source;
    size_t source_size;
    bool enabled;
} Fl_Script;

/* Allocate a version-two script payload in out; caller releases out->data with free. */
bool fl_script_encode(const Fl_Script *script, Fl_Component *out);
/* Decode a script component into a borrowed view, rejecting malformed flags/source. */
bool fl_script_decode(const Fl_Component *component, Fl_Script *out);
/* Return the stable Lua name for a property, or NULL when invalid. */
const char *fl_property_name(Fl_AnimProperty property);
