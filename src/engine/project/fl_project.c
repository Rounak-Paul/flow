#include "fl_project.h"
#include "fl_timeline_internal.h"
#include "components/fl_component.h"
#include "components/fl_animation.h"
#include "components/fl_script.h"
#include "schema/fl_widgets.h"
#include "time/fl_scene_state.h"
#include "doc/fl_binary.h"
#include "core/fl_sha256.h"
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

#define MANIFEST_VERSION 3
#define SCENE_VERSION 6

struct Fl_EntityIndex { Fl_Id id; size_t position; };

typedef struct Fl_Retired { Fl_Id id; uint64_t revision; } Fl_Retired;
typedef struct Fl_RetiredAsset { unsigned char hash[32]; char extension[FL_ASSET_EXTENSION_MAX + 1]; } Fl_RetiredAsset;

struct Fl_Project {
    char *directory, *name;
    Fl_Scene *scenes;
    uint64_t *saved;
    size_t count;
    Fl_Retired *retired;
    size_t retired_count;
    Fl_Asset *assets;
    size_t asset_count;
    Fl_RetiredAsset *retired_assets;
    size_t retired_asset_count;
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
    for (size_t i = 0; i < entity->component_count; ++i) fl_component_clear((Fl_Component *)&entity->components[i]);
    free((void *)entity->components); free((void *)entity->name);
}

/* Release a scene's owned document storage. */
static void free_scene(Fl_Scene *scene)
{
    for (size_t i = 0; i < scene->entity_count; ++i) free_entity((Fl_Entity *)&scene->entities[i]);
    free((void *)scene->entities); free((void *)scene->identity_index); free((void *)scene->name);
    fl_timeline_free(scene);
}

/* Release one asset record's owned strings. */
static void free_asset(Fl_Asset *asset)
{
    free((void *)asset->name); free((void *)asset->extension);
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
bool fl_asset_extension_valid(const char *extension)
{
    if (!extension) return false;
    size_t length = 0;
    for (; extension[length]; ++length)
        if (length >= FL_ASSET_EXTENSION_MAX || !((extension[length] >= 'a' && extension[length] <= 'z') ||
            (extension[length] >= '0' && extension[length] <= '9'))) return false;
    return length > 0;
}

bool fl_asset_store_path(const char *directory, const unsigned char hash[32], const char *extension,
                         char *out, size_t capacity)
{
    if (out && capacity) out[0] = 0;
    if (!directory || !hash || !fl_asset_extension_valid(extension)) return false;
    char hex[65], leaf[96], folder[FL_PATH_MAX];
    fl_sha256_hex(hash, hex);
    snprintf(leaf, sizeof(leaf), "%s.%s", hex, extension);
    return fl_fs_join(folder, sizeof(folder), directory, "assets") && fl_fs_join(out, capacity, folder, leaf);
}

size_t fl_project_asset_count(const Fl_Project *p) { return p ? p->asset_count : 0; }
const Fl_Asset *fl_project_asset_at(const Fl_Project *p, size_t i) { return p && i < p->asset_count ? &p->assets[i] : NULL; }
const Fl_Asset *fl_project_asset(const Fl_Project *p, Fl_Id id)
{
    if (!p || !id) return NULL;
    size_t low = 0, high = p->asset_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (p->assets[mid].id == id) return &p->assets[mid];
        if (p->assets[mid].id < id) low = mid + 1; else high = mid;
    }
    return NULL;
}
const Fl_Asset *fl_project_asset_by_hash(const Fl_Project *p, const unsigned char hash[32])
{
    if (p && hash) for (size_t i = 0; i < p->asset_count; ++i) if (!memcmp(p->assets[i].hash, hash, 32)) return &p->assets[i];
    return NULL;
}
size_t fl_project_asset_uses(const Fl_Project *p, Fl_Id asset)
{
    size_t uses = 0;
    for (size_t i = 0; p && asset && i < p->count; ++i) uses += fl_timeline_asset_uses(&p->scenes[i], asset);
    for (size_t i = 0; p && asset && i < p->count; ++i)
        for (size_t j = 0; j < p->scenes[i].entity_count; ++j) {
            const Fl_Entity *e = &p->scenes[i].entities[j];
            for (size_t k = 0; k < e->component_count; ++k) {
                Fl_Id id;
                if (fl_component_asset_reference(&e->components[k], &id, NULL) && id == asset) ++uses;
            }
        }
    return uses;
}
bool fl_project_asset_path(const Fl_Project *p, const Fl_Asset *a, char *out, size_t capacity)
{
    return p && a && fl_asset_store_path(p->directory, a->hash, a->extension, out, capacity);
}

/* Require every asset reference in components to name an existing asset of the required kind. */
static bool references_valid(const Fl_Project *p, const Fl_Component *components, size_t count)
{
    for (size_t i = 0; components && i < count; ++i) {
        Fl_Id id; Fl_AssetKind kind;
        if (!fl_component_asset_reference(&components[i], &id, &kind)) continue;
        const Fl_Asset *asset = fl_project_asset(p, id);
        if (!asset || asset->kind != kind) return false;
    }
    return true;
}

