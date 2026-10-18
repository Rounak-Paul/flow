#pragma once
#include "fl_component.h"
#include "time/fl_time.h"
#include "doc/fl_binary.h"

/* Animatable entity properties. Colour values are sRGB 0..255 per channel; crop values are fractions of the source image. */
typedef enum Fl_AnimProperty {
    FL_PROP_X, FL_PROP_Y, FL_PROP_WIDTH, FL_PROP_HEIGHT, FL_PROP_ROTATION, FL_PROP_SCALE_X, FL_PROP_SCALE_Y,
    FL_PROP_OPACITY, FL_PROP_FILL, FL_PROP_STROKE, FL_PROP_STROKE_WIDTH, FL_PROP_FONT_SIZE,
    FL_PROP_CROP_X, FL_PROP_CROP_Y, FL_PROP_CROP_W, FL_PROP_CROP_H, FL_PROP_COUNT
} Fl_AnimProperty;
/* Properties up to here are exposed to Lua; the crop rectangle of images and videos is animation-only. */
#define FL_PROP_SCRIPTED (FL_PROP_FONT_SIZE + 1)
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

/* Wire and in-memory schema version of the animation component. */
#define FL_ANIMATION_VERSION 3u

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
