#include "fl_project.h"
#include "doc/fl_binary.h"
#include "core/fl_thread.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

struct Fl_EntityIndex { Fl_Id id; size_t position; };

typedef struct Fl_Retired { Fl_Id id; uint64_t revision; } Fl_Retired;

struct Fl_Project {
    char *directory, *name;
    Fl_Scene *scenes;
    uint64_t *saved;
    size_t count;
    Fl_Retired *retired;
    size_t retired_count;
    uint64_t next_id, owner;
    Fl_EventBus *bus;
    Fl_EventType changed;
    FILE *lock;
    bool dirty, publishing;
};

/* Duplicate a validated caller string into owned storage. */
static char *copy_string(const char *text)
{
    if (!text) return NULL;
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}

/* Release one entity and all of its component payloads. */
static void free_entity(Fl_Entity *entity)
{
    for (size_t i = 0; i < entity->component_count; ++i) free((void *)entity->components[i].data);
    free((void *)entity->components); free((void *)entity->name);
}

/* Release a scene's owned document storage. */
static void free_scene(Fl_Scene *scene)
{
    for (size_t i = 0; i < scene->entity_count; ++i) free_entity((Fl_Entity *)&scene->entities[i]);
    free((void *)scene->entities); free((void *)scene->identity_index); free((void *)scene->name);
}

/* Build a manifest or immutable scene snapshot path without accepting file names from disk. */
static bool project_path(const Fl_Project *p, Fl_Id id, uint64_t revision, char out[FL_PATH_MAX])
{
    char leaf[96];
    if (id) snprintf(leaf, sizeof(leaf), "scenes/%016llx-%016llx.flscene",
                     (unsigned long long)id, (unsigned long long)revision);
    else snprintf(leaf, sizeof(leaf), "project.flow");
    return fl_fs_join(out, FL_PATH_MAX, p->directory, leaf);
}

/* Acquire an OS-backed writer lock; a crashed process releases it automatically. */
static bool lock_project(Fl_Project *p)
{
    char path[FL_PATH_MAX];
    if (!fl_fs_join(path, sizeof(path), p->directory, ".flow") || !fl_fs_mkdir_p(path) ||
        !fl_fs_join(path, sizeof(path), p->directory, ".flow/lock")) return false;
    p->lock = fl_fs_fopen(path, "a+b");
    if (!p->lock) return false;
#ifdef _WIN32
    OVERLAPPED offset = {0};
    bool ok = LockFileEx((HANDLE)_get_osfhandle(_fileno(p->lock)),
                         LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset) != 0;
#else
    bool ok = flock(fileno(p->lock), LOCK_EX | LOCK_NB) == 0;
#endif
    if (!ok) { fclose(p->lock); p->lock = NULL; }
    return ok;
}

/* Allocate the project host and register its document change event. */
static Fl_Project *allocate_project(const char *directory, Fl_EventBus *bus)
{
    if (!directory || !directory[0]) return NULL;
    Fl_Project *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->owner = fl_thread_current_id();
    char absolute[FL_PATH_MAX];
    p->directory = fl_fs_absolute(directory, absolute, sizeof(absolute)) ? copy_string(absolute) : NULL; p->next_id = 1; p->bus = bus;
    p->changed = bus ? fl_event_type_register(bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange)) : 0;
    if (!p->directory || (bus && p->changed == FL_EVENT_TYPE_INVALID) || !lock_project(p)) {
        fl_project_destroy(p); return NULL;
    }
    return p;
}

const char *fl_project_name(const Fl_Project *p) { return p ? p->name : NULL; }
const char *fl_project_directory(const Fl_Project *p) { return p ? p->directory : NULL; }
size_t fl_project_scene_count(const Fl_Project *p) { return p ? p->count : 0; }
bool fl_project_dirty(const Fl_Project *p) { return p && p->dirty; }
const Fl_Scene *fl_project_scene_at(const Fl_Project *p, size_t i) { return p && i < p->count ? &p->scenes[i] : NULL; }
const Fl_Scene *fl_project_scene(const Fl_Project *p, Fl_Id id)
{
    if (p) for (size_t i = 0; i < p->count; ++i) if (p->scenes[i].id == id) return &p->scenes[i];
    return NULL;
}
/* Mix stable ids into a power-of-two identity table. */
static size_t identity_hash(Fl_Id id, size_t capacity)
{
    id ^= id >> 30; id *= UINT64_C(0xbf58476d1ce4e5b9);
    id ^= id >> 27; id *= UINT64_C(0x94d049bb133111eb);
    return (size_t)(id ^ (id >> 31)) & (capacity - 1);
}

