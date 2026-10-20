#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "doc/fl_binary.h"

typedef uint64_t Fl_Id;
typedef struct Fl_Entity Fl_Entity;

/* Component schema ids are persistent; unknown schemas are preserved verbatim. */
enum { FL_COMPONENT_TRANSFORM = 1, FL_COMPONENT_RECTANGLE = 2, FL_COMPONENT_TEXT = 3,
       FL_COMPONENT_APPEARANCE = 4, FL_COMPONENT_GEOMETRY = 5, FL_COMPONENT_TYPOGRAPHY = 6,
       FL_COMPONENT_IMAGE = 7, FL_COMPONENT_OPACITY = 8,
       FL_COMPONENT_ANIMATION = 9, FL_COMPONENT_SCRIPT = 10, FL_COMPONENT_STYLE = 11, FL_COMPONENT_VIDEO = 12 };
/* Canonical byte payloads, except Animation which owns typed track storage through its codec. */
typedef struct Fl_Component { uint64_t type, version; size_t size; const unsigned char *data; Fl_Id id; } Fl_Component;

/* Deep-copy a component into out; out is unchanged on failure. */
bool fl_component_clone(const Fl_Component *component, Fl_Component *out);
/* Release an owned component payload and reset its record. */
void fl_component_clear(Fl_Component *component);
/* Write an owned component with its versioned wire payload. */
void fl_component_write(const Fl_Component *component, Fl_Binary *stream);
/* Read one component, converting wire data into owned storage. */
bool fl_component_read(Fl_Component *component, Fl_Binary *stream);
/* Remove an optional component from an entity; Transform cannot be removed. */
bool fl_entity_remove_component(Fl_Entity *entity, uint64_t type);

/* Borrow the display name of a built-in component, or NULL for an unknown type. */
const char *fl_component_name(uint64_t type);

/* Borrow component of schema type from entity, or NULL. Valid until mutation. */
const Fl_Component *fl_entity_component(const Fl_Entity *entity, uint64_t type);

/* Borrow an entity-owned component by stable instance id; NULL when absent. */
const Fl_Component *fl_entity_component_instance(const Fl_Entity *entity, Fl_Id id);
/* Remove a particular optional component instance; Transform remains mandatory. */
bool fl_entity_remove_component_instance(Fl_Entity *entity, Fl_Id id);
