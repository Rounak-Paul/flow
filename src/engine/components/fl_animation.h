#pragma once
#include "fl_component.h"
#include "time/fl_time.h"
#include "doc/fl_binary.h"

/* Channel registry shared by animation, Lua, bake and native controls.
   Identity is (component schema, channel). The enum is the registry index. */
typedef enum Fl_AnimProperty {
    FL_PROP_X, FL_PROP_Y, FL_PROP_Z, FL_PROP_RX, FL_PROP_RY, FL_PROP_RZ,
    FL_PROP_SCALE_X, FL_PROP_SCALE_Y, FL_PROP_SCALE_Z, FL_PROP_PIVOT_X, FL_PROP_PIVOT_Y, FL_PROP_PIVOT_Z,
    FL_PROP_WIDTH, FL_PROP_HEIGHT, FL_PROP_DEPTH,
    FL_PROP_OPACITY, FL_PROP_FILL, FL_PROP_STROKE, FL_PROP_STROKE_WIDTH, FL_PROP_FONT_SIZE,
    FL_PROP_CROP_X, FL_PROP_CROP_Y, FL_PROP_CROP_W, FL_PROP_CROP_H, FL_PROP_RADIUS, FL_PROP_BLUR, FL_PROP_GRADIENT_ANGLE,
    FL_PROP_SHADOW_BLUR, FL_PROP_SHADOW_X, FL_PROP_SHADOW_Y, FL_PROP_GRADIENT_TO, FL_PROP_SHADOW_COLOR, FL_PROP_MEDIA_TIME,
    FL_PROP_FOV, FL_PROP_NEAR, FL_PROP_FAR, FL_PROP_EXPOSURE, FL_PROP_ORTHO,
    FL_PROP_ROUGHNESS, FL_PROP_METALLIC, FL_PROP_EMISSIVE,
    FL_PROP_INTENSITY, FL_PROP_RANGE, FL_PROP_SPOT_ANGLE, FL_PROP_LIGHT_COLOR,
    FL_PROP_EMIT_RATE, FL_PROP_LIFE, FL_PROP_PARTICLE_SIZE, FL_PROP_SPEED, FL_PROP_PARTICLE_COLOR,
    FL_PROP_BLOOM, FL_PROP_ASPECT_W, FL_PROP_ASPECT_H,
    FL_PROP_TRIM_START, FL_PROP_TRIM_END,
    FL_PROP_TRACKING, FL_PROP_STAGGER, FL_PROP_RISE, FL_PROP_TEXT_FADE, FL_PROP_TEXT_DURATION,
    FL_PROP_BLEND, FL_PROP_RADIAL, FL_PROP_GLOW, FL_PROP_GRADIENT_MID, FL_PROP_GRADIENT_MID_POS,
    FL_PROP_VIGNETTE, FL_PROP_GRAIN, FL_PROP_LIFT, FL_PROP_GAMMA, FL_PROP_GAIN,
    FL_PROP_PARTICLE_SIZE_END, FL_PROP_GRAVITY, FL_PROP_SPREAD, FL_PROP_BURST, FL_PROP_PARTICLE_BLEND,
    FL_PROP_FORCE_X, FL_PROP_FORCE_Y, FL_PROP_FORCE_Z, FL_PROP_PARTICLE_COLOR_END,
    FL_PROP_EFFECT_KIND, FL_PROP_U0, FL_PROP_U1, FL_PROP_U2, FL_PROP_U3, FL_PROP_U4, FL_PROP_U5, FL_PROP_U6, FL_PROP_U7,
    FL_PROP_TEXT_DELAY,
    FL_PROP_COUNT
} Fl_AnimProperty;
#define FL_PROP_ROTATION FL_PROP_RZ
/* Interpolation from a keyframe towards the next one. */
typedef enum Fl_Ease { FL_EASE_LINEAR, FL_EASE_IN, FL_EASE_OUT, FL_EASE_IN_OUT, FL_EASE_HOLD, FL_EASE_CUBIC, FL_EASE_SPRING, FL_EASE_COUNT } Fl_Ease;
typedef struct Fl_Keyframe {
    Fl_Id id;
    Fl_TimeAnchor at;
    double value[4];            /* scalar properties use value[0] */
    Fl_Ease ease;
    double ease_param[4];       /* cubic: x1,y1,x2,y2. spring: stiffness, damping */
} Fl_Keyframe;
/* One property of one entity driven by keyframes (at most one track per entity/property). */
typedef struct Fl_Track {
    Fl_Id id, entity;
    uint64_t component;         /* schema that owns the channel */
    Fl_AnimProperty property;   /* channel index in the registry */
    Fl_Keyframe *keys;
    size_t key_count, key_capacity;
} Fl_Track;

/* Wire and in-memory schema version of the animation component. */
#define FL_ANIMATION_VERSION 5u

/* Entity-owned keyframe tracks, at most one per property. */
typedef struct Fl_Animation {
    size_t track_count;
    Fl_Track tracks[FL_PROP_COUNT];
} Fl_Animation;

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
Fl_Animation *fl_animation_read(Fl_Binary *stream);
/* Validate component-local property, key and identity invariants. */
bool fl_animation_valid(const Fl_Animation *animation);

/* Validate finite scalar/RGBA channels against the property range. */
bool fl_property_value_valid(Fl_AnimProperty property, const double value[4]);

/* Return the canonical document/operation name for a property, or NULL when invalid. */
const char *fl_property_name(Fl_AnimProperty property);
