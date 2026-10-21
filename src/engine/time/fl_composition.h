#pragma once
#include "project/fl_project.h"
#include "fl_time_mapping.h"
#include <stdatomic.h>
#include <cJSON.h>

typedef struct Fl_Composition Fl_Composition;
typedef struct Fl_SceneState Fl_SceneState;
typedef struct Fl_ScriptRuntime Fl_ScriptRuntime;
/* Stable provenance of one evaluated entity; component identifies the root generator; path contains nested local generator component IDs. */
typedef struct Fl_Origin {
    Fl_Id owner, component, entity;
    Fl_Id path[32];
    size_t depth;
    bool generated;
} Fl_Origin;
/* Compile native and prototype hierarchies, rejecting recursion, identity collisions and invalid overrides. */
Fl_Composition *fl_composition_create(const Fl_Scene *source, const atomic_bool *cancel, char *error, size_t capacity);
/* Release compiled scope storage and independently owned behavior evaluators. */
void fl_composition_destroy(Fl_Composition *composition);
/* Borrow the flattened render/inspection hierarchy. */
const Fl_Scene *fl_composition_scene(const Fl_Composition *composition);
/* Borrow provenance for a flat entity index, or NULL when out of range. */
const Fl_Origin *fl_composition_origin(const Fl_Composition *composition, size_t index);
/* Number of retained overrides whose targets or nested paths are unresolved. */
size_t fl_composition_unresolved(const Fl_Composition *composition);
/* Sample each independent scope clock into destination's flat entity storage; destination owns the array. */
bool fl_composition_sample(Fl_Composition *composition, Fl_Tick tick, Fl_SceneState *destination, const atomic_bool *cancel);
/* Whether a hierarchy contains live prototype instances requiring composition compilation. */
bool fl_composition_required(const Fl_Scene *scene);

/* Read one compiled local clock/hierarchy without sampling behavior; root parent is SIZE_MAX. */
bool fl_composition_scope(const Fl_Composition *composition, size_t index, const Fl_Scene **scene,
                           size_t *parent, const Fl_TimeMapping **mapping);

/* Borrow the authored root scope's behavior runtime; generated scopes retain independent runtimes. */
const Fl_ScriptRuntime *fl_composition_scripts(const Fl_Composition *composition);

/* Borrow the behavior diagnostic for a flat authored/generated entity address. */
const char *fl_composition_entity_error(const Fl_Composition *composition, size_t index);

/* Resolve a displayed component to its persistent local identity for scoped authoring. */
bool fl_composition_component_local(const Fl_Composition *composition, size_t entity, Fl_Id runtime, Fl_Id *local);

/* Borrow the selected scope runtime and its local entity index for native authoring/provenance. */
const Fl_ScriptRuntime *fl_composition_entity_scripts(const Fl_Composition *composition, size_t entity, size_t *local);

/* Borrow the native scope clock/timeline containing a displayed entity. */
const Fl_Scene *fl_composition_entity_scope(const Fl_Composition *composition, size_t entity);

/* Copy compiled native content before runtime address remapping; owner/generator are parent-local IDs. */
cJSON *fl_composition_scope_document(const Fl_Composition *composition, size_t index, Fl_Id *owner, Fl_Id *generator);

/* Copy the canonical local document containing a displayed entity; caller owns the tree. */
cJSON *fl_composition_entity_document(const Fl_Composition *composition, size_t entity);
/* Translate a scope-local edit/seek to root time, using the loop cycle at reference_root. */
bool fl_composition_entity_root_tick(const Fl_Composition *composition, size_t entity,
                                      Fl_Tick local_tick, Fl_Tick reference_root, Fl_Tick *root_tick);