/* Reserve lookup capacity before document mutations, so rebuilding cannot fail. */
static bool reserve_identity(Fl_Scene *s, size_t count)
{
    if (count > SIZE_MAX / 2) return false;
    size_t capacity = s->identity_capacity ? s->identity_capacity : 8;
    while (capacity / 2 < count) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    if (capacity == s->identity_capacity) return true;
    if (capacity > SIZE_MAX / sizeof(Fl_EntityIndex)) return false;
    Fl_EntityIndex *index = calloc(capacity, sizeof(*index));
    if (!index) return false;
    for (size_t i = 0; i < s->entity_count; ++i) {
        size_t slot = identity_hash(s->entities[i].id, capacity);
        while (index[slot].id) slot = (slot + 1) & (capacity - 1);
        index[slot] = (Fl_EntityIndex){s->entities[i].id, i};
    }
    free((void *)s->identity_index); s->identity_index = index; s->identity_capacity = capacity;
    return true;
}

/* Rebuild identity positions after ordered entity storage moves. */
static void rebuild_identity(Fl_Scene *s)
{
    Fl_EntityIndex *index = (Fl_EntityIndex *)s->identity_index;
    memset(index, 0, s->identity_capacity * sizeof(*index));
    for (size_t i = 0; i < s->entity_count; ++i) {
        size_t slot = identity_hash(s->entities[i].id, s->identity_capacity);
        while (index[slot].id) slot = (slot + 1) & (s->identity_capacity - 1);
        index[slot] = (Fl_EntityIndex){s->entities[i].id, i};
    }
}

const Fl_Entity *fl_scene_entity(const Fl_Scene *s, Fl_Id id)
{
    if (!s || !id || !s->identity_capacity) return NULL;
    size_t slot = identity_hash(id, s->identity_capacity);
    for (size_t i = 0; i < s->identity_capacity; ++i) {
        const Fl_EntityIndex *entry = &s->identity_index[slot];
        if (!entry->id) return NULL;
        if (entry->id == id) return &s->entities[entry->position];
        slot = (slot + 1) & (s->identity_capacity - 1);
    }
    return NULL;
}
const Fl_Component *fl_entity_component(const Fl_Entity *e, uint64_t type)
{
    if (e) for (size_t i = 0; i < e->component_count; ++i) if (e->components[i].type == type) return &e->components[i];
    return NULL;
}

void fl_transform_encode(const Fl_Transform *t, unsigned char out[56])
{
    const double values[7] = {t->x, t->y, t->width, t->height, t->rotation, t->scale_x, t->scale_y};
    for (size_t i = 0; i < 7; ++i) {
        uint64_t bits; memcpy(&bits, &values[i], 8);
        for (unsigned b = 0; b < 8; ++b) out[i * 8 + b] = (unsigned char)(bits >> (b * 8));
    }
}

bool fl_transform_decode(const Fl_Component *c, Fl_Transform *out)
{
    if (!c || !out || c->type != FL_COMPONENT_TRANSFORM || c->version != 1 || c->size != 56 || !c->data) return false;
    double v[7];
    for (size_t i = 0; i < 7; ++i) {
        uint64_t bits = 0;
        for (unsigned b = 0; b < 8; ++b) bits |= (uint64_t)c->data[i * 8 + b] << (b * 8);
        memcpy(&v[i], &bits, 8);
        if (!isfinite(v[i])) return false;
    }
    if (v[2] < 0 || v[3] < 0) return false;
    *out = (Fl_Transform){v[0], v[1], v[2], v[3], v[4], v[5], v[6]}; return true;
}

