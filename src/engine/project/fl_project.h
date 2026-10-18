#pragma once
#include "core/fl_event.h"
#include "components/fl_transform.h"
#include "components/fl_animation.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Fl_Project Fl_Project;

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
typedef struct Fl_Entity {
    Fl_Id id, parent;
    const char *name;
    bool visible;
    const Fl_Component *components;
    size_t component_count;
} Fl_Entity;
typedef struct Fl_EntityIndex Fl_EntityIndex;

/* Narrative step; beats partition the scene and their durations sum to the scene duration. */
typedef struct Fl_Beat {
    Fl_Id id;
    const char *name;
    uint64_t duration;          /* ticks, > 0 */
    bool auto_advance;          /* presenter: continue without a click */
} Fl_Beat;
/* Audio asset placed on the scene timeline. */
typedef struct Fl_AudioClip {
    Fl_Id id, asset;
    Fl_TimeAnchor start;
    uint64_t source_offset;     /* ticks skipped at the start of the asset */
    uint64_t duration;          /* ticks played; zero plays to the end of the asset */
    double gain;                /* linear, 0..4 */
    uint64_t fade_in, fade_out; /* ticks */
} Fl_AudioClip;

/* Scene backdrop colour (sRGB RGBA8) of newly created scenes. */
#define FL_SCENE_BACKGROUND_DEFAULT {25, 31, 47, 255}

/* Reusable Lua source stored in a scene. Entities run it through their Script component, which names it by id. */
typedef struct Fl_ScriptSource {
    Fl_Id id;
    const char *name;       /* unique within the scene */
    const char *source;
    size_t source_size;
} Fl_ScriptSource;

typedef struct Fl_Scene {
    Fl_Id id;
    const char *name;
    uint64_t revision;
    uint64_t duration; /* 120000 ticks per second */
    uint64_t width, height; /* canvas dimensions; target framing never moves entities */
    uint64_t aspect_width, aspect_height; /* target ratio; both zero use the whole canvas */
    double camera_x, camera_y; /* output camera offset from canvas center, in scene units */
    unsigned char background[4]; /* backdrop colour, sRGB RGBA8 with opaque alpha */
    const Fl_Entity *entities;
    size_t entity_count;
    const Fl_EntityIndex *identity_index; /* private lookup storage; callers never mutate */
    size_t identity_capacity, entity_capacity;
    const Fl_Beat *beats;               /* ordered; empty means one implicit beat spanning duration */
    size_t beat_count;
    const Fl_AudioClip *clips;
    size_t clip_count;
    const Fl_ScriptSource *scripts;     /* script repository, in creation order */
    size_t script_count;
} Fl_Scene;