/* Clone a component merge before committing a document mutation. */
static bool merge_components(const Fl_Entity *entity, const Fl_Component *updates, size_t count,
                             Fl_Component **out, size_t *out_count)
{
    *out = NULL; *out_count = 0;
    size_t existing = entity ? entity->component_count : 0;
    if ((count && !updates) || count > SIZE_MAX - existing ||
        existing + count > SIZE_MAX / sizeof(Fl_Component)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!fl_widget_component_valid(&updates[i])) return false;
        for (size_t j = 0; j < i; ++j) if (updates[j].type == updates[i].type) return false;
    }
    Fl_Component *copy = calloc(existing + count, sizeof(*copy));
    if (existing + count && !copy) return false;
    for (size_t i = 0; i < existing + count; ++i) {
        const Fl_Component *source = i < existing ? &entity->components[i] : &updates[i - existing];
        if (i < existing) {
            bool replaced = false;
            for (size_t j = 0; j < count; ++j) replaced |= updates[j].type == source->type;
            if (replaced) continue;
        }
        if (!fl_component_clone(source, &copy[*out_count])) {
            for (size_t j = 0; j < *out_count; ++j) fl_component_clear(&copy[j]);
            free(copy); *out_count = 0; return false;
        }
        ++*out_count;
    }
    for (size_t i = 0; i < *out_count; ++i) if (copy[i].type == FL_COMPONENT_TRANSFORM) {
        Fl_Component transform = copy[i];
        memmove(copy + 1, copy, i * sizeof(*copy)); copy[0] = transform; break;
    }
    *out = copy; return true;
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

/* Compare a proposed channel with its evaluated value at tick, ignoring numerical roundoff. */
static bool value_changed(const Fl_Scene *scene, const Fl_Track *track, Fl_Tick tick, const double value[4])
{
    double current[4];
    if (!fl_track_sample(scene, track, tick, current)) return true;
    int channels = track->property == FL_PROP_FILL || track->property == FL_PROP_STROKE ? 4 : 1;
    for (int c = 0; c < channels; ++c) {
        if (channels == 4) current[c] = lround(current[c]);
        if (fabs(current[c] - value[c]) > 1e-12 * fmax(1, fmax(fabs(current[c]), fabs(value[c])))) return true;
    }
    return false;
}

/* Split keyed configure components: animated channels become key writes and keep their base values.
   Rewritten components point at the caller's scratch buffers. Returns the number of writes. */
static size_t keyed_components(const Fl_Scene *s, const Fl_Entity *e, Fl_Component *components, size_t count,
                               unsigned char transform[56], unsigned char appearance[24], unsigned char opacity[8],
                               Fl_KeyWrite *writes, Fl_Tick tick)
{
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        Fl_Component *c = &components[i];
        if (c->type == FL_COMPONENT_TRANSFORM) {
            Fl_Transform next, base;
            if (!fl_transform_decode(c, &next) || !fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &base)) continue;
            double *value[7] = {&next.x, &next.y, &next.width, &next.height, &next.rotation, &next.scale_x, &next.scale_y};
            const double kept[7] = {base.x, base.y, base.width, base.height, base.rotation, base.scale_x, base.scale_y};
            for (int k = 0; k < 7; ++k) {
                if (fl_scene_track(s, e->id, (Fl_AnimProperty)(FL_PROP_X + k))) {
                    writes[n++] = (Fl_KeyWrite){e->id, (Fl_AnimProperty)(FL_PROP_X + k), {*value[k], 0, 0, 0}};
                    *value[k] = kept[k];
                }
            }
            fl_transform_encode(&next, transform); c->data = transform;
        } else if (c->type == FL_COMPONENT_APPEARANCE) {
            Fl_Appearance next, base = fl_widget_appearance(e);
            if (!fl_appearance_decode(c, &next)) continue;
            if (fl_scene_track(s, e->id, FL_PROP_FILL)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_FILL, {next.fill[0], next.fill[1], next.fill[2], next.fill[3]}};
                memcpy(next.fill, base.fill, 4);
            }
            if (fl_scene_track(s, e->id, FL_PROP_STROKE)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_STROKE, {next.stroke[0], next.stroke[1], next.stroke[2], next.stroke[3]}};
                memcpy(next.stroke, base.stroke, 4);
            }
            if (fl_scene_track(s, e->id, FL_PROP_STROKE_WIDTH)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_STROKE_WIDTH, {next.stroke_width, 0, 0, 0}}; next.stroke_width = base.stroke_width;
            }
            if (fl_scene_track(s, e->id, FL_PROP_FONT_SIZE)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_FONT_SIZE, {next.font_size, 0, 0, 0}}; next.font_size = base.font_size;
            }
            fl_appearance_encode(&next, appearance); c->data = appearance;
        } else if (c->type == FL_COMPONENT_OPACITY) {
            double next;
            if (!fl_opacity_decode(c, &next)) continue;
            if (fl_scene_track(s, e->id, FL_PROP_OPACITY)) writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_OPACITY, {next, 0, 0, 0}};
            else continue;
            fl_opacity_encode(fl_widget_opacity(e), opacity); c->data = opacity;
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) if (value_changed(s, fl_scene_track(s, e->id, writes[i].property), tick, writes[i].value))
        writes[kept++] = writes[i];
    return kept;
}

