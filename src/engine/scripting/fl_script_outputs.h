#pragma once
#include "components/fl_script.h"
#include <lua.h>

typedef struct Fl_Scene Fl_Scene;

#define FL_SCRIPT_TARGET_LIMIT 65u
#define FL_SCRIPT_TEXT_PROPERTY FL_PROP_COUNT
#define FL_SCRIPT_OUTPUT_COUNT (FL_PROP_COUNT+1)
typedef enum Fl_OutputMode {FL_OUTPUT_NONE, FL_OUTPUT_REPLACE, FL_OUTPUT_ADD, FL_OUTPUT_MULTIPLY} Fl_OutputMode;
/* Compiled self/bound target; missing references remain explicitly unresolved. */
typedef struct Fl_ScriptTarget {
    char name[64];
    size_t entity;
    Fl_OutputMode modes[FL_SCRIPT_OUTPUT_COUNT];
} Fl_ScriptTarget;
/* Capture immutable module output declarations under the caller's protected Lua boundary.
   targets is caller-owned fixed storage; slot zero always represents self, including a read-only self. */
size_t fl_script_outputs_capture(lua_State *lua, int module, const Fl_Scene *scene, size_t entity,
                                  const cJSON *bindings, bool sample, Fl_ScriptTarget targets[FL_SCRIPT_TARGET_LIMIT]);

/* Capture sampled evaluated-input declarations; only declared property channels are exposed. */
size_t fl_script_reads_capture(lua_State *lua, int module, const Fl_Scene *scene, size_t entity,
                                const cJSON *bindings, bool sample, Fl_ScriptTarget targets[FL_SCRIPT_TARGET_LIMIT]);