/* Validate built-in schemas while retaining unknown versioned components unchanged. */
static bool valid_component(const Fl_Component *c)
{
    if (!c->type || !c->version || (c->size && !c->data)) return false;
    if (c->type <= FL_COMPONENT_TEXT && c->version != 1) return false;
    if (c->type == FL_COMPONENT_TRANSFORM) { Fl_Transform t; return fl_transform_decode(c, &t); }
    if (c->type == FL_COMPONENT_RECTANGLE) return c->size == 4;
    if (c->type == FL_COMPONENT_TEXT) return c->size && !memchr(c->data, 0, c->size);
    return true;
}

/* Return whether candidate is a descendant of ancestor in the validated hierarchy. */
static bool descendant(const Fl_Scene *s, Fl_Id candidate, Fl_Id ancestor)
{
    for (size_t depth = 0; candidate && depth <= s->entity_count; ++depth) {
        if (candidate == ancestor) return true;
        const Fl_Entity *e = fl_scene_entity(s, candidate);
        if (!e) return false;
        candidate = e->parent;
    }
    return false;
}

bool fl_project_apply(Fl_Project *p, const Fl_Op *op, Fl_Id *out_id)
{
    if (out_id) *out_id = 0;
    if (!p || !op || p->publishing || p->owner != fl_thread_current_id()) return false;
    Fl_Scene *s = (Fl_Scene *)fl_project_scene(p, op->scene);
    Fl_Entity *e = (Fl_Entity *)fl_scene_entity(s, op->entity);
    Fl_Id created = 0;
    if (s && s->revision == UINT64_MAX && op->kind != FL_OP_SCENE_REMOVE && op->kind != FL_OP_SCENE_MOVE) return false;
    switch (op->kind) {
    case FL_OP_SCENE_ADD: {
        if (!op->name || !op->name[0] || p->next_id == UINT64_MAX || p->count >= SIZE_MAX / sizeof(*s) - 1) return false;
        char *name = copy_string(op->name);
        Fl_Scene *scenes = malloc((p->count + 1) * sizeof(*scenes));
        uint64_t *saved = malloc((p->count + 1) * sizeof(*saved));
        if (!name || !scenes || !saved) { free(name); free(scenes); free(saved); return false; }
        if (p->count) { memcpy(scenes, p->scenes, p->count * sizeof(*scenes)); memcpy(saved, p->saved, p->count * sizeof(*saved)); }
        created = p->next_id++;
        scenes[p->count] = (Fl_Scene){.id = created, .name = name, .revision = 1, .duration = 600000, .width = 1920, .height = 1080};
        saved[p->count++] = 0;
        free(p->scenes); free(p->saved); p->scenes = scenes; p->saved = saved;
        s = NULL; break;
    }
    case FL_OP_SCENE_REMOVE: {
        if (!s) return false;
        size_t index = (size_t)(s - p->scenes);
        if (p->saved[index]) {
            if (p->retired_count >= SIZE_MAX / sizeof(Fl_Retired) - 1) return false;
            Fl_Retired *retired = realloc(p->retired, (p->retired_count + 1) * sizeof(*retired));
            if (!retired) return false;
            p->retired = retired; p->retired[p->retired_count++] = (Fl_Retired){s->id, p->saved[index]};
        }
        free_scene(s);
        memmove(s, s + 1, (p->count - index - 1) * sizeof(*s));
        memmove(p->saved + index, p->saved + index + 1, (p->count - index - 1) * sizeof(*p->saved));
        --p->count; s = NULL; break;
    }
    case FL_OP_SCENE_MOVE: {
        if (!s || op->position >= p->count) return false;
        size_t from = (size_t)(s - p->scenes), to = op->position;
        Fl_Scene value = *s; uint64_t saved = p->saved[from];
        if (from < to) { memmove(s, s + 1, (to - from) * sizeof(*s)); memmove(p->saved + from, p->saved + from + 1, (to - from) * sizeof(*p->saved)); }
        else { memmove(p->scenes + to + 1, p->scenes + to, (from - to) * sizeof(*s)); memmove(p->saved + to + 1, p->saved + to, (from - to) * sizeof(*p->saved)); }
        p->scenes[to] = value; p->saved[to] = saved; s = NULL; break;
    }
    case FL_OP_SCENE_RENAME:
    case FL_OP_ENTITY_RENAME: {
        if (!s || (op->kind == FL_OP_ENTITY_RENAME && !e) || !op->name || !op->name[0]) return false;
        char *name = copy_string(op->name); if (!name) return false;
        const char **field = op->kind == FL_OP_SCENE_RENAME ? &s->name : &e->name;
        free((void *)*field); *field = name; break;
    }
    case FL_OP_ENTITY_ADD: {
        if (!s || !op->name || !op->name[0] || (op->parent && !fl_scene_entity(s, op->parent)) ||
            p->next_id == UINT64_MAX || s->entity_count >= SIZE_MAX / sizeof(*e) - 1) return false;
        char *name = copy_string(op->name);
        Fl_Component *c = malloc(sizeof(*c)); unsigned char *bytes = malloc(56);
        if (!name || !c || !bytes) { free(name); free(c); free(bytes); return false; }
        Fl_Transform t = {.width = 320, .height = 180, .scale_x = 1, .scale_y = 1};
        fl_transform_encode(&t, bytes); *c = (Fl_Component){FL_COMPONENT_TRANSFORM, 1, 56, bytes};
        if (!reserve_identity(s, s->entity_count + 1)) { free(name); free(c); free(bytes); return false; }
        Fl_Entity *entities = (Fl_Entity *)s->entities;
        if (s->entity_count == s->entity_capacity) {
            size_t capacity = s->entity_capacity ? s->entity_capacity : 8;
            if (capacity > SIZE_MAX / 2 / sizeof(*entities)) { free(name); free(c); free(bytes); return false; }
            capacity *= 2;
            entities = realloc(entities, capacity * sizeof(*entities));
            if (!entities) { free(name); free(c); free(bytes); return false; }
            s->entity_capacity = capacity;
        }
        created = p->next_id++;
        entities[s->entity_count++] = (Fl_Entity){created, op->parent, name, true, c, 1};
        s->entities = entities;
        size_t slot = identity_hash(created, s->identity_capacity);
        Fl_EntityIndex *index = (Fl_EntityIndex *)s->identity_index;
        while (index[slot].id) slot = (slot + 1) & (s->identity_capacity - 1);
        index[slot] = (Fl_EntityIndex){created, s->entity_count - 1};
        break;
    }
    case FL_OP_ENTITY_REMOVE: {
        if (!e) return false;
        /* Mark the subtree before compacting, preserving parent lookup until all marks exist. */
        unsigned char *remove = calloc(s->entity_count, 1);
        if (!remove) return false;
        remove[e - s->entities] = 2;
        for (size_t i = 0; i < s->entity_count; ++i) {
            const Fl_Entity *current = &s->entities[i];
            while (current && !remove[current - s->entities]) current = fl_scene_entity(s, current->parent);
            unsigned char result = current ? remove[current - s->entities] : 1;
            current = &s->entities[i];
            while (current && !remove[current - s->entities]) {
                remove[current - s->entities] = result;
                current = fl_scene_entity(s, current->parent);
            }
        }
        size_t count = 0; Fl_Entity *entities = (Fl_Entity *)s->entities;
        for (size_t i = 0; i < s->entity_count; ++i) {
            if (remove[i] == 2) free_entity(&entities[i]); else entities[count++] = entities[i];
        }
        s->entity_count = count; rebuild_identity(s); free(remove); break;
    }
    case FL_OP_ENTITY_PARENT:
        if (!e || (op->parent && !fl_scene_entity(s, op->parent)) || descendant(s, op->parent, e->id)) return false;
        e->parent = op->parent; break;
    case FL_OP_ENTITY_MOVE: {
        if (!e || op->position >= s->entity_count) return false;
        Fl_Entity *entities = (Fl_Entity *)s->entities, value = *e;
        size_t from = (size_t)(e - entities), to = op->position;
        if (from < to) memmove(e, e + 1, (to - from) * sizeof(*e));
        else memmove(entities + to + 1, entities + to, (from - to) * sizeof(*e));
        entities[to] = value; rebuild_identity(s); break;
    }
    case FL_OP_ENTITY_VISIBLE:
        if (!e) return false;
        e->visible = op->visible; break;
    case FL_OP_COMPONENT_SET: {
        if (!e || !valid_component(&op->component)) return false;
        unsigned char *bytes = op->component.size ? malloc(op->component.size) : NULL;
        if (op->component.size && !bytes) return false;
        if (bytes) memcpy(bytes, op->component.data, op->component.size);
        Fl_Component *c = (Fl_Component *)fl_entity_component(e, op->component.type);
        if (!c) {
            if (e->component_count >= SIZE_MAX / sizeof(*c) - 1) { free(bytes); return false; }
            Fl_Component *components = realloc((void *)e->components, (e->component_count + 1) * sizeof(*c));
            if (!components) { free(bytes); return false; }
            e->components = components; c = &components[e->component_count++]; *c = (Fl_Component){0};
        }
        free((void *)c->data); *c = op->component; c->data = bytes; break;
    }
    case FL_OP_COMPONENT_REMOVE: {
        Fl_Component *c = (Fl_Component *)fl_entity_component(e, op->component.type);
        if (!c) return false;
        size_t index = (size_t)(c - e->components); free((void *)c->data);
        memmove(c, c + 1, (e->component_count - index - 1) * sizeof(*c)); --e->component_count; break;
    }
    case FL_OP_SCENE_SETTINGS:
        if (!s || !op->width || !op->height || op->width > 65536 || op->height > 65536 || !op->duration || op->duration > INT64_MAX) return false;
        s->width = op->width; s->height = op->height; s->duration = op->duration; break;
    default: return false;
    }
    if (s) ++s->revision;
    p->dirty = true;
    if (out_id) *out_id = created;
    if (p->bus) {
        Fl_ProjectChange change = {p, op->kind, op->kind == FL_OP_SCENE_ADD ? created : op->scene,
                                   op->kind == FL_OP_ENTITY_ADD ? created : op->entity};
        p->publishing = true; fl_event_publish(p->bus, p->changed, &change, sizeof(change)); p->publishing = false;
    }
    return true;
}

