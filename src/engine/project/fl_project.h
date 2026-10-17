#pragma once
#include "core/fl_event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t Fl_Id;
typedef struct Fl_Project Fl_Project;

/* Component schema ids are persistent; unknown schemas are preserved verbatim. */
enum { FL_COMPONENT_TRANSFORM = 1, FL_COMPONENT_RECTANGLE = 2, FL_COMPONENT_TEXT = 3 };
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
} Fl_Scene;

typedef enum Fl_OpKind {
    FL_OP_SCENE_ADD, FL_OP_SCENE_REMOVE, FL_OP_SCENE_RENAME, FL_OP_SCENE_MOVE,
    FL_OP_ENTITY_ADD, FL_OP_ENTITY_REMOVE, FL_OP_ENTITY_RENAME, FL_OP_ENTITY_PARENT,
    FL_OP_ENTITY_MOVE, FL_OP_ENTITY_VISIBLE, FL_OP_COMPONENT_SET, FL_OP_COMPONENT_REMOVE,
    FL_OP_SCENE_SETTINGS
} Fl_OpKind;
/* Operations reference stable ids; position is a scene/entity array insertion index. */
typedef struct Fl_Op {
    Fl_OpKind kind;
    Fl_Id scene, entity, parent;
    const char *name;
    size_t position;
    bool visible;
    Fl_Component component;
    uint64_t width, height, duration;
} Fl_Op;
#define FL_EVENT_PROJECT_CHANGED "project.changed"
typedef struct Fl_ProjectChange { Fl_Project *project; Fl_OpKind kind; Fl_Id scene, entity; } Fl_ProjectChange;

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
/* Return whether project has changes beyond its committed manifest. */
bool fl_project_dirty(const Fl_Project *project);
/* Encode/decode the v1 transform component; no native struct bytes reach disk. */
void fl_transform_encode(const Fl_Transform *transform, unsigned char out[56]);
bool fl_transform_decode(const Fl_Component *component, Fl_Transform *out);