bool fl_project_apply(Fl_Project *p, const Fl_Op *op, Fl_Id *out_id)
{
    if (out_id) *out_id = 0;
    if (!p || !op || p->publishing || p->owner != fl_thread_current_id()) return false;
    Fl_Scene *s = (Fl_Scene *)fl_project_scene(p, op->scene);
    Fl_Entity *e = (Fl_Entity *)fl_scene_entity(s, op->entity);
    Fl_Id created = 0;
    if (op->kind == FL_OP_ENTITY_ADD || op->kind == FL_OP_ENTITY_CONFIGURE) {
        if (op->component_count && !op->components) return false;
        for (size_t i = 0; i < op->component_count; ++i) {
            if (op->components[i].type == FL_COMPONENT_ANIMATION) return false;
            if (op->components[i].type == FL_COMPONENT_SCRIPT) {
                Fl_Script script;
                if (!fl_script_decode(&op->components[i], &script)) return false;
            }
        }
    }
    if (op->keyed && (op->kind == FL_OP_TRANSFORMS || op->kind == FL_OP_ENTITY_CONFIGURE) &&
        (!s || op->tick < 0 || op->tick > fl_scene_length(s))) return false;
    if (s && s->revision == UINT64_MAX && op->kind != FL_OP_SCENE_REMOVE && op->kind != FL_OP_SCENE_MOVE) return false;
    if ((op->kind>=FL_OP_ANIMATION_STATE_SET&&op->kind<=FL_OP_ANIMATION_TRANSITION_REMOVE)||
        (op->animation_state&&(op->kind==FL_OP_KEY_SET||op->kind==FL_OP_KEY_REMOVE||op->kind==FL_OP_TRACK_REMOVE))) {
        if (!e) return false;
        const Fl_Animation *current=fl_entity_animation(e);
        Fl_Animation *draft=current ? fl_animation_clone(current) : calloc(1,sizeof(*draft));
        if (!draft) return false;
        uint64_t next=p->next_id;
        if (!fl_animation_apply(draft,op,&next,&created)||!fl_animation_valid(draft)) {fl_animation_destroy(draft);return false;}
        Fl_Component *component=(Fl_Component *)fl_entity_component(e,FL_COMPONENT_ANIMATION);
        if (!component) {
            if (e->component_count>=SIZE_MAX/sizeof(*component)-1) {fl_animation_destroy(draft);return false;}
            Fl_Component *components=realloc((void *)e->components,(e->component_count+1)*sizeof(*components));
            if (!components) {fl_animation_destroy(draft);return false;}
            e->components=components;component=&components[e->component_count++];*component=(Fl_Component){0};
        }
        fl_component_clear(component);*component=(Fl_Component){FL_COMPONENT_ANIMATION,2,sizeof(*draft),(unsigned char *)draft};
        p->next_id=next;
        goto applied;
    }
    switch (op->kind) {
    case FL_OP_SCENE_ADD: {
        if (!op->name || !op->name[0] || p->next_id >= UINT64_MAX - 1 || p->count >= SIZE_MAX / sizeof(*s) - 1) return false;
        char *name = copy_string(op->name), *beat_name = copy_string("Beat 1");
        Fl_Beat *beat = malloc(sizeof(*beat));
        Fl_Scene *scenes = malloc((p->count + 1) * sizeof(*scenes));
        uint64_t *saved = malloc((p->count + 1) * sizeof(*saved));
        if (!name || !beat_name || !beat || !scenes || !saved) { free(name); free(beat_name); free(beat); free(scenes); free(saved); return false; }
        if (p->count) { memcpy(scenes, p->scenes, p->count * sizeof(*scenes)); memcpy(saved, p->saved, p->count * sizeof(*saved)); }
        created = p->next_id++;
        *beat = (Fl_Beat){p->next_id++, beat_name, 600000, false};
        scenes[p->count] = (Fl_Scene){.id = created, .name = name, .revision = 1, .duration = 600000, .width = 1920, .height = 1080,
                                      .beats = beat, .beat_count = 1};
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
            !references_valid(p, op->components, op->component_count) ||
            p->next_id == UINT64_MAX || s->entity_count >= SIZE_MAX / sizeof(*e) - 1) return false;
        char *name = copy_string(op->name);
        unsigned char bytes[56];
        Fl_Transform t = {.width = 320, .height = 180, .scale_x = 1, .scale_y = 1};
        fl_transform_encode(&t, bytes);
        Fl_Component base = {FL_COMPONENT_TRANSFORM, 1, 56, bytes};
        Fl_Entity initial = {.components = &base, .component_count = 1}, prepared = {.name = name};
        Fl_Component *c = NULL;
        if (!name || !merge_components(&initial, op->components, op->component_count, &c, &prepared.component_count)) {
            free(name); return false;
        }
        prepared.components = c;
        if (!reserve_identity(s, s->entity_count + 1)) { free_entity(&prepared); return false; }
        Fl_Entity *entities = (Fl_Entity *)s->entities;
        if (s->entity_count == s->entity_capacity) {
            size_t capacity = s->entity_capacity ? s->entity_capacity : 8;
            if (capacity > SIZE_MAX / 2 / sizeof(*entities)) { free_entity(&prepared); return false; }
            capacity *= 2;
            entities = realloc(entities, capacity * sizeof(*entities));
            if (!entities) { free_entity(&prepared); return false; }
            s->entity_capacity = capacity;
        }
        created = p->next_id++;
        entities[s->entity_count++] = (Fl_Entity){created, op->parent, name, true, c, prepared.component_count};
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
        if (!e || !fl_widget_component_valid(&op->component) || !references_valid(p, &op->component, 1)) return false;
        if (op->component.type == FL_COMPONENT_ANIMATION) return false;
        if (op->component.type == FL_COMPONENT_SCRIPT) {
            Fl_Script script;
            if (!fl_script_decode(&op->component, &script)) return false;
        }
        Fl_Component copy = {0};
        if (!fl_component_clone(&op->component, &copy)) return false;
        Fl_Component *c = (Fl_Component *)fl_entity_component(e, copy.type);
        if (!c) {
            if (e->component_count >= SIZE_MAX / sizeof(*c) - 1) {fl_component_clear(&copy); return false;}
            Fl_Component *components = realloc((void *)e->components, (e->component_count + 1) * sizeof(*c));
            if (!components) {fl_component_clear(&copy); return false;}
            e->components = components; c = &components[e->component_count++]; *c = (Fl_Component){0};
        }
        fl_component_clear(c); *c = copy; break;
    }
    case FL_OP_COMPONENT_REMOVE:
        if (!fl_entity_remove_component(e, op->component.type)) return false;
        break;
    case FL_OP_ENTITY_CONFIGURE: {
        if (!e || !op->name || !op->name[0] || !references_valid(p, op->components, op->component_count) ||
            op->component_count > 64) return false;
        Fl_Component keyed[64];
        unsigned char transform_bytes[56], appearance_bytes[24], opacity_bytes[8];
        Fl_KeyWrite writes[FL_PROP_COUNT];
        size_t write_count = 0;
        const Fl_Component *components = op->components;
        if (op->keyed && op->component_count) {
            memcpy(keyed, op->components, op->component_count * sizeof(*keyed));
            components = keyed;
            write_count = keyed_components(s, e, keyed, op->component_count, transform_bytes, appearance_bytes, opacity_bytes, writes, op->tick);
        }
        Fl_Entity prepared = {.name = copy_string(op->name)};
        Fl_Component *merged = NULL;
        if (!prepared.name || !merge_components(e, components, op->component_count, &merged, &prepared.component_count)) {
            free((void *)prepared.name); return false;
        }
        prepared.components = merged; prepared.id = e->id; prepared.parent = e->parent; prepared.visible = op->visible;
        Fl_Entity previous = *e; *e = prepared;
        if (write_count && !fl_timeline_reserve(s, writes, write_count, p->next_id)) {
            free_entity(e); *e = previous; return false;
        }
        if (write_count) fl_timeline_commit(s, writes, write_count, op->tick, &p->next_id);
        free_entity(&previous);
        break;
    }
    case FL_OP_TRANSFORMS: {
        typedef struct Target { Fl_Component *component; unsigned char *bytes; } Target;
        if (!s || !op->transforms || !op->transform_count || op->transform_count > SIZE_MAX / sizeof(Target) / 7) return false;
        Target *targets = calloc(op->transform_count, sizeof(*targets));
        Fl_KeyWrite *writes = op->keyed ? calloc(op->transform_count * 7, sizeof(*writes)) : NULL;
        if (!targets || (op->keyed && !writes)) { free(targets); free(writes); return false; }
        bool valid = true;
        size_t write_count = 0;
        for (size_t i = 0; valid && i < op->transform_count; ++i) {
            const Fl_TransformEdit *edit = &op->transforms[i];
            for (size_t j = 0; j < i; ++j) if (op->transforms[j].entity == edit->entity) valid = false;
            Fl_Entity *entity = (Fl_Entity *)fl_scene_entity(s, edit->entity);
            targets[i].component = (Fl_Component *)fl_entity_component(entity, FL_COMPONENT_TRANSFORM);
            targets[i].bytes = malloc(56);
            if (!targets[i].component || !targets[i].bytes) { valid = false; break; }
            Fl_Transform base = edit->transform, current;
            if (op->keyed && fl_transform_decode(targets[i].component, &current)) {
                double *next[7] = {&base.x, &base.y, &base.width, &base.height, &base.rotation, &base.scale_x, &base.scale_y};
                const double now[7] = {current.x, current.y, current.width, current.height, current.rotation, current.scale_x, current.scale_y};
                for (int c = 0; c < 7; ++c) {
                            if (!fl_scene_track(s, edit->entity, (Fl_AnimProperty)(FL_PROP_X + c))) continue;
                    Fl_KeyWrite write = {edit->entity, (Fl_AnimProperty)(FL_PROP_X + c), {*next[c], 0, 0, 0}};
                    if (value_changed(s, fl_scene_track(s, edit->entity, write.property), op->tick, write.value))
                        writes[write_count++] = write;
                    *next[c] = now[c];
                }
            }
            fl_transform_encode(&base, targets[i].bytes);
            Fl_Component candidate = {FL_COMPONENT_TRANSFORM, 1, 56, targets[i].bytes};
            valid &= fl_widget_component_valid(&candidate);
        }
        if (valid && write_count) valid = fl_timeline_reserve(s, writes, write_count, p->next_id);
        for (size_t i = 0; i < op->transform_count; ++i) {
            if (valid) {
                free((void *)targets[i].component->data);
                *targets[i].component = (Fl_Component){FL_COMPONENT_TRANSFORM, 1, 56, targets[i].bytes};
            } else free(targets[i].bytes);
        }
        if (valid && write_count) fl_timeline_commit(s, writes, write_count, op->tick, &p->next_id);
        free(targets); free(writes); if (!valid) return false;
        break;
    }
    case FL_OP_ASSET_ADD: {
        const Fl_Asset *r = op->asset_record;
        if (!r || !r->name || !r->name[0] || r->kind < FL_ASSET_IMAGE || r->kind > FL_ASSET_FONT ||
            r->sample_rate > 768000 || r->channels > 64 ||
            !fl_asset_extension_valid(r->extension) || fl_project_asset_by_hash(p, r->hash) ||
            p->next_id == UINT64_MAX || p->asset_count >= SIZE_MAX / sizeof(Fl_Asset) - 1) return false;
        char *name = copy_string(r->name), *extension = copy_string(r->extension);
        Fl_Asset *assets = name && extension ? realloc(p->assets, (p->asset_count + 1) * sizeof(*assets)) : NULL;
        if (!assets) { free(name); free(extension); return false; }
        p->assets = assets;
        created = p->next_id++;
        Fl_Asset *a = &assets[p->asset_count++];
        *a = *r; a->id = created; a->name = name; a->extension = extension;
        break;
    }
    case FL_OP_ASSET_REMOVE: {
        Fl_Asset *a = (Fl_Asset *)fl_project_asset(p, op->asset);
        if (!a || fl_project_asset_uses(p, a->id) || p->retired_asset_count >= SIZE_MAX / sizeof(Fl_RetiredAsset) - 1) return false;
        Fl_RetiredAsset *retired = realloc(p->retired_assets, (p->retired_asset_count + 1) * sizeof(*retired));
        if (!retired) return false;
        p->retired_assets = retired;
        Fl_RetiredAsset *entry = &retired[p->retired_asset_count++];
        memcpy(entry->hash, a->hash, 32); snprintf(entry->extension, sizeof(entry->extension), "%s", a->extension);
        size_t index = (size_t)(a - p->assets);
        free_asset(a);
        memmove(a, a + 1, (p->asset_count - index - 1) * sizeof(*a)); --p->asset_count;
        break;
    }
    case FL_OP_ASSET_RENAME: {
        Fl_Asset *a = (Fl_Asset *)fl_project_asset(p, op->asset);
        if (!a || !op->name || !op->name[0]) return false;
        char *name = copy_string(op->name); if (!name) return false;
        free((void *)a->name); a->name = name; break;
    }
    case FL_OP_BEAT_ADD: case FL_OP_BEAT_REMOVE: case FL_OP_BEAT_UPDATE: case FL_OP_BEAT_MOVE: case FL_OP_BEAT_SPLIT:
    case FL_OP_KEY_SET: case FL_OP_KEY_REMOVE: case FL_OP_TRACK_REMOVE:
    case FL_OP_CLIP_ADD: case FL_OP_CLIP_UPDATE: case FL_OP_CLIP_REMOVE:
        if (!fl_timeline_apply(p, s, op, &p->next_id, &created)) return false;
        break;
    case FL_OP_SCENE_CAMERA:
        if (!s || !isfinite(op->camera_x) || !isfinite(op->camera_y)) return false;
        if (s->camera_x == op->camera_x && s->camera_y == op->camera_y) return true;
        s->camera_x = op->camera_x; s->camera_y = op->camera_y; break;
    case FL_OP_SCENE_ASPECT:
        if (!s || op->aspect_width > 65536 || op->aspect_height > 65536 ||
            (!op->aspect_width != !op->aspect_height)) return false;
        if (s->aspect_width == op->aspect_width && s->aspect_height == op->aspect_height) return true;
        s->aspect_width = op->aspect_width; s->aspect_height = op->aspect_height; break;
    case FL_OP_SCENE_SETTINGS: {
        if (!s || !op->width || !op->height || op->width > 65536 || op->height > 65536 || !op->duration || op->duration > INT64_MAX) return false;
        /* Beats partition the scene: a length change resizes the last beat. */
        Fl_Beat *last = s->beat_count ? (Fl_Beat *)&s->beats[s->beat_count - 1] : NULL;
        uint64_t others = last ? s->duration - last->duration : 0;
        if (last && op->duration <= others) return false;
        if (last && !fl_timeline_apply(p, s, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .beat = last->id,
            .name = last->name, .beat_duration = op->duration - others, .auto_advance = last->auto_advance}, &p->next_id, &created)) return false;
        s->width = op->width; s->height = op->height; s->duration = op->duration; break;
    }
    default: return false;
    }
