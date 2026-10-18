#pragma once
#include "fl_animation.h"

#define FL_SCRIPT_SOURCE_LIMIT (128u * 1024u)
#define FL_SCRIPT_NAME_MAX 64u
/* Wire schema version and size: the id of the scene script and an enabled flag, each a little-endian u64. */
#define FL_SCRIPT_VERSION 3u
#define FL_SCRIPT_BYTES 16u

/* An entity's Lua component: which scene script it runs, and whether it is active. */
typedef struct Fl_Script {
    Fl_Id script;
    bool enabled;
} Fl_Script;

/* Allocate a script payload in out; caller releases out->data with free. False when script.script is zero. */
bool fl_script_encode(const Fl_Script *script, Fl_Component *out);
/* Decode a script component, rejecting a malformed flag or a zero id. */
bool fl_script_decode(const Fl_Component *component, Fl_Script *out);
/* Whether source is storable: 1..FL_SCRIPT_SOURCE_LIMIT bytes without NUL. */
bool fl_script_source_valid(const char *source, size_t size);
/* Whether name is a usable script name: 1..FL_SCRIPT_NAME_MAX bytes, no control characters. */
bool fl_script_name_valid(const char *name);
/* Return the stable Lua name for a property, or NULL when invalid. */
const char *fl_property_name(Fl_AnimProperty property);
