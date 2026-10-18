#pragma once
#include "components/fl_script.h"
#include "time/fl_time.h"

typedef struct Fl_Scene Fl_Scene;
typedef struct Fl_EntityState Fl_EntityState;
typedef struct Fl_ScriptRuntime Fl_ScriptRuntime;

/* Create a bounded scene evaluator; source programs compile lazily for this scene revision. */
Fl_ScriptRuntime *fl_script_runtime_create(const Fl_Scene *scene);
/* Release every cached Lua program and its VM. NULL is safe. */
void fl_script_runtime_destroy(Fl_ScriptRuntime *runtime);
/* Run the entity's on_evaluate(self, seconds) over state at tick. Errors preserve state and retain diagnostics. */
bool fl_script_runtime_evaluate(Fl_ScriptRuntime *runtime, const Fl_Scene *scene, size_t entity,
                                Fl_Tick tick, Fl_EntityState *state);
/* Borrow the most recent diagnostic for entity, or an empty string when none. */
const char *fl_script_runtime_error(const Fl_ScriptRuntime *runtime, size_t entity);