applied:
    if (s) ++s->revision;
    p->dirty = true;
    if (out_id) *out_id = created;
    if (p->bus) {
        Fl_ProjectChange change = {p, op->kind, op->kind == FL_OP_SCENE_ADD ? created : op->scene,
                                   op->kind == FL_OP_ENTITY_ADD ? created : op->entity,
                                   op->kind == FL_OP_ASSET_ADD ? created : op->asset};
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
            fl_component_write(c, &b);
        }
    }
    fl_timeline_save(s, &b);
    fl_binary_u64(&b, s->aspect_width); fl_binary_u64(&b, s->aspect_height);
    const double camera[2] = {s->camera_x, s->camera_y};
    for (size_t i = 0; i < 2; ++i) { uint64_t bits; memcpy(&bits, &camera[i], sizeof(bits)); fl_binary_u64(&b, bits); }
    bool ok = project_path(p, s->id, s->revision, path) && fl_binary_save(&b, path, "FLSCENE\0", SCENE_VERSION);
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
    fl_binary_u64(&b, p->asset_count);
    for (size_t i = 0; i < p->asset_count; ++i) {
        const Fl_Asset *a = &p->assets[i];
        fl_binary_u64(&b, a->id); fl_binary_string(&b, a->name); fl_binary_u64(&b, (uint64_t)a->kind);
        fl_binary_write(&b, a->hash, 32); fl_binary_string(&b, a->extension);
        fl_binary_u64(&b, a->size); fl_binary_u64(&b, a->width); fl_binary_u64(&b, a->height);
        fl_binary_u64(&b, a->duration); fl_binary_u64(&b, a->sample_rate); fl_binary_u64(&b, a->channels);
    }
    bool ok = project_path(p, 0, 0, path) && fl_binary_save(&b, path, "FLPROJ\0\0", MANIFEST_VERSION);
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
        for (size_t i = 0; i < p->retired_asset_count; ++i) {
            const Fl_RetiredAsset *r = &p->retired_assets[i];
            if (!fl_project_asset_by_hash(p, r->hash) && fl_asset_store_path(p->directory, r->hash, r->extension, path, sizeof(path)))
                fl_fs_remove(path);
        }
        free(p->retired_assets); p->retired_assets = NULL; p->retired_asset_count = 0;
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
    char path[FL_PATH_MAX]; Fl_Binary b; uint64_t version;
    if (!project_path(p, s->id, s->revision, path) || !fl_binary_load(&b, path, "FLSCENE\0", SCENE_VERSION, &version)) return false;
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
            if (!fl_component_read(c, &b)) {b.failed = true; break;}
            if (c->type == FL_COMPONENT_ANIMATION) {
                Fl_Animation *animation = (Fl_Animation *)c->data;
                for (size_t k=0;k<animation->timeline.track_count;++k) animation->timeline.tracks[k].entity=e->id;
                for (size_t state=0;state<animation->state_count;++state) for (size_t k=0;k<animation->states[state].track_count;++k) animation->states[state].tracks[k].entity=e->id;
                if (version < 5) b.failed = true;
            }
            if (!fl_widget_component_valid(c)) b.failed = true;
            for (size_t k = 0; k < j; ++k) if (components[k].type == c->type) b.failed = true;
        }
        if (!b.failed && !fl_entity_component(e, FL_COMPONENT_TRANSFORM)) {
            if (version >= 5) b.failed = true;
            else {
                unsigned char bytes[56];
                Fl_Transform t = {.width = 320, .height = 180, .scale_x = 1, .scale_y = 1};
                fl_transform_encode(&t, bytes);
                Fl_Component base = {FL_COMPONENT_TRANSFORM, 1, 56, bytes}, *merged = NULL;
                size_t count = 0;
                if (!merge_components(e, &base, 1, &merged, &count)) b.failed = true;
                else {
                    for (size_t k = 0; k < e->component_count; ++k) fl_component_clear((Fl_Component *)&e->components[k]);
                    free((void *)e->components); e->components = merged; e->component_count = count;
                }
            }
        }
        for (size_t j = 0; !b.failed && j < e->component_count; ++j) if (e->components[j].type == FL_COMPONENT_TRANSFORM) {
            Fl_Component *all = (Fl_Component *)e->components, transform = all[j];
            memmove(all + 1, all, j * sizeof(*all)); all[0] = transform; break;
        }
    }
    if (!b.failed && !reserve_identity(s, s->entity_count)) b.failed = true;
    if (!b.failed) rebuild_identity(s);
    if (!b.failed && version >= 2 && !fl_timeline_load(s, &b, version < 5)) b.failed = true;
    if (!b.failed && version >= 3) {
        s->aspect_width = fl_binary_get_u64(&b); s->aspect_height = fl_binary_get_u64(&b);
        if (s->aspect_width > 65536 || s->aspect_height > 65536 ||
            (!s->aspect_width != !s->aspect_height)) b.failed = true;
    }
    if (!b.failed && version >= 4) {
        uint64_t bits = fl_binary_get_u64(&b); memcpy(&s->camera_x, &bits, sizeof(bits));
        bits = fl_binary_get_u64(&b); memcpy(&s->camera_y, &bits, sizeof(bits));
        if (!isfinite(s->camera_x) || !isfinite(s->camera_y)) b.failed = true;
    }
    bool ok = !b.failed && b.cursor == b.size && id == s->id && revision == s->revision &&
              s->name && s->name[0] && s->width && s->width <= 65536 && s->height && s->height <= 65536 &&
              s->duration && s->duration <= INT64_MAX;
    if (ok && version < 6) {
        if (s->revision == UINT64_MAX) ok = false;
        else {++s->revision; p->dirty = true;}
    }
    free(b.data); return ok;
}