/* Encode one independently replaceable scene document. */
static bool save_scene(const Fl_Project *p, const Fl_Scene *s)
{
    Fl_Binary b = {0}; char path[FL_PATH_MAX];
    fl_binary_u64(&b, s->id); fl_binary_u64(&b, s->revision); fl_binary_string(&b, s->name);
    fl_binary_u64(&b, s->width); fl_binary_u64(&b, s->height); fl_binary_u64(&b, s->duration);
    fl_binary_u64(&b, s->entity_count);
    for (size_t i = 0; i < s->entity_count; ++i) {
        const Fl_Entity *e = &s->entities[i];
        fl_binary_u64(&b, e->id); fl_binary_u64(&b, e->parent); fl_binary_string(&b, e->name);
        fl_binary_u64(&b, e->visible); fl_binary_u64(&b, e->component_count);
        for (size_t j = 0; j < e->component_count; ++j) {
            const Fl_Component *c = &e->components[j];
            fl_binary_u64(&b, c->type); fl_binary_u64(&b, c->version); fl_binary_u64(&b, c->size);
            fl_binary_write(&b, c->data, c->size);
        }
    }
    bool ok = project_path(p, s->id, s->revision, path) && fl_binary_save(&b, path, "FLSCENE\0");
    free(b.data); return ok;
}

