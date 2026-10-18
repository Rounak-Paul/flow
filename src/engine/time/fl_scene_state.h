#pragma once
#include "schema/fl_widgets.h"
#include "scripting/fl_script_runtime.h"

/* Evaluated per-entity values at one scene time (base components overridden by tracks). */
typedef struct Fl_EntityState {
    Fl_Transform transform;
    Fl_Appearance appearance;
    double opacity;             /* own opacity, 0..1; geometry multiplies ancestors */
} Fl_EntityState;
/* Cache of evaluated entity values, indexed like the scene's entities. */
typedef struct Fl_SceneState {
    Fl_Id scene;
    uint64_t revision;
    Fl_Tick tick;
    bool valid;
    Fl_EntityState *entities;
    size_t count, capacity;
    Fl_ScriptRuntime *scripts;
} Fl_SceneState;

/* Apply ease to normalised progress t in [0, 1]. */
double fl_ease_apply(Fl_Ease ease, double t);
/* Sample track at tick into out (4 values); false for an empty track. */
bool fl_track_sample(const Fl_Scene *scene, const Fl_Track *track, Fl_Tick tick, double out[4]);
/* Evaluate scene at tick; reuses the previous result when scene, revision and tick are unchanged.
   On allocation failure the previous state is kept and false is returned. */
bool fl_scene_state_update(Fl_SceneState *state, const Fl_Scene *scene, Fl_Tick tick);
/* Free storage and reset. */
void fl_scene_state_clear(Fl_SceneState *state);

/* Apply one validated property value to evaluated state. */
void fl_entity_state_apply(Fl_EntityState *state, Fl_AnimProperty property, const double value[4]);
