#pragma once
#include "fl_project.h"
#include "doc/fl_json.h"

#define FL_PROTOTYPE_SOURCE_LIMIT (16u * 1024u * 1024u)
/* A reusable hierarchy with its own identity and time namespaces; no canvas or presentation settings. */
typedef struct Fl_Prototype {
    Fl_Id root, next_id, next_component_id;
    Fl_Scene scope;
} Fl_Prototype;

/* Parse a bounded .flproto.json source into empty owned storage; failure leaves it empty.
   Local hierarchy, identity and native timeline semantics are checked without resolving project resources. */
bool fl_prototype_read(Fl_Json *context, const char *source, size_t size, Fl_Prototype *out);
/* Encode a prototype document; caller owns the JSON tree. */
cJSON *fl_prototype_write(Fl_Json *context, const Fl_Prototype *prototype);
/* Validate all referenced project resources and enabled Lua declarations in a parsed prototype. */
bool fl_prototype_validate(Fl_Json *context, Fl_Prototype *prototype, const Fl_Project *project);
/* Release an owned prototype, including partial storage, and reset it. */
void fl_prototype_clear(Fl_Prototype *prototype);
