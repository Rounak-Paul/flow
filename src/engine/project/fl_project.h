#pragma once
#include "core/fl_event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t Fl_Id;
typedef struct Fl_Project Fl_Project;

/* Component schema ids are persistent; unknown schemas are preserved verbatim. */
enum { FL_COMPONENT_TRANSFORM = 1, FL_COMPONENT_RECTANGLE = 2, FL_COMPONENT_TEXT = 3,
       FL_COMPONENT_APPEARANCE = 4, FL_COMPONENT_GEOMETRY = 5, FL_COMPONENT_TYPOGRAPHY = 6,
       FL_COMPONENT_IMAGE = 7, FL_COMPONENT_OPACITY = 8 };
/* Persistent media classes of imported assets. */
typedef enum Fl_AssetKind { FL_ASSET_IMAGE = 1, FL_ASSET_VIDEO, FL_ASSET_AUDIO, FL_ASSET_FONT } Fl_AssetKind;
#define FL_ASSET_EXTENSION_MAX 15
/* Content-addressed media stored once as assets/<sha256>.<extension>. */
typedef struct Fl_Asset {
    Fl_Id id;
    const char *name;           /* display name; never empty */
    Fl_AssetKind kind;
    unsigned char hash[32];     /* SHA-256 of the stored bytes; unique per project */
    const char *extension;      /* canonical lowercase [a-z0-9]{1,15} */
    uint64_t size;              /* stored bytes */
    uint64_t width, height;     /* pixel dimensions for images and video; zero when not applicable */
    uint64_t duration;          /* video/audio length in 120000-per-second ticks; zero when unknown */
    uint32_t sample_rate, channels; /* audio stream description; zero otherwise */
} Fl_Asset;
#define FL_TICKS_PER_SECOND 120000u
/* Transform values are local to the parent, in scene pixels and radians. */
typedef struct Fl_Transform { double x, y, width, height, rotation, scale_x, scale_y; } Fl_Transform;
typedef struct Fl_Component { uint64_t type, version; size_t size; const unsigned char *data; } Fl_Component;
typedef struct Fl_Entity {
    Fl_Id id, parent;
    const char *name;
    bool visible;
    const Fl_Component *components;
    size_t component_count;
} Fl_Entity;
typedef struct Fl_EntityIndex Fl_EntityIndex;

/* Scene time in FL_TICKS_PER_SECOND units. */
typedef int64_t Fl_Tick;
/* Keyframe/clip time: an absolute scene time, or an offset from a beat's start or end edge. */
typedef enum Fl_AnchorKind { FL_ANCHOR_ABSOLUTE, FL_ANCHOR_BEAT } Fl_AnchorKind;
typedef struct Fl_TimeAnchor {
    Fl_AnchorKind kind;
    Fl_Id beat;                 /* FL_ANCHOR_BEAT only */
    bool end;                   /* measure from the beat's end edge */
    Fl_Tick offset;             /* ticks after the edge; the scene time itself for FL_ANCHOR_ABSOLUTE */
} Fl_TimeAnchor;
/* Narrative step; beats partition the scene and their durations sum to the scene duration. */
typedef struct Fl_Beat {
    Fl_Id id;
    const char *name;
    uint64_t duration;          /* ticks, > 0 */
    bool auto_advance;          /* presenter: continue without a click */
} Fl_Beat;
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
/* Audio asset placed on the scene timeline. */
typedef struct Fl_AudioClip {
    Fl_Id id, asset;
    Fl_TimeAnchor start;
    uint64_t source_offset;     /* ticks skipped at the start of the asset */
    uint64_t duration;          /* ticks played; zero plays to the end of the asset */
    double gain;                /* linear, 0..4 */
    uint64_t fade_in, fade_out; /* ticks */
} Fl_AudioClip;

