#pragma once
#include "fl_animation.h"
#include "fl_script_bindings.h"
#include "fl_prototype_instance.h"
#include "time/fl_time_mapping.h"

#define FL_SCRIPT_SOURCE_LIMIT (128u * 1024u)
/* Script attachment schema: asset, local clock and independent JSON parameter values. */
#define FL_SCRIPT_VERSION 8u
#define FL_SCRIPT_PARAMETER_LIMIT (32u * 1024u)

/* An entity's Lua component: which Lua asset it runs, and whether it is active. */
typedef struct Fl_Script {
    Fl_Id script;
    bool enabled;
    int32_t layer;                /* explicit evaluation order, -1000000..1000000 */
    Fl_TimeMapping time;
    const char *parameters;       /* borrowed JSON object bytes; NULL means {} */
    size_t parameters_size;
    const char *bindings;         /* borrowed named typed references; NULL means {} */
    size_t bindings_size;
    const char *overrides;        /* structural edits to build output; NULL means [] */
    size_t overrides_size;
} Fl_Script;

/* Allocate a script payload in out; caller releases out->data with free. False when script.script is zero. */
bool fl_script_encode(const Fl_Script *script, Fl_Component *out);
/* Borrow an attachment from its owned payload; parameter bytes remain component-owned. */
bool fl_script_decode(const Fl_Component *component, Fl_Script *out);
/* Whether source is storable: 1..FL_SCRIPT_SOURCE_LIMIT bytes without NUL. */
bool fl_script_source_valid(const char *source, size_t size);
/* Borrow the first Script attachment referencing asset, or NULL. */
const Fl_Component *fl_entity_script_component(const Fl_Entity *entity, Fl_Id asset);

/* Validate the complete attachment, including its bounded parameter object. */
bool fl_script_component_valid(const Fl_Component *component);
