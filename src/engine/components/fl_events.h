#pragma once
#include "fl_component.h"
#include "doc/fl_document.h"

#define FL_EVENTS_LIMIT (64u*1024u)
#define FL_EVENTS_COUNT 256u
/* Validate stable component-local event IDs, beat-aware anchors and bounded named JSON payloads. */
bool fl_events_valid(const cJSON *events);
/* Encode an event array as owned canonical bytes. IDs belong to this component, not the project allocator. */
bool fl_events_encode(const cJSON *events, Fl_Component *out);
/* Decode owned JSON from a validated native component; caller releases it with cJSON_Delete. */
cJSON *fl_events_read(const Fl_Component *component);
