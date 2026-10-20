#pragma once
#include "fl_json.h"
#include "project/fl_project.h"
#include "time/fl_time_mapping.h"

/* Encode a scene as a typed JSON tree; caller owns the result. */
cJSON *fl_document_scene_write(Fl_Json *context, const Fl_Scene *scene);
/* Decode scene storage into an empty destination; caller clears partial storage on failure. */
bool fl_document_scene_read(Fl_Json *context, const cJSON *root, Fl_Scene *scene);
/* Encode/decode components with readable typed fields and owned payloads. */
cJSON *fl_document_component_write(Fl_Json *context, const Fl_Component *component);
bool fl_document_component_read(Fl_Json *context, const cJSON *root, Fl_Id entity, Fl_Component *out);
/* Shared exact tick/beat anchor representation. */
cJSON *fl_document_anchor_write(Fl_Json *context, const Fl_TimeAnchor *anchor);
bool fl_document_anchor_read(Fl_Json *context, const cJSON *root, Fl_TimeAnchor *anchor);

/* Shared readable local-clock mappings for behaviors and media components. */
cJSON *fl_document_time_write(Fl_Json *context, const Fl_TimeMapping *mapping);
bool fl_document_time_read(Fl_Json *context, const cJSON *root, Fl_TimeMapping *mapping);

/* Shared hierarchy and timeline payload for scene and Flow prototype scopes.
   Read allocates into an empty scope; caller clears partial storage on failure. */
bool fl_document_content_write(Fl_Json *context, cJSON *root, const Fl_Scene *scope);
bool fl_document_content_read(Fl_Json *context, const cJSON *root, Fl_Scene *scope);
