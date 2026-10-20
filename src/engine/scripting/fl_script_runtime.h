#pragma once
#include "components/fl_script.h"
#include <cJSON.h>
#include <stdatomic.h>
#include "time/fl_time.h"

typedef struct Fl_Scene Fl_Scene;
typedef struct Fl_EntityState Fl_EntityState;
typedef struct Fl_ScriptRuntime Fl_ScriptRuntime;

/* Create a bounded scene evaluator; source programs compile lazily for this scene revision. */
Fl_ScriptRuntime *fl_script_runtime_create(const Fl_Scene *scene);
/* Release every cached Lua program and its VM. NULL is safe. */
void fl_script_runtime_destroy(Fl_ScriptRuntime *runtime);
/* Release sampled output storage before evaluating a new frame. Previously borrowed text becomes invalid. */
void fl_script_runtime_begin_frame(Fl_ScriptRuntime *runtime);
/* Borrow a cancellation token during evaluation only; clear it before the caller releases token storage. */
void fl_script_runtime_set_cancel(Fl_ScriptRuntime *runtime, const atomic_bool *cancel);

/* Restore/step explicit simulation state and sample each attached module at tick. Errors preserve outputs and retain diagnostics. */
bool fl_script_runtime_evaluate(Fl_ScriptRuntime *runtime, const Fl_Scene *scene, size_t entity,
                                Fl_Tick tick, Fl_EntityState *state);
/* Compile source without running it. False with the Lua diagnostic (chunk name "script") in error on a syntax error. */
bool fl_script_check(const char *source, size_t size, char *error, size_t error_size);
/* Borrow the most recent diagnostic for entity, or an empty string when none. */
const char *fl_script_runtime_error(const Fl_ScriptRuntime *runtime, size_t entity);

/* Borrow the most recent diagnostic for a stable Script component instance. */
const char *fl_script_runtime_component_error(const Fl_ScriptRuntime *runtime, Fl_Id component);

/* Borrow the evaluated module's exposed parameter definitions for native controls and agent inspection. */
const cJSON *fl_script_runtime_parameter_schema(const Fl_ScriptRuntime *runtime, Fl_Id component);

/* Compile attached enabled modules and validate exposed parameters without initializing, stepping or sampling.
   Unbound clocks remain diagnosed runtime bindings; invalid executable declarations fail publication. */
bool fl_script_validate_scene(const Fl_Scene *scene, char *error, size_t error_size);