bool fl_project_save(Fl_Project *p)
{
    if (!p || p->publishing || p->owner != fl_thread_current_id()) return false;
    if (!p->dirty) return true;
    char path[FL_PATH_MAX];
    if (!fl_fs_join(path, sizeof(path), p->directory, "scenes") || !fl_fs_mkdir_p(path)) return false;
    for (size_t i = 0; i < p->count; ++i)
        if (p->saved[i] != p->scenes[i].revision && !save_scene(p, &p->scenes[i])) return false;
    Fl_Binary b = {0};
    fl_binary_string(&b, p->name); fl_binary_u64(&b, p->next_id); fl_binary_u64(&b, p->count);
    for (size_t i = 0; i < p->count; ++i) { fl_binary_u64(&b, p->scenes[i].id); fl_binary_u64(&b, p->scenes[i].revision); }
    bool ok = project_path(p, 0, 0, path) && fl_binary_save(&b, path, "FLPROJ\0\0");
    free(b.data);
    if (ok) {
        p->dirty = false;
        for (size_t i = 0; i < p->count; ++i) {
            if (p->saved[i] && p->saved[i] != p->scenes[i].revision &&
                project_path(p, p->scenes[i].id, p->saved[i], path)) fl_fs_remove(path);
            p->saved[i] = p->scenes[i].revision;
        }
        for (size_t i = 0; i < p->retired_count; ++i)
            if (project_path(p, p->retired[i].id, p->retired[i].revision, path)) fl_fs_remove(path);
        free(p->retired); p->retired = NULL; p->retired_count = 0;
    }
    return ok;
}