/* Decode asset records in strictly ascending id order with unique content hashes. */
static void load_assets(Fl_Project *p, Fl_Binary *b, uint64_t version)
{
    p->asset_count = read_count(b, sizeof(Fl_Asset), version >= 3 ? 112 : 88);
    p->assets = p->asset_count ? calloc(p->asset_count, sizeof(*p->assets)) : NULL;
    if (p->asset_count && !p->assets) { p->asset_count = 0; b->failed = true; return; }
    for (size_t i = 0; !b->failed && i < p->asset_count; ++i) {
        Fl_Asset *a = &p->assets[i];
        a->id = fl_binary_get_u64(b); a->name = fl_binary_get_string(b);
        uint64_t kind = fl_binary_get_u64(b);
        fl_binary_read(b, a->hash, 32); a->extension = fl_binary_get_string(b);
        a->size = fl_binary_get_u64(b); a->width = fl_binary_get_u64(b); a->height = fl_binary_get_u64(b);
        uint64_t rate = 0, channels = 0;
        if (version >= 3) { a->duration = fl_binary_get_u64(b); rate = fl_binary_get_u64(b); channels = fl_binary_get_u64(b); }
        if (rate > 768000 || channels > 64) b->failed = true;
        a->sample_rate = (uint32_t)rate; a->channels = (uint32_t)channels;
        a->kind = kind >= FL_ASSET_IMAGE && kind <= FL_ASSET_FONT ? (Fl_AssetKind)kind : FL_ASSET_IMAGE;
        if (b->failed || !a->id || a->id >= p->next_id || (i && a->id <= p->assets[i - 1].id) || !a->name || !a->name[0] ||
            kind < FL_ASSET_IMAGE || kind > FL_ASSET_FONT || !fl_asset_extension_valid(a->extension)) b->failed = true;
        for (size_t j = 0; !b->failed && j < i; ++j) if (!memcmp(p->assets[j].hash, a->hash, 32)) b->failed = true;
    }
}

