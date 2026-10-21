#pragma once
#include "fl_component.h"
#include "time/fl_time_mapping.h"
#include "doc/fl_json.h"

#define FL_PROTOTYPE_INSTANCE_VERSION 1u
#define FL_PROTOTYPE_OVERRIDE_LIMIT (1024u * 1024u)
/* A live reusable hierarchy; overrides address stable local component/entity IDs and nested component paths. */
typedef struct Fl_PrototypeInstance {
    Fl_Id asset;
    Fl_TimeMapping time;
    const char *overrides; /* borrowed JSON operation array; NULL means [] */
    size_t overrides_size;
} Fl_PrototypeInstance;
/* Encode a validated instance into owned component storage. */
bool fl_prototype_instance_encode(const Fl_PrototypeInstance *instance, Fl_Component *out);
/* Borrow an instance and its override bytes from a component. */
bool fl_prototype_instance_decode(const Fl_Component *component, Fl_PrototypeInstance *out);
/* Validate the payload and override operation shapes independently of the referenced source. */
bool fl_prototype_instance_valid(const Fl_Component *component);
/* Check override operation shapes; resource and target validation occurs when compiling the final composition. */
bool fl_prototype_overrides_valid(Fl_Json *context, const cJSON *operations);