typedef struct Fl_Scene {
    Fl_Id id;
    const char *name;
    uint64_t revision;
    uint64_t duration; /* 120000 ticks per second */
    uint64_t width, height;
    const Fl_Entity *entities;
    size_t entity_count;
    const Fl_EntityIndex *identity_index; /* private lookup storage; callers never mutate */
    size_t identity_capacity, entity_capacity;
    const Fl_Beat *beats;               /* ordered; empty means one implicit beat spanning duration */
    size_t beat_count;
    const Fl_Track *tracks;
    size_t track_count, track_capacity;
    const Fl_AudioClip *clips;
    size_t clip_count;
} Fl_Scene;

typedef enum Fl_OpKind {
    FL_OP_SCENE_ADD, FL_OP_SCENE_REMOVE, FL_OP_SCENE_RENAME, FL_OP_SCENE_MOVE,
    FL_OP_ENTITY_ADD, FL_OP_ENTITY_REMOVE, FL_OP_ENTITY_RENAME, FL_OP_ENTITY_PARENT,
    FL_OP_ENTITY_MOVE, FL_OP_ENTITY_VISIBLE, FL_OP_COMPONENT_SET, FL_OP_COMPONENT_REMOVE,
    FL_OP_SCENE_SETTINGS, FL_OP_ENTITY_CONFIGURE, FL_OP_TRANSFORMS,
    FL_OP_ASSET_ADD, FL_OP_ASSET_REMOVE, FL_OP_ASSET_RENAME,
    FL_OP_BEAT_ADD, FL_OP_BEAT_REMOVE, FL_OP_BEAT_UPDATE, FL_OP_BEAT_MOVE,
    FL_OP_KEY_SET, FL_OP_KEY_REMOVE, FL_OP_TRACK_REMOVE,
    FL_OP_CLIP_ADD, FL_OP_CLIP_UPDATE, FL_OP_CLIP_REMOVE
} Fl_OpKind;
/* One local transform replacement in an atomic selection gesture. */
typedef struct Fl_TransformEdit { Fl_Id entity; Fl_Transform transform; } Fl_TransformEdit;
/* Operations reference stable ids; position is a scene/entity array insertion index. */
typedef struct Fl_Op {
    Fl_OpKind kind;
    Fl_Id scene, entity, parent;
    const char *name;
    size_t position;
    bool visible;
    Fl_Component component;
    uint64_t width, height, duration;
    const Fl_Component *components; /* add/configure: merge these schemas, preserving others */
    size_t component_count;
    const Fl_TransformEdit *transforms; /* FL_OP_TRANSFORMS only */
    size_t transform_count;
    Fl_Id asset;                        /* asset remove/rename target */
    const Fl_Asset *asset_record;       /* FL_OP_ASSET_ADD: copied record; its id is ignored */
    /* Timeline. Beats: add (position, name, duration, auto_advance), remove (beat; anchors on it become
       absolute), update (beat, name, duration, auto_advance), move (beat, position).
       Keys: set (entity, property, key: 0 creates, or updates the key already at the anchor's time; anchor, value, ease),
       remove (key); track remove (track).
       Clips: add/update (clip_record; update targets clip), remove (clip). */
    Fl_Id beat, key, track, clip;
    uint64_t beat_duration;
    bool auto_advance;
    bool scale_keys; /* BEAT_UPDATE: scale in-beat key offsets with its duration; audio is not stretched. */
    Fl_AnimProperty property;
    Fl_TimeAnchor anchor;
    double value[4];
    Fl_Ease ease;
    const Fl_AudioClip *clip_record;
    /* FL_OP_TRANSFORMS / FL_OP_ENTITY_CONFIGURE: animated properties are written as keyframes at tick
       instead of changing their base values. */
    bool keyed;
    Fl_Tick tick;
} Fl_Op;
#define FL_EVENT_PROJECT_CHANGED "project.changed"
typedef struct Fl_ProjectChange { Fl_Project *project; Fl_OpKind kind; Fl_Id scene, entity, asset; } Fl_ProjectChange;