/* Parse assets/<64 hex>.<extension>; false for any other name. */
static bool parse_store_name(const char *name, unsigned char hash[32], const char **extension)
{
    for (size_t i = 0; i < 64; ++i) {
        char c = name[i];
        int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (value < 0) return false;
        if (i & 1) hash[i / 2] = (unsigned char)(hash[i / 2] | value); else hash[i / 2] = (unsigned char)(value << 4);
    }
    *extension = name + 65;
    return name[64] == '.' && fl_asset_extension_valid(*extension);
}

typedef struct Fl_StoreSweep { const Fl_Project *project; char folder[FL_PATH_MAX]; } Fl_StoreSweep;

/* Remove one unreferenced content-addressed file or abandoned import; other names are never touched. */
static bool sweep_entry(const char *name, bool directory, void *data)
{
    Fl_StoreSweep *sweep = data;
    if (directory) return true;
    unsigned char hash[32]; const char *extension;
    size_t length = strlen(name);
    bool abandoned = !strncmp(name, ".import-", 8) && length > 12 && !strcmp(name + length - 4, ".tmp");
    bool orphan = !abandoned && parse_store_name(name, hash, &extension);
    if (orphan) {
        const Fl_Asset *a = fl_project_asset_by_hash(sweep->project, hash);
        orphan = !a || strcmp(a->extension, extension);
    }
    char path[FL_PATH_MAX];
    if ((abandoned || orphan) && fl_fs_join(path, sizeof(path), sweep->folder, name)) fl_fs_remove(path);
    return true;
}

