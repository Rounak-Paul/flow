#pragma once
#include "components/fl_animation.h"

/* One numeric property vocabulary shared by animation, Lua, document codecs and native controls. */
typedef struct Fl_PropertyDef {
    const char *name, *label;
    const char *component;      /* schema name: pose, bounds, camera, ... */
    uint64_t component_type;
    const char *lua_group, *lua_field;
    unsigned channels;
    double minimum, maximum;
} Fl_PropertyDef;

/* Borrow property metadata, or NULL for an invalid property. */
const Fl_PropertyDef *fl_property_definition(Fl_AnimProperty property);
/* Whether an entity's native components expose this property for authoring. */
bool fl_property_supported(const Fl_Entity *entity, Fl_AnimProperty property);