typedef enum Fl_OpKind {
    FL_OP_SCENE_ADD, FL_OP_SCENE_REMOVE, FL_OP_SCENE_RENAME, FL_OP_SCENE_MOVE,
    FL_OP_ENTITY_ADD, FL_OP_ENTITY_REMOVE, FL_OP_ENTITY_RENAME, FL_OP_ENTITY_PARENT,
    FL_OP_ENTITY_MOVE, FL_OP_ENTITY_VISIBLE, FL_OP_COMPONENT_SET, FL_OP_COMPONENT_REMOVE,
    FL_OP_SCENE_SETTINGS, FL_OP_ENTITY_CONFIGURE, FL_OP_TRANSFORMS,
    FL_OP_ASSET_ADD, FL_OP_ASSET_REMOVE, FL_OP_ASSET_RENAME,
    FL_OP_BEAT_ADD, FL_OP_BEAT_REMOVE, FL_OP_BEAT_UPDATE, FL_OP_BEAT_MOVE,
    FL_OP_KEY_SET, FL_OP_KEY_REMOVE, FL_OP_TRACK_REMOVE,
    FL_OP_CLIP_ADD, FL_OP_CLIP_UPDATE, FL_OP_CLIP_REMOVE, FL_OP_BEAT_SPLIT, FL_OP_SCENE_ASPECT, FL_OP_SCENE_CAMERA, FL_OP_SCENE_BACKGROUND,
    FL_OP_SCRIPT_SET, FL_OP_SCRIPT_REMOVE,
    FL_OP_RESTORE /* notification kind only: scenes were replaced by fl_project_restore; never a valid operation */
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
    uint64_t aspect_width, aspect_height; /* FL_OP_SCENE_ASPECT; both zero reset to canvas */
    double camera_x, camera_y; /* FL_OP_SCENE_CAMERA; finite scene-space offsets */
    unsigned char background[4]; /* FL_OP_SCENE_BACKGROUND; alpha must be 255 */
    const Fl_Component *components; /* add/configure: merge these schemas, preserving others */
    size_t component_count;
    const Fl_TransformEdit *transforms; /* FL_OP_TRANSFORMS only */
    size_t transform_count;
    /* Scripts. SET creates when script is zero (name and source required) or updates script (NULL name or source keeps
       the current one); REMOVE deletes script and detaches it from every entity that runs it. */
    Fl_Id script;
    const char *source;
    size_t source_size;
    Fl_Id asset;                        /* asset remove/rename target */
    const Fl_Asset *asset_record;       /* FL_OP_ASSET_ADD: copied record; its id is ignored */
    /* Timeline. Beats: add (position, name, duration, auto_advance),
       remove (beat; removes its keys, preserves audio start times),
       update (beat, name, duration, auto_advance; duration changes scale owned keys), move (beat, position),
       split (beat, tick strictly inside beat, name for the new right beat; preserves anchored times).
       Keys: set (entity, property, key: 0 upserts at anchor time; anchor, value, ease).
       Keys stay within scene/beat bounds; absolute inputs attach to their containing beat.
       remove (key); track remove (track).
       Clips: add/update (clip_record; update targets clip), remove (clip). */
    Fl_Id beat, key, track, clip;
    uint64_t beat_duration;
    bool auto_advance;
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
/* Open and validate a binary project of the current format with its exclusive process lock. */
Fl_Project *fl_project_open(const char *directory, Fl_EventBus *bus);
/* Count of successful document mutations; changes on every apply and restore. */
uint64_t fl_project_generation(const Fl_Project *project);
/* Point-in-time copy of every scene, for atomic multi-operation transactions and undo.
   Assets are content-addressed and outside the snapshot. */
typedef struct Fl_Snapshot Fl_Snapshot;
/* Capture all scenes; NULL on allocation failure or from a non-owner thread. */
Fl_Snapshot *fl_project_snapshot(const Fl_Project *project);
/* Replace the project's scenes with the snapshot's. Restored scenes receive fresh revisions, scenes absent
   from the snapshot are removed, and FL_OP_RESTORE is published per affected scene. False leaves the
   project untouched. */
bool fl_project_restore(Fl_Project *project, const Fl_Snapshot *snapshot);
/* Encoded size of the snapshot, for history budgeting. */
size_t fl_snapshot_bytes(const Fl_Snapshot *snapshot);
/* Release a snapshot. NULL is safe. */
void fl_snapshot_destroy(Fl_Snapshot *snapshot);
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
/* Borrow a scene script by id or by exact name, or NULL. Valid until the next mutation. */
const Fl_ScriptSource *fl_scene_script(const Fl_Scene *scene, Fl_Id id);
const Fl_ScriptSource *fl_scene_script_named(const Fl_Scene *scene, const char *name);
/* Number of entities in scene whose Script component runs script. */
size_t fl_scene_script_uses(const Fl_Scene *scene, Fl_Id script);
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
/* Linear traversal of entity-owned Timeline tracks without rebuilding scene-level storage. */
typedef struct Fl_TrackIterator { const Fl_Scene *scene; size_t entity, track; } Fl_TrackIterator;
/* Borrow the next track and advance iterator; NULL at the end. */
const Fl_Track *fl_scene_track_next(Fl_TrackIterator *iterator);
/* Count entity-owned Timeline tracks in scene order. */
size_t fl_scene_track_count(const Fl_Scene *scene);
/* Borrow a track at its flattened scene index, or NULL. Valid until mutation. */
const Fl_Track *fl_scene_track_at(const Fl_Scene *scene, size_t index);
/* Borrow the Timeline track animating entity's property, or NULL. Valid until mutation. */
const Fl_Track *fl_scene_track(const Fl_Scene *scene, Fl_Id entity, Fl_AnimProperty property);
/* Borrow a Timeline keyframe by id with its track, or NULL. */
const Fl_Keyframe *fl_scene_key(const Fl_Scene *scene, Fl_Id key, const Fl_Track **track);