Fl_Project *fl_project_create(const char *directory, const char *name, Fl_EventBus *bus)
{
    if (!name || !name[0] || !directory || !fl_fs_mkdir_p(directory)) return NULL;
    Fl_Project *p = allocate_project(directory, bus);
    if (!p) return NULL;
    char path[FL_PATH_MAX];
    if (!project_path(p, 0, 0, path) || fl_fs_exists(path)) { fl_project_destroy(p); return NULL; }
    p->name = copy_string(name); p->dirty = true;
    const char *folders[] = {"assets", "components", "exports", ".flow/cache"};
    bool ok = p->name != NULL;
    for (size_t i = 0; ok && i < sizeof(folders) / sizeof(folders[0]); ++i)
        ok = fl_fs_join(path, sizeof(path), directory, folders[i]) && fl_fs_mkdir_p(path);
    if (!ok || !fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_ADD, .name = "Scene 1"}, NULL) || !fl_project_save(p)) {
        fl_project_destroy(p); return NULL;
    }
    return p;
}

/* Decode a count only when the remaining bytes can contain that many minimum-size records. */
static size_t read_count(Fl_Binary *b, size_t stride, size_t minimum)
{
    uint64_t count = fl_binary_get_u64(b);
    if (b->failed || count > SIZE_MAX / stride || count > (b->size - b->cursor) / minimum) { b->failed = true; return 0; }
    return (size_t)count;
}

/* Decode and validate component and entity records without trusting stored sizes. */
static bool load_scene(Fl_Project *p, Fl_Scene *s)
{
    char path[FL_PATH_MAX]; Fl_Binary b;
    if (!project_path(p, s->id, s->revision, path) || !fl_binary_load(&b, path, "FLSCENE\0")) return false;
    uint64_t id = fl_binary_get_u64(&b), revision = fl_binary_get_u64(&b);
    s->name = fl_binary_get_string(&b);
    s->width = fl_binary_get_u64(&b); s->height = fl_binary_get_u64(&b); s->duration = fl_binary_get_u64(&b);
    s->entity_count = read_count(&b, sizeof(Fl_Entity), 40);
    Fl_Entity *entities = s->entity_count ? calloc(s->entity_count, sizeof(*entities)) : NULL;
    if (s->entity_count && !entities) b.failed = true;
    s->entities = entities; s->entity_capacity = s->entity_count;
    if (!entities) s->entity_count = 0;
    for (size_t i = 0; !b.failed && i < s->entity_count; ++i) {
        Fl_Entity *e = &entities[i];
        e->id = fl_binary_get_u64(&b); e->parent = fl_binary_get_u64(&b); e->name = fl_binary_get_string(&b);
        uint64_t visible = fl_binary_get_u64(&b); e->visible = visible != 0;
        e->component_count = read_count(&b, sizeof(Fl_Component), 24);
        Fl_Component *components = e->component_count ? calloc(e->component_count, sizeof(*components)) : NULL;
        if (e->component_count && !components) b.failed = true;
        e->components = components;
        if (!components) e->component_count = 0;
        if (!e->id || e->id >= p->next_id || !e->name || !e->name[0] || visible > 1) b.failed = true;
        for (size_t j = 0; !b.failed && j < e->component_count; ++j) {
            Fl_Component *c = &components[j];
            c->type = fl_binary_get_u64(&b); c->version = fl_binary_get_u64(&b);
            c->size = read_count(&b, 1, 1);
            unsigned char *data = c->size ? malloc(c->size) : NULL; c->data = data;
            if (c->size && !data) { b.failed = true; break; }
            fl_binary_read(&b, data, c->size);
            if (!valid_component(c)) b.failed = true;
            for (size_t k = 0; k < j; ++k) if (components[k].type == c->type) b.failed = true;
        }
    }
    bool ok = !b.failed && b.cursor == b.size && id == s->id && revision == s->revision &&
              s->name && s->name[0] && s->width && s->width <= 65536 && s->height && s->height <= 65536 &&
              s->duration && s->duration <= INT64_MAX;
    if (ok) ok = reserve_identity(s, s->entity_count);
    free(b.data); return ok;
}

