#pragma once
#include "fl_project.h"
#include "doc/fl_json.h"
#include "time/fl_composition.h"

/* Append a typed local-address edit or ordered edit array to a prototype, owned composition or structural Script attachment.
   Each operation supplies op/component/value; address supplies the persistent owner, generator, path and entity.
   The shared project mutation/history boundary validates and publishes the entire resulting scope. */
bool fl_scope_edit(Fl_Project *project, Fl_Id scene, const Fl_Origin *address, const cJSON *operation);

/* Configure one displayed native entity through its persistent scope address. Existing components are patched;
   new components receive reserved extension IDs. remove_script is a displayed Script component ID or zero. */
bool fl_scope_configure(Fl_Project *project, Fl_Id scene, const Fl_Composition *composition, size_t entity,
                         const char *name, bool visible, const Fl_Component *components, size_t count, Fl_Id remove_script, Fl_Tick tick);

/* Apply native beat/key/track/clip operations in a generated object's local clock and identity namespace.
   Operation entity uses its displayed runtime ID; all other timeline IDs remain scope-local. */
bool fl_scope_timeline(Fl_Project *project, Fl_Id scene, const Fl_Composition *composition, size_t entity,
                         const Fl_Op *operation, Fl_Id *created);