/* Remove cached thumbnails (<64 hex>-<side>.flthumb) whose asset no longer exists. */
static bool sweep_thumbnail(const char *name, bool directory, void *data)
{
    Fl_StoreSweep *sweep = data;
    unsigned char hash[32]; const char *rest; char probe[80];
    size_t length = strlen(name);
    if (directory || length < 66 || length > 79 || strcmp(name + length - 8, ".flthumb")) return true;
    memcpy(probe, name, 64); memcpy(probe + 64, ".flthumb", 9);
    char path[FL_PATH_MAX];
    if (parse_store_name(probe, hash, &rest) && name[64] == '-' && !fl_project_asset_by_hash(sweep->project, hash) &&
        fl_fs_join(path, sizeof(path), sweep->folder, name)) fl_fs_remove(path);
    return true;
}

/* Collect store files left by crashes or unsaved imports, and stale thumbnails, while the project lock is held. */
static void sweep_asset_store(const Fl_Project *p)
{
    Fl_StoreSweep sweep = {.project = p};
    if (fl_fs_join(sweep.folder, sizeof(sweep.folder), p->directory, "assets")) fl_fs_list_dir(sweep.folder, sweep_entry, &sweep);
    if (fl_fs_join(sweep.folder, sizeof(sweep.folder), p->directory, ".flow/cache/thumbnails")) fl_fs_list_dir(sweep.folder, sweep_thumbnail, &sweep);
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
    if (p->asset_count > SIZE_MAX - p->count) return false;
    size_t count = p->count + p->asset_count;
    for (size_t i = 0; i < p->count; ++i) {
        size_t timeline = fl_timeline_id_count(&p->scenes[i]);
        if (p->scenes[i].entity_count > SIZE_MAX - count || timeline > SIZE_MAX - count - p->scenes[i].entity_count) return false;
        count += p->scenes[i].entity_count + timeline;
        if (!fl_timeline_valid(p, &p->scenes[i], p->next_id)) return false;
    }
    if (count > SIZE_MAX / sizeof(Fl_Id)) return false;
    Fl_Id *ids = count ? malloc(count * sizeof(*ids)) : NULL;
    if (count && !ids) return false;
    size_t n = 0;
    for (size_t i = 0; i < p->asset_count; ++i) ids[n++] = p->assets[i].id;
    for (size_t i = 0; i < p->count; ++i) {
        const Fl_Scene *s = &p->scenes[i]; ids[n++] = s->id;
        fl_timeline_ids(s, ids + n); n += fl_timeline_id_count(s);
        unsigned char *state = s->entity_count ? calloc(s->entity_count, 1) : NULL;
        if (s->entity_count && !state) { free(ids); return false; }
        for (size_t j = 0; j < s->entity_count; ++j) {
            const Fl_Entity *e = &s->entities[j]; ids[n++] = e->id;
            if (!references_valid(p, e->components, e->component_count) || !fl_entity_component(e, FL_COMPONENT_TRANSFORM)) { free(state); free(ids); return false; }
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
    for (size_t i=0;i<count;++i) if (!ids[i]||ids[i]>=p->next_id||(i&&ids[i]==ids[i-1])) ok=false;
    free(ids); return ok;
}

Fl_Project *fl_project_open(const char *directory, Fl_EventBus *bus)
{
    char path[FL_PATH_MAX];
    if (!directory || !fl_fs_join(path, sizeof(path), directory, "project.flow") || !fl_fs_exists(path)) return NULL;
    Fl_Project *p = allocate_project(directory, bus);
    if (!p) return NULL;
    Fl_Binary b; uint64_t version;
    if (!fl_binary_load(&b, path, "FLPROJ\0\0", MANIFEST_VERSION, &version)) { fl_project_destroy(p); return NULL; }
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
    if (!b.failed && version >= 2) load_assets(p, &b, version);
    bool ok = !b.failed && b.cursor == b.size && p->name && p->name[0] && p->next_id && validate_project(p);
    free(b.data);
    if (!ok) { fl_project_destroy(p); return NULL; }
    for (size_t i = 0; i < p->count; ++i) {
        bool changed;
        if (!fl_timeline_repair(&p->scenes[i], &p->next_id, &changed)) {fl_project_destroy(p); return NULL;}
        p->dirty |= changed;
    }
    sweep_asset_store(p);
    return p;
}

void fl_project_destroy(Fl_Project *p)
{
    if (!p || p->publishing || p->owner != fl_thread_current_id()) return;
    for (size_t i = 0; i < p->count; ++i) free_scene(&p->scenes[i]);
    for (size_t i = 0; i < p->asset_count; ++i) free_asset(&p->assets[i]);
    free(p->scenes); free(p->saved); free(p->retired); free(p->directory); free(p->name);
    free(p->assets); free(p->retired_assets);
    if (p->lock) fclose(p->lock);
    free(p);
}