/* Sort identity keys for duplicate detection across all scene documents. */
static int compare_ids(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Validate identities and hierarchy; reject dangling parents and cycles before exposing the project. */
static bool validate_project(const Fl_Project *p)
{
    size_t count = p->count;
    for (size_t i = 0; i < p->count; ++i) {
        if (p->scenes[i].entity_count > SIZE_MAX - count) return false;
        count += p->scenes[i].entity_count;
    }
    if (count > SIZE_MAX / sizeof(Fl_Id)) return false;
    Fl_Id *ids = count ? malloc(count * sizeof(*ids)) : NULL;
    if (count && !ids) return false;
    size_t n = 0;
    for (size_t i = 0; i < p->count; ++i) {
        const Fl_Scene *s = &p->scenes[i]; ids[n++] = s->id;
        unsigned char *state = s->entity_count ? calloc(s->entity_count, 1) : NULL;
        if (s->entity_count && !state) { free(ids); return false; }
        for (size_t j = 0; j < s->entity_count; ++j) {
            const Fl_Entity *e = &s->entities[j]; ids[n++] = e->id;
            const Fl_Entity *current = e;
            while (current && !state[current - s->entities]) {
                state[current - s->entities] = 1;
                if (!current->parent) { current = NULL; break; }
                current = fl_scene_entity(s, current->parent);
                if (!current) { free(state); free(ids); return false; }
            }
            if (current && state[current - s->entities] == 1) { free(state); free(ids); return false; }
            current = e;
            while (current && state[current - s->entities] == 1) {
                state[current - s->entities] = 2;
                current = fl_scene_entity(s, current->parent);
            }
        }
        free(state);
    }
    if (count) qsort(ids, count, sizeof(*ids), compare_ids);
    bool ok = true;
    for (size_t i = 1; i < count; ++i) if (ids[i] == ids[i - 1]) ok = false;
    free(ids); return ok;
}

Fl_Project *fl_project_open(const char *directory, Fl_EventBus *bus)
{
    char path[FL_PATH_MAX];
    if (!directory || !fl_fs_join(path, sizeof(path), directory, "project.flow") || !fl_fs_exists(path)) return NULL;
    Fl_Project *p = allocate_project(directory, bus);
    if (!p) return NULL;
    Fl_Binary b;
    if (!fl_binary_load(&b, path, "FLPROJ\0\0")) { fl_project_destroy(p); return NULL; }
    p->name = fl_binary_get_string(&b); p->next_id = fl_binary_get_u64(&b);
    p->count = read_count(&b, sizeof(Fl_Scene), 16);
    p->scenes = p->count ? calloc(p->count, sizeof(*p->scenes)) : NULL;
    p->saved = p->count ? calloc(p->count, sizeof(*p->saved)) : NULL;
    if (p->count && (!p->scenes || !p->saved)) { b.failed = true; if (!p->scenes) p->count = 0; }
    for (size_t i = 0; !b.failed && i < p->count; ++i) {
        Fl_Scene *s = &p->scenes[i];
        s->id = fl_binary_get_u64(&b); s->revision = fl_binary_get_u64(&b); p->saved[i] = s->revision;
        if (!s->id || s->id >= p->next_id || !s->revision || !load_scene(p, s)) b.failed = true;
    }
    bool ok = !b.failed && b.cursor == b.size && p->name && p->name[0] && p->next_id && validate_project(p);
    free(b.data);
    if (!ok) { fl_project_destroy(p); return NULL; }
    return p;
}

void fl_project_destroy(Fl_Project *p)
{
    if (!p || p->publishing || p->owner != fl_thread_current_id()) return;
    for (size_t i = 0; i < p->count; ++i) free_scene(&p->scenes[i]);
    free(p->scenes); free(p->saved); free(p->retired); free(p->directory); free(p->name);
    if (p->lock) fclose(p->lock);
    free(p);
}
