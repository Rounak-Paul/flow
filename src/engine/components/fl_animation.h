#pragma once
#include "fl_component.h"
#include "time/fl_time.h"
#include "doc/fl_binary.h"

/* Animatable entity properties. Colour values are sRGB 0..255 per channel. */
typedef enum Fl_AnimProperty {
    FL_PROP_X, FL_PROP_Y, FL_PROP_WIDTH, FL_PROP_HEIGHT, FL_PROP_ROTATION, FL_PROP_SCALE_X, FL_PROP_SCALE_Y,
    FL_PROP_OPACITY, FL_PROP_FILL, FL_PROP_STROKE, FL_PROP_STROKE_WIDTH, FL_PROP_FONT_SIZE, FL_PROP_COUNT
} Fl_AnimProperty;
/* Interpolation from a keyframe towards the next one. */
typedef enum Fl_Ease { FL_EASE_LINEAR, FL_EASE_IN, FL_EASE_OUT, FL_EASE_IN_OUT, FL_EASE_HOLD, FL_EASE_COUNT } Fl_Ease;
typedef struct Fl_Keyframe {
    Fl_Id id;
    Fl_TimeAnchor at;
    double value[4];            /* scalar properties use value[0] */
    Fl_Ease ease;
} Fl_Keyframe;
/* One property of one entity driven by keyframes (at most one track per entity/property). */
typedef struct Fl_Track {
    Fl_Id id, entity;
    Fl_AnimProperty property;
    Fl_Keyframe *keys;
    size_t key_count, key_capacity;
} Fl_Track;
typedef struct Fl_Op Fl_Op;

/* Keyframed motion in local ticks; the embedded Timeline state uses scene beat anchors. */
typedef struct Fl_AnimationState {
    Fl_Id id;
    const char *name;
    uint64_t duration;
    bool loop;
    size_t track_count;
    Fl_Track tracks[FL_PROP_COUNT];
} Fl_AnimationState;

/* Typed graph input: scalar, boolean or a consumable trigger. */
typedef enum Fl_AnimationParameterKind { FL_ANIM_NUMBER, FL_ANIM_BOOL, FL_ANIM_TRIGGER } Fl_AnimationParameterKind;
/* Persistent named graph input and its initial value. */
typedef struct Fl_AnimationParameter { Fl_Id id; const char *name; Fl_AnimationParameterKind kind; double value; } Fl_AnimationParameter;
/* Transition comparison against a graph parameter, or an unconditional edge. */
typedef enum Fl_AnimationCondition { FL_ANIM_ALWAYS, FL_ANIM_GREATER, FL_ANIM_LESS, FL_ANIM_EQUAL, FL_ANIM_FIRED } Fl_AnimationCondition;
/* Ordered graph edge; at_end delays the condition until a non-looping state's end. */
typedef struct Fl_AnimationTransition {
    Fl_Id id, from, to, parameter;
    Fl_AnimationCondition condition;
    double threshold;
    uint64_t blend_duration;
    bool at_end;
} Fl_AnimationTransition;
/* Entity-owned state library and transition graph. State zero is the scene Timeline. */
typedef struct Fl_Animation {
    Fl_AnimationState timeline;
    Fl_AnimationState *states;
    size_t state_count;
    Fl_AnimationParameter *parameters;
    size_t parameter_count;
    Fl_AnimationTransition *transitions;
    size_t transition_count;
    Fl_Id entry;
} Fl_Animation;

#define FL_ANIMATION_GRAPH_LIMIT 256u

/* Borrow a state by stable ID; zero selects the scene Timeline state. */
const Fl_AnimationState *fl_animation_state(const Fl_Animation *animation, Fl_Id state);
/* Borrow a named state, including the built-in Timeline state. */
const Fl_AnimationState *fl_animation_state_named(const Fl_Animation *animation, const char *name);
/* Mutate a private graph draft; the project validates and publishes it atomically. */
bool fl_animation_apply(Fl_Animation *animation, const Fl_Op *op, uint64_t *next_id, Fl_Id *created);
/* Count and append stable graph/state/key IDs for project-wide identity validation. */
size_t fl_animation_id_count(const Fl_Animation *animation);
void fl_animation_ids(const Fl_Animation *animation, Fl_Id *ids);

/* Borrow an entity's animation component, or NULL when absent. */
const Fl_Animation *fl_entity_animation(const Fl_Entity *entity);
/* Borrow mutable animation storage; optionally create an empty component. Internal owner-thread API. */
Fl_Animation *fl_animation_require(Fl_Entity *entity);
/* Release animation tracks and their keys; NULL is safe. */
void fl_animation_destroy(Fl_Animation *animation);
/* Deep-copy animation and its key storage; NULL on allocation failure. */
Fl_Animation *fl_animation_clone(const Fl_Animation *animation);
/* Serialize animation tracks into stream without native pointers or padding. */
void fl_animation_write(const Fl_Animation *animation, Fl_Binary *stream);
/* Deserialize bounded animation storage; NULL on malformed data or allocation failure. */
Fl_Animation *fl_animation_read(Fl_Binary *stream, uint64_t version);
/* Validate component-local property, key and identity invariants. */
bool fl_animation_valid(const Fl_Animation *animation);

/* Validate finite scalar/RGBA channels against the property range. */
bool fl_property_value_valid(Fl_AnimProperty property, const double value[4]);
