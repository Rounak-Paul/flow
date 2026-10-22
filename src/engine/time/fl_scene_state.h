#pragma once
#include "schema/fl_widgets.h"
#include "components/fl_style.h"
#include "components/fl_spatial.h"
#include "components/fl_motion.h"
#include <stdatomic.h>
#include "scripting/fl_script_runtime.h"

/* Evaluated per-entity values at one scene time (base components overridden by tracks). */
typedef struct Fl_EntityState {
    Fl_Tick local_tick;         /* source scope tick, used by native media */
    Fl_Pose pose;
    Fl_Bounds bounds;
    Fl_Camera camera;
    Fl_Material material;
    Fl_Light light;
    Fl_Particles particles;
    Fl_Post post;
    Fl_MeshShape mesh;
    bool has_camera, has_mesh, has_material, has_light, has_particles, has_post, has_path, has_motion, has_effect;
    double trim_start, trim_end;
    Fl_TextMotion motion;
    uint32_t effect_kind;
    double effect_u[8];
    const char *effect_source;
    size_t effect_source_size;
    Fl_Appearance appearance;
    Fl_Style style;
    double media_seconds;       /* -1 follows the native video placement clock */
    double opacity;             /* own opacity, 0..1; geometry multiplies ancestors */
    double crop[4];             /* visible source region of an image or video: x, y, width, height as fractions */
    bool text_override;         /* a script replaced the text content */
    const char *text;           /* replacement borrowed from the evaluator until the next frame */
    size_t text_size;
} Fl_EntityState;
typedef struct Fl_Composition Fl_Composition;

/* Cache of evaluated entity values, indexed like the scene's entities. */
typedef struct Fl_SceneState {
    Fl_Id scene;
    uint64_t revision;
    Fl_Tick tick;
    bool valid;
    Fl_EntityState *entities;
    size_t count, capacity;
    Fl_ScriptRuntime *scripts;
    Fl_Composition *composition;
    char error[256];
} Fl_SceneState;

/* Apply ease to normalised progress t in [0, 1]. */
double fl_ease_apply(Fl_Ease ease, double t);
/* Cubic uses param x1,y1,x2,y2. Spring uses stiffness and damping. NULL params use the built-in defaults. */
double fl_ease_apply_param(Fl_Ease ease, const double param[4], double t);
/* Sample track at tick into out (4 values); false for an empty track. */
bool fl_track_sample(const Fl_Scene *scene, const Fl_Track *track, Fl_Tick tick, double out[4]);
/* Evaluate scene at tick; reuses the previous result when scene, revision and tick are unchanged.
   On allocation failure the previous state is kept and false is returned. */
bool fl_scene_state_update(Fl_SceneState *state, const Fl_Scene *scene, Fl_Tick tick);
/* Evaluate with cooperative cancellation between entities and inside Lua instruction hooks. Worker callers must
   use an immutable project read view. Cancellation invalidates this destination; it never publishes a partial frame. */
bool fl_scene_state_update_cancel(Fl_SceneState *state, const Fl_Scene *scene, Fl_Tick tick, const atomic_bool *cancel);

/* Free storage and reset. */
void fl_scene_state_clear(Fl_SceneState *state);

/* Apply one validated property value to evaluated state. */
void fl_entity_state_apply(Fl_EntityState *state, Fl_AnimProperty property, const double value[4]);

/* Sample one entity's authored native components and keyframes, without behavior outputs or allocation. */
bool fl_entity_state_sample(const Fl_Scene *scene, size_t entity, Fl_Tick tick, Fl_EntityState *out);

/* Borrow the evaluated hierarchy when expanded, otherwise the supplied authored scene. */
const Fl_Scene *fl_scene_state_scene(const Fl_SceneState *state, const Fl_Scene *authored);
/* Internal native scope evaluation; composition owns scope clocks and expansion. */
bool fl_scene_state_update_native(Fl_SceneState *state, const Fl_Scene *scene, Fl_Tick tick, const atomic_bool *cancel);

/* Borrow the authored hierarchy's behavior runtime, including when a composition owns it. */
const Fl_ScriptRuntime *fl_scene_state_scripts(const Fl_SceneState *state);

/* Borrow a behavior diagnostic using evaluated hierarchy indexing, including generated scopes. */
const char *fl_scene_state_entity_error(const Fl_SceneState *state, size_t entity);

/* Resolve a behavior's declared writer using evaluated hierarchy indexing; FL_PROP_COUNT denotes text. */
bool fl_scene_state_writer(const Fl_SceneState *state, size_t entity, int property, Fl_Id *component, int32_t *layer);

/* Read all native numeric channels into four-channel storage, zeroing unused channels. */
void fl_entity_state_values(const Fl_EntityState *state, double values[FL_PROP_COUNT][4]);