/* Create a new project directory exclusively; existing project files are never replaced. */
Fl_Project *fl_project_create(const char *directory, const char *name, Fl_EventBus *bus);
/* Open and validate a binary project, acquiring its exclusive process lock. */
Fl_Project *fl_project_open(const char *directory, Fl_EventBus *bus);
/* Free the project and release the lock. Does not implicitly save. NULL is safe. */
void fl_project_destroy(Fl_Project *project);
/* Owner-thread only. Apply one validated operation atomically; out_id receives a created object's id. */
bool fl_project_apply(Fl_Project *project, const Fl_Op *op, Fl_Id *out_id);
/* Save changed scenes first and commit their ordered manifest last, atomically. */
bool fl_project_save(Fl_Project *project);
/* Return project's borrowed name; NULL for a NULL project. */
const char *fl_project_name(const Fl_Project *project);
/* Return project's borrowed absolute directory; NULL for a NULL project. */
const char *fl_project_directory(const Fl_Project *project);
/* Return the number of scenes in project render order; zero for NULL. */
size_t fl_project_scene_count(const Fl_Project *project);
/* Borrow scene at zero-based index, or NULL when absent. Valid until mutation. */
const Fl_Scene *fl_project_scene_at(const Fl_Project *project, size_t index);
/* Borrow scene by stable id, or NULL when absent. Valid until mutation. */
const Fl_Scene *fl_project_scene(const Fl_Project *project, Fl_Id id);
/* Borrow entity from scene by stable id, or NULL. Valid until mutation. */
const Fl_Entity *fl_scene_entity(const Fl_Scene *scene, Fl_Id id);
/* Borrow component of schema type from entity, or NULL. Valid until mutation. */
const Fl_Component *fl_entity_component(const Fl_Entity *entity, uint64_t type);
/* Return the number of assets in import order; zero for NULL. */
size_t fl_project_asset_count(const Fl_Project *project);
/* Borrow asset at index, by stable id, or by content hash; NULL when absent. Valid until mutation. */
const Fl_Asset *fl_project_asset_at(const Fl_Project *project, size_t index);
const Fl_Asset *fl_project_asset(const Fl_Project *project, Fl_Id id);
const Fl_Asset *fl_project_asset_by_hash(const Fl_Project *project, const unsigned char hash[32]);
/* Count components across all scenes that reference asset; referenced assets cannot be removed. */
size_t fl_project_asset_uses(const Fl_Project *project, Fl_Id asset);
/* Write the stored file path of asset into out; false when it does not fit. */
bool fl_project_asset_path(const Fl_Project *project, const Fl_Asset *asset, char *out, size_t capacity);
/* Build assets/<hex>.<extension> under directory for a hash/extension pair. */
bool fl_asset_store_path(const char *directory, const unsigned char hash[32], const char *extension,
                         char *out, size_t capacity);
/* Validate a canonical stored-file extension. */
bool fl_asset_extension_valid(const char *extension);
/* Return whether project has changes beyond its committed manifest. */
bool fl_project_dirty(const Fl_Project *project);
/* Scene length in ticks (sum of beats, or the scene duration when it has no beats). */
Fl_Tick fl_scene_length(const Fl_Scene *scene);
/* Resolve beat's [start, end) in ticks; false when the beat is not in scene. */
bool fl_scene_beat_range(const Fl_Scene *scene, Fl_Id beat, Fl_Tick *start, Fl_Tick *end);
/* Resolve an anchor to scene time; anchors on missing beats resolve to their offset. */
Fl_Tick fl_anchor_resolve(const Fl_Scene *scene, const Fl_TimeAnchor *anchor);
/* Anchor tick to the start of the beat containing it (absolute when the scene has no beats). */
Fl_TimeAnchor fl_anchor_at(const Fl_Scene *scene, Fl_Tick tick);
/* Borrow the track animating entity's property, or NULL. Valid until mutation. */
const Fl_Track *fl_scene_track(const Fl_Scene *scene, Fl_Id entity, Fl_AnimProperty property);
/* Borrow a keyframe by id with its track, or NULL. */
const Fl_Keyframe *fl_scene_key(const Fl_Scene *scene, Fl_Id key, const Fl_Track **track);
/* Encode/decode the v1 transform component; no native struct bytes reach disk. */
void fl_transform_encode(const Fl_Transform *transform, unsigned char out[56]);
bool fl_transform_decode(const Fl_Component *component, Fl_Transform *out);
