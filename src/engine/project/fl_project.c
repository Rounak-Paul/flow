#include "fl_resource_snapshot.h"
#include "time/fl_composition.h"
#include "components/fl_prototype_instance.h"
#include <stdatomic.h>
#include "fl_project.h"
#include "fl_scene_storage.h"
#include "fl_source_asset.h"
#include "fl_prototype.h"
#include "project/fl_history.h"
#include "fl_timeline_internal.h"
#include "fl_manuscript.h"
#include "components/fl_component.h"
#include "components/fl_animation.h"
#include "components/fl_script.h"
#include "schema/fl_widgets.h"
#include "scripting/fl_script_runtime.h"
#include "time/fl_scene_state.h"
#include "doc/fl_binary.h"
#include "doc/fl_document.h"
#include "doc/fl_document_store.h"
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




typedef struct Fl_Retired { Fl_Id id; uint64_t revision; } Fl_Retired;
typedef struct Fl_RetiredAsset { unsigned char hash[32]; char extension[FL_ASSET_EXTENSION_MAX + 1]; } Fl_RetiredAsset;

typedef struct Fl_ProjectFile {
    char path[FL_PATH_MAX];
    unsigned char hash[32];
    bool existed;
} Fl_ProjectFile;

struct Fl_Project {
    Fl_History *history;
    Fl_Project *edit_backup;
    bool editing, interactive, edit_dirty;
    uint64_t edit_generation;
    Fl_ProjectChange *changes;
    size_t change_count, change_capacity;
    Fl_ProjectFile *files;
    size_t file_count;
    char diagnostic[256];
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
    uint64_t next_id, next_component_id, owner;
    Fl_EventBus *bus;
    Fl_EventType changed;
    FILE *lock;
    uint64_t generation, identity;
    bool dirty, publishing, read_only;
};

/* Duplicate a validated caller string into owned storage. */
static char *copy_string(const char *text)
{
    if (!text || !fl_json_utf8_valid(text,strlen(text))) return NULL;
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}

/* Release one asset record's owned strings. */
static void free_asset(Fl_Asset *asset)
{
    free((void *)asset->name); free((void *)asset->extension); free((void *)asset->path); free((void *)asset->source); free((void *)asset->references);
}

/* Deep-copy a common asset record, including editable source; partial copies are always releasable. */
static bool clone_asset(const Fl_Asset *from, Fl_Asset *to)
{
    *to=*from; to->name=copy_string(from->name); to->extension=copy_string(from->extension);
    to->path=from->path ? copy_string(from->path) : NULL; to->source=from->source ? copy_string(from->source) : NULL;
    to->references=from->reference_count ? malloc(from->reference_count*sizeof(*from->references)) : NULL;
    if (to->references) memcpy((void *)to->references,from->references,from->reference_count*sizeof(*from->references));
    return to->name && to->extension && (!from->path || to->path) && (!from->source || to->source) && (!from->reference_count || to->references);
}

/* Build a manifest or immutable scene snapshot path without accepting file names from disk. */
static bool project_path(const Fl_Project *p, Fl_Id id, uint64_t revision, char out[FL_PATH_MAX])
{
    char leaf[96];
    (void)revision;
    if (id) snprintf(leaf, sizeof(leaf), "scenes/%016llx.scene.json", (unsigned long long)id);
    else snprintf(leaf, sizeof(leaf), "project.flow.json");
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

static atomic_uint_fast64_t project_identity=1;

/* Reserve a host lifetime without allowing queued/cache identities to wrap. */
static uint64_t reserve_project_identity(void)
{
    uint_fast64_t value=atomic_load_explicit(&project_identity,memory_order_relaxed);
    while (value!=UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(&project_identity,&value,value+1,memory_order_relaxed,memory_order_relaxed)) return value;
    }
    return 0;
}

/* Allocate the project host and register its document change event. */
static Fl_Project *allocate_project(const char *directory, Fl_EventBus *bus)
{
    if (!directory || !directory[0]) return NULL;
    Fl_Project *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->owner = fl_thread_current_id(); p->identity=reserve_project_identity();
    char absolute[FL_PATH_MAX];
    p->directory = fl_fs_absolute(directory, absolute, sizeof(absolute)) ? copy_string(absolute) : NULL; p->next_id = 1; p->next_component_id=1; p->bus = bus;
    p->changed = bus ? fl_event_type_register(bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange)) : 0;
    if (!p->identity || !p->directory || (bus && p->changed == FL_EVENT_TYPE_INVALID) || !lock_project(p)) {
        fl_project_destroy(p); return NULL;
    }
    p->history=fl_history_create(64,256u*1024u*1024u);
    if (!p->history) {fl_project_destroy(p); return NULL;}
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
typedef struct AssetUses {Fl_Id asset; size_t count;} AssetUses;
/* Count references without resolving or reparsing the reusable source catalog. */
static bool count_asset_use(Fl_Id asset, Fl_AssetKind kind, void *data)
{
    (void)kind; AssetUses *uses=data;
    if (asset==uses->asset) {if (uses->count==SIZE_MAX) return false; ++uses->count;}
    return true;
}
/* Validate all semantic resource references, including prototype override inputs. */
static bool asset_reference_valid(Fl_Id id, Fl_AssetKind kind, void *data)
{
    const Fl_Asset *asset=fl_project_asset(data,id);
    return asset && (!kind || asset->kind==kind);
}

size_t fl_project_asset_uses(const Fl_Project *p, Fl_Id asset)
{
    size_t uses = 0;
    for (size_t i = 0; p && asset && i < p->count; ++i)
        uses += fl_timeline_asset_uses(&p->scenes[i], asset) + fl_manuscript_asset_uses(&p->scenes[i], asset);
    for (size_t i = 0; p && asset && i < p->count; ++i)
        for (size_t j = 0; j < p->scenes[i].entity_count; ++j) {
            const Fl_Entity *e = &p->scenes[i].entities[j];
            for (size_t k = 0; k < e->component_count; ++k) {
                AssetUses count={asset,uses};
                if (!fl_component_visit_assets(&e->components[k],count_asset_use,&count)) return SIZE_MAX;
                uses=count.count;
            }
        }
    for (size_t i=0;p && asset && i<p->asset_count;++i)
        for (size_t j=0;j<p->assets[i].reference_count;++j) uses+=p->assets[i].references[j]==asset;
    return uses;
}
bool fl_project_asset_path(const Fl_Project *p, const Fl_Asset *a, char *out, size_t capacity)
{
    return p && a && (a->path ? fl_fs_join(out,capacity,p->directory,a->path) : fl_asset_store_path(p->directory, a->hash, a->extension, out, capacity));
}

/* Require every asset reference in components to name an existing asset of the required kind. */
static bool references_valid(const Fl_Project *p, const Fl_Scene *s, const Fl_Component *components, size_t count)
{
    (void)s;
    for (size_t i = 0; components && i < count; ++i) {
        if (!fl_component_visit_assets(&components[i],asset_reference_valid,(void *)p)) return false;
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
        existing + count > SIZE_MAX / sizeof(Fl_Component) || count>64) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!fl_widget_component_valid(&updates[i])) return false;
        for (size_t j = 0; j < i; ++j) if ((updates[j].type==updates[i].type && updates[i].type!=FL_COMPONENT_SCRIPT) || (updates[i].id && updates[j].id==updates[i].id)) return false;
        if (updates[i].id) {const Fl_Component *old=fl_entity_component_instance(entity,updates[i].id); if (!old || old->type!=updates[i].type) return false;}
    }
    Fl_Component *copy = calloc(existing + count, sizeof(*copy));
    if (existing + count && !copy) return false;
    for (size_t i = 0; i < existing + count; ++i) {
        const Fl_Component *source = i < existing ? &entity->components[i] : &updates[i - existing];
        if (i < existing) {
            bool replaced = false;
            for (size_t j = 0; j < count; ++j) replaced |= updates[j].id ? updates[j].id==source->id : updates[j].type!=FL_COMPONENT_SCRIPT && updates[j].type==source->type;
            if (replaced) continue;
        }
        if (!fl_component_clone(source, &copy[*out_count])) {
            for (size_t j = 0; j < *out_count; ++j) fl_component_clear(&copy[j]);
            free(copy); *out_count = 0; return false;
        }
        if (i>=existing && !copy[*out_count].id && source->type!=FL_COMPONENT_SCRIPT) {
            const Fl_Component *previous=fl_entity_component(entity,source->type);
            if (previous) copy[*out_count].id=previous->id;
        }
        ++*out_count;
    }
    for (size_t i = 0; i < *out_count; ++i) if (copy[i].type == FL_COMPONENT_POSE) {
        Fl_Component transform = copy[i];
        memmove(copy + 1, copy, i * sizeof(*copy)); copy[0] = transform; break;
    }
    if (*out_count>64) {for (size_t i=0;i<*out_count;++i) fl_component_clear(&copy[i]); free(copy); *out_count=0; return false;}
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

const Fl_Asset *fl_project_asset_named(const Fl_Project *p, Fl_AssetKind kind, const char *name)
{
    for (size_t i=0;p && name && i<p->asset_count;++i) if (p->assets[i].kind==kind && !strcmp(p->assets[i].name,name)) return &p->assets[i];
    return NULL;
}

/* Apply an editable source asset change before validating final project dependencies at commit. */
static bool apply_source_asset(Fl_Project *p, const Fl_Op *op, Fl_Id *created)
{
    const Fl_SourceFormat *format=fl_source_asset_format(op->source_kind);
    if (!format) return false;
    const Fl_Asset *existing=fl_project_asset(p,op->asset);
    if (op->asset && (!existing || existing->kind!=op->source_kind)) return false;
    for (size_t i=0;i<p->count;++i) if (p->scenes[i].revision>=UINT64_MAX-1) return false;
    if (!op->name && !op->source) return false;
    const Fl_Asset *named=op->name ? fl_project_asset_named(p,op->source_kind,op->name) : NULL;
    if (op->name && (!fl_source_asset_name_valid(op->name) || (named && named->id!=op->asset))) return false;
    if (!existing && (!op->name || !op->source || p->next_id==UINT64_MAX)) return false;
    Fl_SourceInfo info={0};
    if (op->source && !fl_source_asset_compile(op->source_kind,op->source,op->source_size,&info,p->diagnostic,sizeof(p->diagnostic))) return false;
    char *name=op->name ? copy_string(op->name) : NULL, *source=NULL;
    if (op->source) {source=malloc(op->source_size+1); if (source) {memcpy(source,op->source,op->source_size); source[op->source_size]=0;}}
    if ((op->name && !name) || (op->source && !source)) {free(name); free(source); free(info.references); return false;}
    Fl_Asset *a=(Fl_Asset *)existing;
    if (!a) {
        char relative[96]; snprintf(relative,sizeof(relative),"%s/%016llx.%s",format->directory,(unsigned long long)p->next_id,format->suffix);
        char *path=copy_string(relative), *extension=copy_string(format->extension);
        Fl_Asset *assets=path && extension ? realloc(p->assets,(p->asset_count+1)*sizeof(*assets)) : NULL;
        if (!assets) {free(name); free(source); free(path); free(extension); free(info.references); return false;}
        p->assets=assets; a=&assets[p->asset_count++]; *a=(Fl_Asset){.id=p->next_id++,.kind=op->source_kind,.path=path,.extension=extension}; *created=a->id;
    }
    if (name) {free((void *)a->name); a->name=name;}
    if (source) {
        free((void *)a->source); a->source=source; a->size=op->source_size;
        free((void *)a->references); a->references=info.references; a->reference_count=info.reference_count;
        Fl_Sha256 hash; fl_sha256_init(&hash); fl_sha256_update(&hash,source,op->source_size); fl_sha256_final(&hash,a->hash);
    }
    for (size_t i=0;i<p->count;++i) ++p->scenes[i].revision;
    return true;
}

static bool apply_operation(Fl_Project *p, const Fl_Op *op, Fl_Id *out_id)
{
    if (out_id) *out_id = 0;
    if (!p || !op || p->publishing || p->generation>=UINT64_MAX-1 || p->owner != fl_thread_current_id()) return false;
    Fl_Scene *s = (Fl_Scene *)fl_project_scene(p, op->scene);
    Fl_Entity *e = (Fl_Entity *)fl_scene_entity(s, op->entity);
    Fl_Id created = 0;
    if (op->kind == FL_OP_ENTITY_ADD || op->kind == FL_OP_ENTITY_CONFIGURE) {
        if (op->component_count>64 || (op->component_count && !op->components)) return false;
        for (size_t i = 0; i < op->component_count; ++i) {
            if (op->components[i].type == FL_COMPONENT_ANIMATION) return false;
            if (op->components[i].type == FL_COMPONENT_SCRIPT) {
                Fl_Script script;
                if (!fl_script_decode(&op->components[i], &script)) return false;
            }
        }
    }
    size_t component_bound=0;
    if (op->kind==FL_OP_ENTITY_ADD || op->kind==FL_OP_ENTITY_CONFIGURE) component_bound=op->component_count+2;
    else if (op->kind==FL_OP_COMPONENT_SET || op->kind==FL_OP_KEY_SET) component_bound=1;
    else if (op->kind==FL_OP_TRANSFORMS) component_bound=op->transform_count;
    if (component_bound>UINT64_MAX-p->next_component_id) return false;
    if (op->keyed && (op->kind == FL_OP_TRANSFORMS || op->kind == FL_OP_ENTITY_CONFIGURE) &&
        (!s || op->tick < 0 || op->tick > fl_scene_length(s))) return false;
    if (s && s->revision >= UINT64_MAX-1 && op->kind != FL_OP_SCENE_REMOVE && op->kind != FL_OP_SCENE_MOVE) return false;
    switch (op->kind) {
    case FL_OP_SCENE_ADD: {
        if (!op->name || !op->name[0] || p->next_id >= UINT64_MAX - 1 || p->count >= SIZE_MAX / sizeof(*s) - 1) return false;
        char *name = copy_string(op->name), *beat_name = copy_string("Beat 1");
        char *voice = copy_string("Ryan"), *language = copy_string("English");
        Fl_Beat *beat = malloc(sizeof(*beat));
        Fl_Scene *scenes = malloc((p->count + 1) * sizeof(*scenes));
        uint64_t *saved = malloc((p->count + 1) * sizeof(*saved));
        if (!name || !beat_name || !voice || !language || !beat || !scenes || !saved) {
            free(name); free(beat_name); free(voice); free(language); free(beat); free(scenes); free(saved); return false;
        }
        if (p->count) { memcpy(scenes, p->scenes, p->count * sizeof(*scenes)); memcpy(saved, p->saved, p->count * sizeof(*saved)); }
        created = p->next_id++;
        *beat = (Fl_Beat){p->next_id++, beat_name, 600000, false};
        scenes[p->count] = (Fl_Scene){.project=p, .id = created, .name = name, .revision = 1, .duration = 600000, .width = 1920, .height = 1080,
                                      .beats = beat, .beat_count = 1, .voice = voice, .language = language, .background = FL_SCENE_BACKGROUND_DEFAULT};
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
        fl_scene_storage_clear(s);
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
            !references_valid(p, s, op->components, op->component_count) ||
            p->next_id == UINT64_MAX || s->entity_count >= SIZE_MAX / sizeof(*e) - 1) return false;
        char *name = copy_string(op->name);
        unsigned char pose_bytes[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES];
        Fl_Pose pose = fl_pose_identity();
        Fl_Bounds bounds = {320, 180, 0};
        fl_pose_encode(&pose, pose_bytes); fl_bounds_encode(&bounds, bounds_bytes);
        Fl_Component base[2] = {{.type = FL_COMPONENT_POSE, .version = 1, .size = FL_POSE_BYTES, .data = pose_bytes},
                                {.type = FL_COMPONENT_BOUNDS, .version = 1, .size = FL_BOUNDS_BYTES, .data = bounds_bytes}};
        Fl_Entity initial = {.components = base, .component_count = 2}, prepared = {.name = name};
        Fl_Component *c = NULL;
        if (!name || !merge_components(&initial, op->components, op->component_count, &c, &prepared.component_count)) {
            free(name); return false;
        }
        prepared.components = c;
        if (!fl_scene_storage_reserve_index(s, s->entity_count + 1)) { fl_entity_storage_clear(&prepared); return false; }
        Fl_Entity *entities = (Fl_Entity *)s->entities;
        if (s->entity_count == s->entity_capacity) {
            size_t capacity = s->entity_capacity ? s->entity_capacity : 8;
            if (capacity > SIZE_MAX / 2 / sizeof(*entities)) { fl_entity_storage_clear(&prepared); return false; }
            capacity *= 2;
            entities = realloc(entities, capacity * sizeof(*entities));
            if (!entities) { fl_entity_storage_clear(&prepared); return false; }
            s->entity_capacity = capacity;
        }
        created = p->next_id++;
        entities[s->entity_count++] = (Fl_Entity){created, op->parent, name, true, c, prepared.component_count};
        s->entities = entities;
        fl_scene_storage_index_entity(s,s->entity_count-1);
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
            if (remove[i] == 2) fl_entity_storage_clear(&entities[i]); else entities[count++] = entities[i];
        }
        s->entity_count = count; fl_scene_storage_rebuild_index(s); free(remove); break;
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
        entities[to] = value; fl_scene_storage_rebuild_index(s); break;
    }
    case FL_OP_ENTITY_VISIBLE:
        if (!e) return false;
        e->visible = op->visible; break;
    case FL_OP_COMPONENT_SET: {
        if (!e || !fl_widget_component_valid(&op->component) || !references_valid(p, s, &op->component, 1)) return false;
        if (op->component.type == FL_COMPONENT_ANIMATION) return false;
        if (op->component.type == FL_COMPONENT_SCRIPT) {
            Fl_Script script;
            if (!fl_script_decode(&op->component, &script)) return false;
        }
        Fl_Component copy = {0};
        if (!fl_component_clone(&op->component, &copy)) return false;
        Fl_Component *c=(Fl_Component *)(copy.id ? fl_entity_component_instance(e,copy.id) : copy.type==FL_COMPONENT_SCRIPT ? NULL : fl_entity_component(e,copy.type));
        if (copy.id && (!c || (c->type!=copy.type && !(c->type==FL_COMPONENT_PROTOTYPE && copy.type==FL_COMPONENT_COMPOSITION)))) {fl_component_clear(&copy); return false;}
        if (!c) {
            if (e->component_count >= 64 || e->component_count >= SIZE_MAX / sizeof(*c) - 1) {fl_component_clear(&copy); return false;}
            Fl_Component *components = realloc((void *)e->components, (e->component_count + 1) * sizeof(*c));
            if (!components) {fl_component_clear(&copy); return false;}
            e->components = components; c = &components[e->component_count++]; *c = (Fl_Component){0};
        }
        copy.id=c->id; fl_component_clear(c); *c = copy; created=copy.id; break;
    }
    case FL_OP_COMPONENT_REMOVE:
        if (!(op->component.id ? fl_entity_remove_component_instance(e,op->component.id) : fl_entity_remove_component(e,op->component.type))) return false;
        break;
    case FL_OP_ENTITY_CONFIGURE: {
        if (!e || !op->name || !op->name[0] || !references_valid(p, s, op->components, op->component_count) ||
            op->component_count > 64) return false;
        Fl_Component keyed[64];
        unsigned char pose_bytes[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], appearance_bytes[24], opacity_bytes[8], style_bytes[FL_STYLE_BYTES], camera_bytes[FL_CAMERA_BYTES];
        Fl_KeyWrite writes[FL_PROP_COUNT];
        size_t write_count = 0;
        const Fl_Component *components = op->components;
        if (op->keyed && op->component_count) {
            memcpy(keyed, op->components, op->component_count * sizeof(*keyed));
            components = keyed;
            write_count = fl_keyed_components(s, e, keyed, op->component_count, pose_bytes, bounds_bytes, appearance_bytes, opacity_bytes, style_bytes, camera_bytes, writes, op->tick);
            if (write_count==SIZE_MAX) return false;
        }
        Fl_Entity prepared = {.name = copy_string(op->name)};
        Fl_Component *merged = NULL;
        if (!prepared.name || !merge_components(e, components, op->component_count, &merged, &prepared.component_count)) {
            free((void *)prepared.name); return false;
        }
        prepared.components = merged; prepared.id = e->id; prepared.parent = e->parent; prepared.visible = op->visible;
        Fl_Entity previous = *e; *e = prepared;
        if (write_count && !fl_timeline_reserve(s, writes, write_count, p->next_id)) {
            fl_entity_storage_clear(e); *e = previous; return false;
        }
        if (write_count) fl_timeline_commit(s, writes, write_count, op->tick, &p->next_id);
        fl_entity_storage_clear(&previous);
        break;
    }
    case FL_OP_TRANSFORMS: {
        typedef struct Target { Fl_Component *pose, *bounds; unsigned char *pose_bytes, *bounds_bytes; } Target;
        if (!s || !op->transforms || !op->transform_count || op->transform_count > SIZE_MAX / sizeof(Target) / 16) return false;
        Target *targets = calloc(op->transform_count, sizeof(*targets));
        Fl_KeyWrite *writes = op->keyed ? calloc(op->transform_count * 16, sizeof(*writes)) : NULL;
        if (!targets || (op->keyed && !writes)) { free(targets); free(writes); return false; }
        bool valid = true;
        size_t write_count = 0;
        static const Fl_AnimProperty pose_channels[] = {FL_PROP_X, FL_PROP_Y, FL_PROP_Z, FL_PROP_RX, FL_PROP_RY, FL_PROP_RZ, FL_PROP_SCALE_X, FL_PROP_SCALE_Y, FL_PROP_SCALE_Z, FL_PROP_PIVOT_X, FL_PROP_PIVOT_Y, FL_PROP_PIVOT_Z};
        static const Fl_AnimProperty bounds_channels[] = {FL_PROP_WIDTH, FL_PROP_HEIGHT, FL_PROP_DEPTH};
        for (size_t i = 0; valid && i < op->transform_count; ++i) {
            const Fl_TransformEdit *edit = &op->transforms[i];
            for (size_t j = 0; j < i; ++j) if (op->transforms[j].entity == edit->entity) valid = false;
            Fl_Entity *entity = (Fl_Entity *)fl_scene_entity(s, edit->entity);
            targets[i].pose = (Fl_Component *)fl_entity_component(entity, FL_COMPONENT_POSE);
            targets[i].bounds = (Fl_Component *)fl_entity_component(entity, FL_COMPONENT_BOUNDS);
            targets[i].pose_bytes = malloc(FL_POSE_BYTES); targets[i].bounds_bytes = malloc(FL_BOUNDS_BYTES);
            if (!targets[i].pose || !targets[i].pose_bytes || !targets[i].bounds_bytes) { valid = false; break; }
            Fl_Pose pose = edit->pose; Fl_Bounds bounds = edit->bounds, current_bounds = fl_entity_bounds(entity); Fl_Pose current;
            if (op->keyed && fl_pose_decode(targets[i].pose, &current)) {
                double *next_pose[12] = {&pose.x,&pose.y,&pose.z,&pose.rx,&pose.ry,&pose.rz,&pose.sx,&pose.sy,&pose.sz,&pose.px,&pose.py,&pose.pz};
                const double now_pose[12] = {current.x,current.y,current.z,current.rx,current.ry,current.rz,current.sx,current.sy,current.sz,current.px,current.py,current.pz};
                double *next_bounds[3] = {&bounds.width,&bounds.height,&bounds.depth};
                const double now_bounds[3] = {current_bounds.width,current_bounds.height,current_bounds.depth};
                for (int c = 0; c < 12; ++c) {
                    if (!fl_scene_track(s, edit->entity, pose_channels[c])) continue;
                    Fl_KeyWrite write = {edit->entity, pose_channels[c], {*next_pose[c], 0, 0, 0}};
                    if (fl_keyed_value_changed(s, fl_scene_track(s, edit->entity, write.property), op->tick, write.value)) writes[write_count++] = write;
                    *next_pose[c] = now_pose[c];
                }
                for (int c = 0; targets[i].bounds && c < 3; ++c) {
                    if (!fl_scene_track(s, edit->entity, bounds_channels[c])) continue;
                    Fl_KeyWrite write = {edit->entity, bounds_channels[c], {*next_bounds[c], 0, 0, 0}};
                    if (fl_keyed_value_changed(s, fl_scene_track(s, edit->entity, write.property), op->tick, write.value)) writes[write_count++] = write;
                    *next_bounds[c] = now_bounds[c];
                }
            }
            fl_pose_encode(&pose, targets[i].pose_bytes);
            fl_bounds_encode(&bounds, targets[i].bounds_bytes);
            Fl_Component candidate = {.type = FL_COMPONENT_POSE, .version = 1, .size = FL_POSE_BYTES, .data = targets[i].pose_bytes};
            Fl_Component bounds_candidate = {.type = FL_COMPONENT_BOUNDS, .version = 1, .size = FL_BOUNDS_BYTES, .data = targets[i].bounds_bytes};
            valid &= fl_widget_component_valid(&candidate) && fl_widget_component_valid(&bounds_candidate);
        }
        if (valid && write_count) valid = fl_timeline_reserve(s, writes, write_count, p->next_id);
        for (size_t i = 0; i < op->transform_count; ++i) {
            if (valid) {
                free((void *)targets[i].pose->data);
                *targets[i].pose = (Fl_Component){.type = FL_COMPONENT_POSE, .version = 1, .size = FL_POSE_BYTES, .data = targets[i].pose_bytes, .id=targets[i].pose->id};
                if (targets[i].bounds) {
                    free((void *)targets[i].bounds->data);
                    *targets[i].bounds = (Fl_Component){.type = FL_COMPONENT_BOUNDS, .version = 1, .size = FL_BOUNDS_BYTES, .data = targets[i].bounds_bytes, .id=targets[i].bounds->id};
                } else free(targets[i].bounds_bytes);
            } else {free(targets[i].pose_bytes); free(targets[i].bounds_bytes);}
        }
        if (valid && write_count) fl_timeline_commit(s, writes, write_count, op->tick, &p->next_id);
        free(targets); free(writes); if (!valid) return false;
        break;
    }
    case FL_OP_ASSET_ADD: {
        const Fl_Asset *r = op->asset_record;
        if (!r || !r->name || !r->name[0] || r->kind < FL_ASSET_IMAGE || r->kind > FL_ASSET_FONT || r->path || r->source || r->references || r->reference_count ||
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
        if (fl_source_asset_format(a->kind)) {
            const Fl_Asset *existing = fl_project_asset_named(p, a->kind, op->name);
            if (!fl_source_asset_name_valid(op->name) || (existing && existing->id != a->id)) return false;
        }
        char *name = copy_string(op->name); if (!name) return false;
        free((void *)a->name); a->name = name; break;
    }
    case FL_OP_BEAT_ADD: case FL_OP_BEAT_REMOVE: case FL_OP_BEAT_UPDATE: case FL_OP_BEAT_MOVE: case FL_OP_BEAT_SPLIT:
    case FL_OP_KEY_SET: case FL_OP_KEY_REMOVE: case FL_OP_TRACK_REMOVE:
    case FL_OP_CLIP_ADD: case FL_OP_CLIP_UPDATE: case FL_OP_CLIP_REMOVE:
        if (!fl_timeline_apply(p, s, op, &p->next_id, &created)) return false;
        if (op->kind == FL_OP_BEAT_REMOVE) fl_manuscript_detach_beat(s, op->beat);
        if (op->kind == FL_OP_CLIP_REMOVE) fl_manuscript_detach_clip(s, op->clip);
        break;
    case FL_OP_PHASE_ADD: case FL_OP_PHASE_REMOVE: case FL_OP_PHASE_UPDATE: case FL_OP_PHASE_MOVE: case FL_OP_VOICE_SET: case FL_OP_PHASE_CUES: case FL_OP_CUE_UPDATE:
        if (!s || !fl_manuscript_apply(p, s, op, &p->next_id, &created)) return false;
        break;
    case FL_OP_ASSET_SOURCE_SET:
        if (!apply_source_asset(p,op,&created)) return false;
        s=NULL;
        break;
    case FL_OP_SCENE_CAMERA:
        if (!s || !isfinite(op->camera_x) || !isfinite(op->camera_y)) return false;
        if (s->camera_x == op->camera_x && s->camera_y == op->camera_y) return true;
        s->camera_x = op->camera_x; s->camera_y = op->camera_y; break;
    case FL_OP_SCENE_BACKGROUND:
        if (!s || op->background[3] != 255) return false;
        if (!memcmp(s->background, op->background, 4)) return true;
        memcpy(s->background, op->background, 4); break;
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
    if (s) {
        Fl_Id target=op->kind==FL_OP_ENTITY_ADD ? created : op->entity;
        size_t targets=op->kind==FL_OP_TRANSFORMS ? op->transform_count : 1;
        for (size_t i=0;i<targets;++i) {
            Fl_Entity *owner=(Fl_Entity *)fl_scene_entity(s,op->kind==FL_OP_TRANSFORMS ? op->transforms[i].entity : target);
            for (size_t j=0;owner && j<owner->component_count;++j) {
                Fl_Component *component=(Fl_Component *)&owner->components[j];
                if (!component->id) {component->id=p->next_component_id++; if (op->kind==FL_OP_COMPONENT_SET && component->type==op->component.type) created=component->id;}
            }
        }
        ++s->revision;
    }
    p->dirty = true; ++p->generation;
    if (out_id) *out_id = created;
    Fl_ProjectChange change={p,op->kind,op->kind==FL_OP_SCENE_ADD ? created : op->scene,
        op->kind==FL_OP_ENTITY_ADD ? created : op->entity,
        op->kind==FL_OP_ASSET_ADD || (op->kind==FL_OP_ASSET_SOURCE_SET && created) ? created : op->asset};
    size_t index=0;
    while (index<p->change_count && p->changes[index].scene!=change.scene) ++index;
    if (index==p->change_count) ++p->change_count;
    p->changes[index]=change;
    if (p->bus && p->interactive) {
        p->publishing=true; fl_event_publish(p->bus,p->changed,&change,sizeof(change)); p->publishing=false;
    }
    return true;
}

static void encode_scene(const Fl_Scene *scene, Fl_Binary *stream);
static bool decode_scene(Fl_Project *project, Fl_Scene *scene, const Fl_Binary *stream);

/* Prepare owned rollback storage before mutation; cancellation transfers it without allocating. */
static Fl_Project *clone_document(const Fl_Project *p)
{
    Fl_Project *backup=calloc(1,sizeof(*backup));
    if (!backup) return NULL;
    backup->identity=p->identity; backup->owner=p->owner; backup->name=copy_string(p->name); backup->next_id=p->next_id; backup->next_component_id=p->next_component_id;
    backup->scenes=p->count ? calloc(p->count,sizeof(*backup->scenes)) : NULL;
    backup->saved=p->count ? malloc(p->count*sizeof(*backup->saved)) : NULL;
    backup->assets=p->asset_count ? calloc(p->asset_count,sizeof(*backup->assets)) : NULL;
    backup->retired=p->retired_count ? malloc(p->retired_count*sizeof(*backup->retired)) : NULL;
    backup->retired_assets=p->retired_asset_count ? malloc(p->retired_asset_count*sizeof(*backup->retired_assets)) : NULL;
    bool ok=backup->name && (!p->count || (backup->scenes && backup->saved)) && (!p->asset_count || backup->assets) &&
        (!p->retired_count || backup->retired) && (!p->retired_asset_count || backup->retired_assets);
    if (backup->scenes) backup->count=p->count;
    if (backup->assets) backup->asset_count=p->asset_count;
    if (ok) {
        if (p->count) memcpy(backup->saved,p->saved,p->count*sizeof(*p->saved));
        if (p->retired_count) memcpy(backup->retired,p->retired,p->retired_count*sizeof(*p->retired));
        if (p->retired_asset_count) memcpy(backup->retired_assets,p->retired_assets,p->retired_asset_count*sizeof(*p->retired_assets));
        backup->retired_count=p->retired_count; backup->retired_asset_count=p->retired_asset_count;
    }
    for (size_t i=0;ok && i<p->asset_count;++i) ok=clone_asset(&p->assets[i],&backup->assets[i]);
    for (size_t i=0;ok && i<p->count;++i) {
        Fl_Binary bytes={0}; encode_scene(&p->scenes[i],&bytes);
        backup->scenes[i].id=p->scenes[i].id; backup->scenes[i].revision=p->scenes[i].revision;
        ok=!bytes.failed && decode_scene(backup,&backup->scenes[i],&bytes); free(bytes.data);
    }
    if (!ok) {fl_project_destroy(backup); return NULL;}
    return backup;
}

Fl_Project *fl_project_read_view(const Fl_Project *project)
{
    if (!project || project->editing || project->owner != fl_thread_current_id()) return NULL;
    Fl_Project *view = clone_document(project);
    if (!view) return NULL;
    view->directory = copy_string(project->directory);
    if (!view->directory) {fl_project_destroy(view); return NULL;}
    view->generation = project->generation; view->read_only = true;
    return view;
}

bool fl_project_read_view_rebase(Fl_Project *view, const char *directory)
{
    if (!view || !view->read_only || !directory || view->owner!=fl_thread_current_id()) return false;
    char *copy=copy_string(directory); if (!copy) return false;
    free(view->directory); view->directory=copy; return true;
}

bool fl_project_is_read_view(const Fl_Project *project) {return project && project->read_only;}
uint64_t fl_project_identity(const Fl_Project *project) {return project ? project->identity : 0;}

Fl_History *fl_project_history(Fl_Project *p) {return p ? p->history : NULL;}
bool fl_project_edit_active(const Fl_Project *p) {return p && p->editing;}

bool fl_project_edit_begin(Fl_Project *p, const char *label, bool interactive)
{
    if (!p || p->read_only || p->publishing || p->editing || p->owner!=fl_thread_current_id() || !fl_history_begin(p->history,p,label)) return false;
    p->edit_backup=clone_document(p);
    if (!p->edit_backup) {fl_history_discard(p->history); return false;}
    p->diagnostic[0]=0; p->editing=true; p->interactive=interactive; p->edit_generation=p->generation; p->edit_dirty=p->dirty; p->change_count=0; return true;
}

bool fl_project_edit_cancel(Fl_Project *p)
{
    if (!p || !p->editing || p->publishing || p->owner!=fl_thread_current_id()) return false;
    Fl_Project *backup=p->edit_backup; p->edit_backup=NULL;
    if (p->generation==p->edit_generation) {
        fl_history_discard(p->history); p->editing=false; p->change_count=0; fl_project_destroy(backup); return true;
    }
    for (size_t i=0;i<backup->count;++i) {
        const Fl_Scene *current=fl_project_scene(p,backup->scenes[i].id);
        uint64_t revision=current && current->revision>backup->scenes[i].revision ? current->revision : backup->scenes[i].revision;
        uint64_t restored=revision<UINT64_MAX ? revision+1 : revision;
        if (backup->saved[i]==backup->scenes[i].revision) backup->saved[i]=restored;
        backup->scenes[i].revision=restored;
    }
    /* The host owns disk revisions, lock and observers; exchange only authored storage. */
    char *name=p->name; p->name=backup->name; backup->name=name;
    Fl_Scene *scenes=p->scenes; p->scenes=backup->scenes; backup->scenes=scenes;
    size_t count=p->count; p->count=backup->count; backup->count=count;
    uint64_t *saved=p->saved; p->saved=backup->saved; backup->saved=saved;
    Fl_Asset *assets=p->assets; p->assets=backup->assets; backup->assets=assets;
    count=p->asset_count; p->asset_count=backup->asset_count; backup->asset_count=count;
    Fl_Retired *retired=p->retired; p->retired=backup->retired; backup->retired=retired;
    count=p->retired_count; p->retired_count=backup->retired_count; backup->retired_count=count;
    Fl_RetiredAsset *retired_assets=p->retired_assets; p->retired_assets=backup->retired_assets; backup->retired_assets=retired_assets;
    count=p->retired_asset_count; p->retired_asset_count=backup->retired_asset_count; backup->retired_asset_count=count;
    for (size_t i=0;i<p->count;++i) p->scenes[i].project=p;
    ++p->generation; p->dirty=p->edit_dirty; p->editing=false;
    fl_history_cancel_restored(p->history,p);
    if (p->bus && p->interactive) {
        p->publishing=true;
        for (size_t i=0;i<backup->count;++i) if (!fl_project_scene(p,backup->scenes[i].id)) {
            Fl_ProjectChange change={p,FL_OP_RESTORE,backup->scenes[i].id,0,0}; fl_event_publish(p->bus,p->changed,&change,sizeof(change));
        }
        for (size_t i=0;i<p->count;++i) {Fl_ProjectChange change={p,FL_OP_RESTORE,p->scenes[i].id,0,0}; fl_event_publish(p->bus,p->changed,&change,sizeof(change));}
        p->publishing=false;
    }
    p->change_count=0; fl_project_destroy(backup); return true;
}

/* Resolve a sorted catalog identity in logarithmic time for dependency traversal. */
static size_t asset_position(const Fl_Project *p, Fl_Id id)
{
    size_t lo=0,hi=p->asset_count;
    while (lo<hi) {size_t mid=lo+(hi-lo)/2; if (p->assets[mid].id<id) lo=mid+1; else hi=mid;}
    return lo<p->asset_count && p->assets[lo].id==id ? lo : SIZE_MAX;
}

/* Reject resource cycles even for unused reusable assets; traversal is bounded and nonrecursive. */
static bool validate_asset_graph(Fl_Project *p)
{
    size_t n=p->asset_count;
    unsigned char *state=n ? calloc(n,1) : NULL;
    size_t *stack=n ? malloc(n*sizeof(*stack)) : NULL, *edges=n ? calloc(n,sizeof(*edges)) : NULL;
    if (n && (!state || !stack || !edges)) {free(state); free(stack); free(edges); return false;}
    bool valid=true;
    for (size_t i=0;valid && i<n;++i) if (!state[i]) {
        size_t depth=1; stack[0]=i; state[i]=1;
        while (valid && depth) {
            size_t index=stack[depth-1]; const Fl_Asset *asset=&p->assets[index];
            if (edges[index]==asset->reference_count) {state[index]=2; --depth; continue;}
            size_t next=asset_position(p,asset->references[edges[index]++]);
            if (next==SIZE_MAX) {snprintf(p->diagnostic,sizeof(p->diagnostic),"%s: missing resource dependency",asset->name); valid=false;}
            else if (asset->kind==FL_ASSET_LUA && p->assets[next].kind!=FL_ASSET_LUA) {snprintf(p->diagnostic,sizeof(p->diagnostic),"%s: imports require Lua assets",asset->name); valid=false;}
            else if (state[next]==1) {snprintf(p->diagnostic,sizeof(p->diagnostic),"%s: recursive resource dependency",asset->name); valid=false;}
            else if (!state[next]) {state[next]=1; stack[depth++]=next;}
        }
    }
    free(state); free(stack); free(edges); return valid;
}

/* Resolve reusable document dependencies against the final asset catalog, including behavior interfaces. */
static bool validate_prototypes(Fl_Project *project)
{
    if (!validate_asset_graph(project)) return false;
    for (size_t i=0;i<project->asset_count;++i) if (project->assets[i].kind==FL_ASSET_PROTOTYPE) {
        const Fl_Asset *asset=&project->assets[i]; Fl_Prototype prototype={0}; Fl_Json context={0};
        bool valid=fl_prototype_read(&context,asset->source,(size_t)asset->size,&prototype) && fl_prototype_validate(&context,&prototype,project,NULL);
        fl_prototype_clear(&prototype);
        if (!valid) {snprintf(project->diagnostic,sizeof(project->diagnostic),"%s: %.160s",asset->name,context.error); return false;}
    }
    return true;
}

bool fl_project_edit_validate(Fl_Project *project)
{
    if (!project || !project->editing || project->publishing || project->owner!=fl_thread_current_id()) return false;
    if (!validate_prototypes(project)) return false;
    for (size_t i=0;i<project->count;++i) if (fl_composition_required(&project->scenes[i])) {
        Fl_Composition *composition=fl_composition_create(&project->scenes[i],NULL,project->diagnostic,sizeof(project->diagnostic));
        if (!composition) return false;
        fl_composition_destroy(composition);
    }
    for (size_t i=0;i<project->count;++i)
        if (!fl_script_validate_scene(&project->scenes[i],NULL,project->diagnostic,sizeof(project->diagnostic))) return false;
    return true;
}

/* Executable declarations cannot publish interactive intermediate states; every final changeset validates compiled inputs. */
static bool changes_behavior(const Fl_Op *operation)
{
    if (operation->kind==FL_OP_ASSET_SOURCE_SET && operation->source) return true;
    if (operation->kind==FL_OP_COMPONENT_SET && (operation->component.type==FL_COMPONENT_SCRIPT || operation->component.type==FL_COMPONENT_PROTOTYPE || operation->component.type==FL_COMPONENT_COMPOSITION)) return true;
    if ((operation->kind==FL_OP_ENTITY_ADD || operation->kind==FL_OP_ENTITY_CONFIGURE) && operation->components && operation->component_count<=64)
        for (size_t i=0;i<operation->component_count;++i) if ((operation->components[i].type==FL_COMPONENT_SCRIPT || operation->components[i].type==FL_COMPONENT_PROTOTYPE || operation->components[i].type==FL_COMPONENT_COMPOSITION)) return true;
    return false;
}

bool fl_project_edit_commit(Fl_Project *p)
{
    if (!p || !p->editing || p->publishing || p->owner!=fl_thread_current_id()) return false;
    if (p->generation==p->edit_generation) {fl_history_discard(p->history); p->editing=false; p->change_count=0; fl_project_destroy(p->edit_backup); p->edit_backup=NULL; return true;}
    if (!fl_project_edit_validate(p) || !fl_history_commit(p->history,p)) return fl_project_edit_cancel(p) && false;
    p->editing=false; fl_project_destroy(p->edit_backup); p->edit_backup=NULL;
    if (p->bus && !p->interactive) {
        p->publishing=true;
        for (size_t i=0;i<p->change_count;++i) fl_event_publish(p->bus,p->changed,&p->changes[i],sizeof(*p->changes));
        p->publishing=false;
    }
    p->change_count=0; return true;
}

bool fl_project_apply(Fl_Project *p, const Fl_Op *op, Fl_Id *out_id)
{
    if (!p || p->read_only || !op || p->publishing || p->owner!=fl_thread_current_id()) return false;
    bool single=!p->editing;
    if (single && !fl_project_edit_begin(p,"Edit project",false)) return false;
    /* Reserve the only fallible notification storage before mutating any document state. */
    if (p->change_count==p->change_capacity) {
        size_t capacity=p->change_capacity ? p->change_capacity*2 : 8;
        Fl_ProjectChange *changes=realloc(p->changes,capacity*sizeof(*changes));
        if (!changes) {if (single) fl_project_edit_cancel(p); return false;}
        p->changes=changes; p->change_capacity=capacity;
    }
    bool behavior=changes_behavior(op);
    if (p->interactive && behavior) {snprintf(p->diagnostic,sizeof(p->diagnostic),"Behavior declarations require an atomic changeset"); return false;}
    bool ok=apply_operation(p,op,out_id);
    if (single) {
        if (ok) ok=fl_project_edit_commit(p);
        else fl_project_edit_cancel(p);
    }
    if (!ok && out_id) *out_id=0;
    return ok;
}

/* Serialize one scene document into stream. */
static void encode_scene(const Fl_Scene *s, Fl_Binary *b)
{
    fl_binary_u64(b, s->id); fl_binary_u64(b, s->revision); fl_binary_string(b, s->name);
    fl_binary_u64(b, s->width); fl_binary_u64(b, s->height); fl_binary_u64(b, s->duration);
    fl_binary_u64(b, s->entity_count);
    for (size_t i = 0; i < s->entity_count; ++i) {
        const Fl_Entity *e = &s->entities[i];
        fl_binary_u64(b, e->id); fl_binary_u64(b, e->parent); fl_binary_string(b, e->name);
        fl_binary_u64(b, e->visible); fl_binary_u64(b, e->component_count);
        for (size_t j = 0; j < e->component_count; ++j) fl_component_write(&e->components[j], b);
    }
    fl_timeline_save(s, b);
    fl_binary_u64(b, s->aspect_width); fl_binary_u64(b, s->aspect_height);
    const double camera[2] = {s->camera_x, s->camera_y};
    for (size_t i = 0; i < 2; ++i) { uint64_t bits; memcpy(&bits, &camera[i], sizeof(bits)); fl_binary_u64(b, bits); }
    for (size_t i = 0; i < 4; ++i) fl_binary_u64(b, s->background[i]);
    fl_manuscript_save(s, b);
}

/* Return the tracked disk revision for a project-relative document path. */
static const Fl_ProjectFile *document_file(const Fl_Project *p, const char *path)
{
    for (size_t i=0;i<p->file_count;++i) if (!strcmp(p->files[i].path,path)) return &p->files[i];
    return NULL;
}

/* Capture one accepted document's fingerprint, retaining ownership in the project. */
static bool track_file(Fl_Project *p, const char *path, const char *text, size_t size)
{
    Fl_ProjectFile entry={0};
    if (strlen(path)>=sizeof(entry.path)) return false;
    strcpy(entry.path,path); entry.existed=true;
    Fl_Sha256 hash; fl_sha256_init(&hash); fl_sha256_update(&hash,text,size); fl_sha256_final(&hash,entry.hash);
    const Fl_ProjectFile *existing=document_file(p,path);
    if (existing) {p->files[existing-p->files]=entry; return true;}
    Fl_ProjectFile *files=realloc(p->files,(p->file_count+1)*sizeof(*files));
    if (!files) return false;
    p->files=files; files[p->file_count++]=entry; return true;
}

/* Detect external changes before an editor save; never overwrite an unseen disk revision. */
static bool files_unchanged(const Fl_Project *p)
{
    for (size_t i=0;i<p->file_count;++i) {
        char path[FL_PATH_MAX]; unsigned char hash[32]; bool exists;
        if (!fl_fs_join(path,sizeof(path),p->directory,p->files[i].path) || !fl_document_fingerprint(path,hash,&exists) ||
            exists!=p->files[i].existed || (exists && memcmp(hash,p->files[i].hash,32))) return false;
    }
    return true;
}

/* Serialize the manifest with exact identities and media metadata. */
static cJSON *manifest_write(Fl_Json *c, const Fl_Project *p)
{
    cJSON *o=cJSON_CreateObject(), *scenes=cJSON_CreateArray(), *assets=cJSON_CreateArray();
    fl_json_add(c,o,"version",cJSON_CreateNumber(1)); fl_json_add(c,o,"name",cJSON_CreateString(p->name));
    fl_json_add(c,o,"next_id",fl_json_u64(p->next_id)); fl_json_add(c,o,"next_component_id",fl_json_u64(p->next_component_id));
    for (size_t i=0;i<p->count;++i) fl_json_add(c,scenes,NULL,fl_json_u64(p->scenes[i].id));
    fl_json_add(c,o,"scenes",scenes);
    static const char *const kinds[]={"","image","video","audio","font","lua","flproto"};
    for (size_t i=0;i<p->asset_count;++i) {
        const Fl_Asset *a=&p->assets[i]; cJSON *item=cJSON_CreateObject(); char hash[65]; fl_sha256_hex(a->hash,hash);
        fl_json_add(c,item,"id",fl_json_u64(a->id)); fl_json_add(c,item,"name",cJSON_CreateString(a->name));
        fl_json_add(c,item,"kind",cJSON_CreateString(kinds[a->kind])); fl_json_add(c,item,"hash",cJSON_CreateString(hash)); fl_json_add(c,item,"extension",cJSON_CreateString(a->extension));
        if (a->path) fl_json_add(c,item,"path",cJSON_CreateString(a->path));
        fl_json_add(c,item,"size",fl_json_u64(a->size)); fl_json_add(c,item,"width",fl_json_u64(a->width)); fl_json_add(c,item,"height",fl_json_u64(a->height));
        fl_json_add(c,item,"duration",fl_json_u64(a->duration)); fl_json_add(c,item,"sample_rate",cJSON_CreateNumber(a->sample_rate)); fl_json_add(c,item,"channels",cJSON_CreateNumber(a->channels));
        fl_json_add(c,assets,NULL,item);
    }
    fl_json_add(c,o,"assets",assets); return o;
}

/* Complete an earlier managed save and update only the files named by its checked intent. */
static bool recover_save(Fl_Project *p, Fl_Json *c)
{
    char path[FL_PATH_MAX];
    if (!fl_fs_join(path,sizeof(path),p->directory,".flow/local/pending.json")) return false;
    if (!fl_fs_exists(path)) return true;
    size_t size; char *text=fl_fs_read_file(path,FL_DOCUMENT_LIMIT,&size);
    cJSON *root=text ? fl_json_parse(c,text,size) : NULL; free(text);
    const cJSON *files=root ? fl_json_member(c,root,"files",cJSON_Array) : NULL;
    if (!files) {cJSON_Delete(root); return false;}
    /* Unrelated files must still match their accepted revisions. */
    bool ok=true;
    for (size_t i=0;i<p->file_count && ok;++i) {
        bool included=false;
        for (const cJSON *v=files->child;v;v=v->next) {
            const cJSON *name=fl_json_member(c,v,"path",cJSON_String);
            if (name && !strcmp(name->valuestring,p->files[i].path)) included=true;
        }
        if (included) continue;
        unsigned char hash[32]; bool exists;
        ok=fl_fs_join(path,sizeof(path),p->directory,p->files[i].path) && fl_document_fingerprint(path,hash,&exists) &&
            exists==p->files[i].existed && (!exists || !memcmp(hash,p->files[i].hash,32));
    }
    if (ok) ok=fl_document_recover(c,p->directory);
    for (const cJSON *v=files->child;ok && v;v=v->next) {
        const cJSON *name=fl_json_member(c,v,"path",cJSON_String), *body=fl_json_member(c,v,"text",cJSON_String);
        ok=name && body && track_file(p,name->valuestring,body->valuestring,strlen(body->valuestring));
    }
    cJSON_Delete(root);
    return ok || fl_json_fail(c,"Cannot recover pending save without overwriting external changes");
}

/* Whether a tracked source path still belongs to the accepted authored document. */
static bool document_referenced(const Fl_Project *p, const char *path)
{
    if (!strcmp(path,"project.flow.json")) return true;
    for (size_t i=0;i<p->asset_count;++i) if (p->assets[i].path && !strcmp(p->assets[i].path,path)) return true;
    for (size_t i=0;i<p->count;++i) {
        char relative[96]; snprintf(relative,sizeof(relative),"scenes/%016llx.scene.json",(unsigned long long)p->scenes[i].id);
        if (!strcmp(path,relative)) return true;
    }
    return false;
}

bool fl_project_save(Fl_Project *p)
{
    if (!p || p->read_only || p->publishing || p->editing || p->owner!=fl_thread_current_id()) return false;
    if (!p->dirty) return true;
    Fl_Json context={0};
    if (!recover_save(p,&context)) {snprintf(p->diagnostic,sizeof(p->diagnostic),"%s",context.error); return false;}
    if (!files_unchanged(p)) {snprintf(p->diagnostic,sizeof(p->diagnostic),"External file changes require reload or conflict resolution"); return false;}
    size_t source_count=0;
    for (size_t i=0;i<p->asset_count;++i) source_count+=fl_source_asset_format(p->assets[i].kind)!=NULL;
    size_t count=1+source_count;
    for (size_t i=0;i<p->count;++i) count+=p->saved[i]!=p->scenes[i].revision;
    Fl_DocumentWrite *writes=calloc(count,sizeof(*writes));
    char **paths=calloc(count,sizeof(*paths));
    if (!writes || !paths) {free(writes); free(paths); return false;}
    size_t at=0;
    for (size_t i=0;i<p->asset_count;++i) if (fl_source_asset_format(p->assets[i].kind)) {
        const Fl_Asset *a=&p->assets[i]; paths[at]=copy_string(a->path); writes[at].path=paths[at];
        const Fl_ProjectFile *file=document_file(p,a->path);
        if (file) {writes[at].existed=file->existed; memcpy(writes[at].expected,file->hash,32);}
        writes[at].text=copy_string(a->source); writes[at].size=(size_t)a->size;
        if (!paths[at] || !writes[at].text) fl_json_fail(&context,"Cannot allocate source asset save");
        ++at;
    }
    for (size_t i=0;i<=p->count && !context.error[0];++i) {
        if (i<p->count && p->saved[i]==p->scenes[i].revision) continue;
        char relative[96];
        if (i==p->count) strcpy(relative,"project.flow.json");
        else snprintf(relative,sizeof(relative),"scenes/%016llx.scene.json",(unsigned long long)p->scenes[i].id);
        cJSON *root=i==p->count ? manifest_write(&context,p) : fl_document_scene_write(&context,&p->scenes[i]);
        paths[at]=copy_string(relative); writes[at].path=paths[at];
        const Fl_ProjectFile *file=document_file(p,relative);
        if (file) {writes[at].existed=file->existed; memcpy(writes[at].expected,file->hash,32);}
        char *text=root ? cJSON_Print(root) : NULL; cJSON_Delete(root);
        writes[at].text=text; writes[at].size=text ? strlen(text) : 0;
        if (!paths[at] || !text) fl_json_fail(&context,"Cannot allocate document save");
        ++at;
    }
    bool ok=!context.error[0] && at==count && fl_document_commit(&context,p->directory,writes,count);
    if (ok) {
        for (size_t i=0;i<count;++i) if (!track_file(p,writes[i].path,writes[i].text,writes[i].size)) ok=false;
    }
    for (size_t i=0;i<count;++i) {free(paths[i]); cJSON_free((void *)writes[i].text);}
    free(paths); free(writes);
    if (!ok) {snprintf(p->diagnostic,sizeof(p->diagnostic),"%s",context.error); return false;}
    p->diagnostic[0]=0; p->dirty=false;
    for (size_t i=0;i<p->count;++i) p->saved[i]=p->scenes[i].revision;
    free(p->retired); p->retired=NULL; p->retired_count=0;
    size_t retained=0;
    for (size_t i=0;i<p->retired_asset_count;++i) {
        Fl_RetiredAsset r=p->retired_assets[i]; char path[FL_PATH_MAX];
        if (fl_project_asset_by_hash(p,r.hash) || fl_history_has_asset(p->history,r.hash) || fl_resource_snapshot_leased(p->directory,r.hash)) p->retired_assets[retained++]=r;
        else if (fl_asset_store_path(p->directory,r.hash,r.extension,path,sizeof(path))) fl_fs_remove(path);
    }
    p->retired_asset_count=retained;
    /* Retire only previously tracked source bytes; never remove an external replacement. */
    for (size_t i=0;i<p->file_count;) {
        Fl_ProjectFile *file=&p->files[i];
        if (!document_referenced(p,file->path)) {
            char path[FL_PATH_MAX]; unsigned char hash[32]; bool exists;
            if (fl_fs_join(path,sizeof(path),p->directory,file->path) && fl_document_fingerprint(path,hash,&exists) &&
                exists && !memcmp(hash,file->hash,32)) fl_fs_remove(path);
            memmove(file,file+1,(p->file_count-i-1)*sizeof(*file)); --p->file_count;
        } else ++i;
    }
    return true;
}

static bool decode_scene(Fl_Project *p, Fl_Scene *s, const Fl_Binary *input);

uint64_t fl_project_generation(const Fl_Project *p) { return p ? p->generation : 0; }

/* One scene's encoded document inside a snapshot. */
typedef struct Fl_SnapshotScene { Fl_Id id; uint64_t revision; Fl_Binary data; } Fl_SnapshotScene;
struct Fl_Snapshot { Fl_SnapshotScene *scenes; size_t count, bytes; Fl_Asset *assets; size_t asset_count; char *name; };

void fl_snapshot_destroy(Fl_Snapshot *snapshot)
{
    if (!snapshot) return;
    for (size_t i = 0; i < snapshot->count; ++i) free(snapshot->scenes[i].data.data);
    for (size_t i=0;i<snapshot->asset_count;++i) free_asset(&snapshot->assets[i]);
    free(snapshot->name); free(snapshot->assets); free(snapshot->scenes); free(snapshot);
}

bool fl_snapshot_has_asset(const Fl_Snapshot *snapshot, const unsigned char hash[32])
{
    for (size_t i=0;snapshot && hash && i<snapshot->asset_count;++i) if (!memcmp(snapshot->assets[i].hash,hash,32)) return true;
    return false;
}

size_t fl_snapshot_bytes(const Fl_Snapshot *snapshot) { return snapshot ? snapshot->bytes : 0; }

Fl_Snapshot *fl_project_snapshot(const Fl_Project *p)
{
    if (!p || p->owner != fl_thread_current_id()) return NULL;
    Fl_Snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return NULL;
    snapshot->name=copy_string(p->name);
    if (!snapshot->name) {free(snapshot); return NULL;}
    snapshot->bytes=strlen(p->name)+1;
    snapshot->scenes = p->count ? calloc(p->count, sizeof(*snapshot->scenes)) : NULL;
    if (p->count && !snapshot->scenes) {fl_snapshot_destroy(snapshot); return NULL;}
    for (size_t i = 0; i < p->count; ++i) {
        Fl_SnapshotScene *entry = &snapshot->scenes[snapshot->count++];
        entry->id = p->scenes[i].id; entry->revision = p->scenes[i].revision;
        encode_scene(&p->scenes[i], &entry->data);
        if (entry->data.failed) {fl_snapshot_destroy(snapshot); return NULL;}
        snapshot->bytes += entry->data.size;
    }
    snapshot->assets=p->asset_count ? calloc(p->asset_count,sizeof(*snapshot->assets)) : NULL;
    if (p->asset_count && !snapshot->assets) {fl_snapshot_destroy(snapshot); return NULL;}
    for (size_t i=0;i<p->asset_count;++i) {
        ++snapshot->asset_count;
        if (!clone_asset(&p->assets[i],&snapshot->assets[i])) {fl_snapshot_destroy(snapshot); return NULL;}
        const Fl_Asset *a=&p->assets[i];
        snapshot->bytes+=sizeof(*a)+strlen(a->name)+strlen(a->extension)+2;
        if (a->path) snapshot->bytes+=strlen(a->path)+1;
        if (a->source) snapshot->bytes+=(size_t)a->size+1;
        snapshot->bytes+=a->reference_count*sizeof(*a->references);
    }
    return snapshot;
}

/* Revision a restored scene must use so it never collides with a committed or pending-delete file. */
static uint64_t restored_revision(const Fl_Project *p, Fl_Id id, uint64_t snapshot_revision)
{
    uint64_t highest = snapshot_revision;
    for (size_t i = 0; i < p->count; ++i) if (p->scenes[i].id == id && p->scenes[i].revision > highest) highest = p->scenes[i].revision;
    for (size_t i = 0; i < p->retired_count; ++i) if (p->retired[i].id == id && p->retired[i].revision > highest) highest = p->retired[i].revision;
    return highest < UINT64_MAX ? highest + 1 : 0;
}

bool fl_project_restore(Fl_Project *p, const Fl_Snapshot *snapshot)
{
    if (!p || p->read_only || !snapshot || p->publishing || p->generation==UINT64_MAX || p->owner != fl_thread_current_id()) return false;
    char *name=copy_string(snapshot->name);
    if (!name) return false;
    Fl_Asset *assets=snapshot->asset_count ? calloc(snapshot->asset_count,sizeof(*assets)) : NULL;
    if (snapshot->asset_count && !assets) {free(name); return false;}
    for (size_t i=0;i<snapshot->asset_count;++i) if (!clone_asset(&snapshot->assets[i],&assets[i])) {
        for (size_t j=0;j<=i;++j) free_asset(&assets[j]);
        free(name); free(assets); return false;
    }
    Fl_Scene *scenes = snapshot->count ? calloc(snapshot->count, sizeof(*scenes)) : NULL;
    uint64_t *saved = snapshot->count ? calloc(snapshot->count, sizeof(*saved)) : NULL;
    size_t removed = 0;
    for (size_t i = 0; i < p->count; ++i) {
        bool kept = false;
        for (size_t j = 0; j < snapshot->count; ++j) kept |= snapshot->scenes[j].id == p->scenes[i].id;
        removed += !kept && p->saved[i];
    }
    Fl_Id *previous = p->count ? malloc(p->count * sizeof(*previous)) : NULL;
    size_t previous_count = p->count;
    for (size_t i = 0; previous && i < previous_count; ++i) previous[i] = p->scenes[i].id;
    bool fits = removed <= SIZE_MAX / sizeof(Fl_Retired) - p->retired_count;
    Fl_Retired *retired = removed && fits ? realloc(p->retired, (p->retired_count + removed) * sizeof(*retired)) : p->retired;
    bool ok = (!previous_count || previous) && fits && (!snapshot->count || (scenes && saved)) && (!removed || retired);
    if (retired) p->retired = retired;
    for (size_t i = 0; ok && i < snapshot->count; ++i) {
        const Fl_SnapshotScene *entry = &snapshot->scenes[i];
        scenes[i].id = entry->id; scenes[i].revision = entry->revision;
        ok = entry->revision < UINT64_MAX - 1 && decode_scene(p, &scenes[i], &entry->data);
        scenes[i].revision = restored_revision(p, entry->id, entry->revision);
        ok = ok && scenes[i].revision != 0;
        for (size_t j = 0; j < p->count; ++j) if (p->scenes[j].id == entry->id) saved[i] = p->saved[j];
    }
    if (!ok) {
        for (size_t i = 0; i < snapshot->count && scenes; ++i) fl_scene_storage_clear(&scenes[i]);
        free(previous); free(scenes); free(saved);
        for (size_t i=0;i<snapshot->asset_count;++i) free_asset(&assets[i]);
        free(name); free(assets); return false;
    }
    for (size_t i = 0; i < p->count; ++i) {
        bool kept = false;
        for (size_t j = 0; j < snapshot->count; ++j) kept |= snapshot->scenes[j].id == p->scenes[i].id;
        if (!kept && p->saved[i]) p->retired[p->retired_count++] = (Fl_Retired){p->scenes[i].id, p->saved[i]};
    }
    for (size_t i = 0; i < p->count; ++i) fl_scene_storage_clear(&p->scenes[i]);
    free(p->scenes); free(p->saved);
    free(p->name); p->name=name;
    p->scenes = scenes; p->saved = saved; p->count = snapshot->count;
    for (size_t i=0;i<p->asset_count;++i) free_asset(&p->assets[i]);
    free(p->assets); p->assets=assets; p->asset_count=snapshot->asset_count;
    p->dirty = true; ++p->generation;
    if (p->bus && (!p->editing || p->interactive)) {
        p->publishing = true;
        for (size_t i = 0; i < previous_count; ++i) {
            if (fl_project_scene(p, previous[i])) continue;
            Fl_ProjectChange change = {p, FL_OP_RESTORE, previous[i], 0, 0};
            fl_event_publish(p->bus, p->changed, &change, sizeof(change));
        }
        for (size_t i = 0; i < p->count; ++i) {
            Fl_ProjectChange change = {p, FL_OP_RESTORE, p->scenes[i].id, 0, 0};
            fl_event_publish(p->bus, p->changed, &change, sizeof(change));
        }
        p->publishing = false;
    }
    free(previous);
    return true;
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
    fl_history_clear(p->history);
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
static bool decode_scene(Fl_Project *p, Fl_Scene *s, const Fl_Binary *input)
{
    s->project=p;
    Fl_Binary b = *input;
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
                for (size_t k=0;k<animation->track_count;++k) animation->tracks[k].entity=e->id;
            }
            if (!fl_widget_component_valid(c)) b.failed = true;
            for (size_t k = 0; k < j; ++k) if (components[k].type == c->type && c->type!=FL_COMPONENT_SCRIPT) b.failed = true;
        }
        if (!b.failed && !fl_entity_component(e, FL_COMPONENT_POSE)) b.failed = true;
        for (size_t j = 0; !b.failed && j < e->component_count; ++j) if (e->components[j].type == FL_COMPONENT_POSE) {
            Fl_Component *all = (Fl_Component *)e->components, transform = all[j];
            memmove(all + 1, all, j * sizeof(*all)); all[0] = transform; break;
        }
    }
    if (!b.failed && !fl_scene_storage_reserve_index(s, s->entity_count)) b.failed = true;
    if (!b.failed) fl_scene_storage_rebuild_index(s);
    if (!b.failed && !fl_timeline_load(s, &b)) b.failed = true;
    if (!b.failed) {
        s->aspect_width = fl_binary_get_u64(&b); s->aspect_height = fl_binary_get_u64(&b);
        if (s->aspect_width > 65536 || s->aspect_height > 65536 ||
            (!s->aspect_width != !s->aspect_height)) b.failed = true;
    }
    if (!b.failed) {
        uint64_t bits = fl_binary_get_u64(&b); memcpy(&s->camera_x, &bits, sizeof(bits));
        bits = fl_binary_get_u64(&b); memcpy(&s->camera_y, &bits, sizeof(bits));
        if (!isfinite(s->camera_x) || !isfinite(s->camera_y)) b.failed = true;
    }
    for (size_t i = 0; !b.failed && i < 4; ++i) {
        uint64_t channel = fl_binary_get_u64(&b);
        if (channel > 255) b.failed = true;
        s->background[i] = (unsigned char)channel;
    }
    if (s->background[3] != 255) b.failed = true;
    if (!b.failed && !fl_manuscript_load(s, &b)) b.failed = true;
    bool ok = !b.failed && b.cursor == b.size && id == s->id && revision == s->revision &&
              s->name && s->name[0] && s->width && s->width <= 65536 && s->height && s->height <= 65536 &&
              s->duration && s->duration <= INT64_MAX;
    return ok;
}

/* Read a stable scene file and build its fast identity index before validation. */
static bool load_scene(Fl_Project *p, Fl_Scene *scene)
{
    char path[FL_PATH_MAX], relative[96]; Fl_Json c={0}; size_t size;
    Fl_Id id=scene->id; scene->project=p;
    if (!project_path(p,id,0,path)) return false;
    char *text=fl_fs_read_file(path,FL_DOCUMENT_LIMIT,&size);
    cJSON *root=text ? fl_json_parse(&c,text,size) : NULL;
    bool ok=root && fl_document_scene_read(&c,root,scene) && scene->id==id && scene->id<p->next_id && fl_scene_storage_reserve_index(scene,scene->entity_count);
    if (ok) fl_scene_storage_rebuild_index(scene);
    snprintf(relative,sizeof(relative),"scenes/%016llx.scene.json",(unsigned long long)id);
    ok=ok && track_file(p,relative,text,size);
    cJSON_Delete(root); free(text);
    return ok;
}

/* Decode a manifest's media catalog, rejecting unknown fields and duplicate hashes. */
static bool load_assets(Fl_Project *p, Fl_Json *c, const cJSON *array)
{
    if (!cJSON_IsArray(array)) return false;
    size_t count=(size_t)cJSON_GetArraySize(array);
    if (count>FL_DOCUMENT_LIMIT/sizeof(Fl_Asset)) return false;
    p->assets=count ? calloc(count,sizeof(*p->assets)) : NULL;
    if (count && !p->assets) return false;
    p->asset_count=count;
    static const char *const kinds[]={"","image","video","audio","font","lua","flproto"};
    size_t i=0;
    for (const cJSON *v=array->child;v;v=v->next,++i) {
        Fl_Asset *a=&p->assets[i]; uint64_t rate,channels;
        if (!fl_json_object(c,v,"id name kind hash extension size width height duration sample_rate channels path")) return false;
        const cJSON *kind=fl_json_member(c,v,"kind",cJSON_String), *hash=fl_json_member(c,v,"hash",cJSON_String);
        if (!kind || !hash || strlen(hash->valuestring)!=64) return false;
        for (int k=FL_ASSET_IMAGE;k<=FL_ASSET_PROTOTYPE;++k) if (!strcmp(kind->valuestring,kinds[k])) a->kind=(Fl_AssetKind)k;
        a->name=fl_json_string(c,fl_json_member(c,v,"name",cJSON_String)); a->extension=fl_json_string(c,fl_json_member(c,v,"extension",cJSON_String));
        if (!fl_json_uint(c,fl_json_member(c,v,"id",0),&a->id) || !fl_json_uint(c,fl_json_member(c,v,"size",0),&a->size) ||
            !fl_json_uint(c,fl_json_member(c,v,"width",0),&a->width) || !fl_json_uint(c,fl_json_member(c,v,"height",0),&a->height) ||
            !fl_json_uint(c,fl_json_member(c,v,"duration",0),&a->duration) || !fl_json_uint(c,fl_json_member(c,v,"sample_rate",0),&rate) ||
            !fl_json_uint(c,fl_json_member(c,v,"channels",0),&channels)) return false;
        if (!a->id || a->id>=p->next_id || (i && a->id<=p->assets[i-1].id) || !a->kind || !a->name || !a->name[0] || !fl_asset_extension_valid(a->extension) || rate>768000 || channels>64) return false;
        a->sample_rate=(uint32_t)rate; a->channels=(uint32_t)channels;
        for (size_t j=0;j<64;++j) {
            char digit=hash->valuestring[j]; unsigned n=digit>='0' && digit<='9' ? (unsigned)(digit-'0') : digit>='a' && digit<='f' ? (unsigned)(digit-'a'+10) : 16;
            if (n>15) return false;
            a->hash[j/2]=(unsigned char)((a->hash[j/2]<<4)|n);
        }
        const cJSON *source_path=cJSON_GetObjectItemCaseSensitive(v,"path");
        const Fl_SourceFormat *format=fl_source_asset_format(a->kind);
        if (format) {
            if (!cJSON_IsString(source_path) || strncmp(source_path->valuestring,"assets/",7) || !fl_document_path_valid(source_path->valuestring) || strcmp(a->extension,format->extension)) return false;
            a->path=copy_string(source_path->valuestring); char absolute[FL_PATH_MAX]; size_t size=0;
            if (!a->path || !fl_fs_join(absolute,sizeof(absolute),p->directory,a->path)) return false;
            a->source=fl_fs_read_file(absolute,format->limit,&size);
            Fl_SourceInfo info={0};
            if (!a->source || !fl_source_asset_name_valid(a->name) || !fl_source_asset_compile(a->kind,a->source,size,&info,p->diagnostic,sizeof(p->diagnostic))) return false;
            a->references=info.references; a->reference_count=info.reference_count;
            a->size=size; Fl_Sha256 hash_state; fl_sha256_init(&hash_state); fl_sha256_update(&hash_state,a->source,size); fl_sha256_final(&hash_state,a->hash);
            if (!track_file(p,a->path,a->source,(size_t)a->size)) return false;
        } else if (source_path) return false;
        for (size_t j=0;j<i;++j) {
            if (format && p->assets[j].path && (!strcmp(p->assets[j].path,a->path) || (p->assets[j].kind==a->kind && !strcmp(p->assets[j].name,a->name)))) return false;
            if (!format && !p->assets[j].path && !memcmp(p->assets[j].hash,a->hash,32)) return false;
        }
    }
    return true;
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
        orphan = (!a || strcmp(a->extension, extension)) && !fl_resource_snapshot_leased(fl_project_directory(sweep->project),hash);
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

/* Validate stable component-instance identities and native multiplicity. */
static bool validate_component_ids(const Fl_Project *p)
{
    size_t count=0;
    for (size_t i=0;i<p->count;++i) for (size_t j=0;j<p->scenes[i].entity_count;++j) {
        size_t n=p->scenes[i].entities[j].component_count;
        if (n>SIZE_MAX-count) return false;
        count+=n;
    }
    if (count>SIZE_MAX/sizeof(Fl_Id)) return false;
    Fl_Id *ids=count ? malloc(count*sizeof(*ids)) : NULL;
    if (count && !ids) return false;
    size_t at=0; bool valid=true;
    for (size_t i=0;i<p->count;++i) for (size_t j=0;j<p->scenes[i].entity_count;++j) {
        const Fl_Entity *e=&p->scenes[i].entities[j];
        for (size_t k=0;k<e->component_count;++k) {
            const Fl_Component *c=&e->components[k]; ids[at++]=c->id;
            if (!c->id || c->id>=p->next_component_id || !fl_widget_component_valid(c)) valid=false;
            for (size_t n=0;n<k;++n) if (e->components[n].type==c->type && c->type!=FL_COMPONENT_SCRIPT) valid=false;
        }
    }
    if (count) qsort(ids,count,sizeof(*ids),compare_ids);
    for (size_t i=1;i<count;++i) if (ids[i]==ids[i-1]) valid=false;
    free(ids); return valid;
}

/* Validate identities and hierarchy; reject dangling parents and cycles before exposing the project. */
static bool validate_project(const Fl_Project *p)
{
    if (!validate_component_ids(p) || p->asset_count > SIZE_MAX - p->count) return false;
    size_t count = p->count + p->asset_count;
    for (size_t i = 0; i < p->count; ++i) {
        size_t timeline = fl_timeline_id_count(&p->scenes[i]);
        size_t phases = fl_manuscript_id_count(&p->scenes[i]);
        if (p->scenes[i].entity_count > SIZE_MAX - count || timeline > SIZE_MAX - count - p->scenes[i].entity_count ||
            phases > SIZE_MAX - count - p->scenes[i].entity_count - timeline) return false;
        count += p->scenes[i].entity_count + timeline + phases;
        if (!fl_timeline_valid(p, &p->scenes[i], p->next_id) || !fl_manuscript_valid(p, &p->scenes[i], p->next_id)) return false;
    }
    if (count > SIZE_MAX / sizeof(Fl_Id)) return false;
    Fl_Id *ids = count ? malloc(count * sizeof(*ids)) : NULL;
    if (count && !ids) return false;
    size_t n = 0;
    for (size_t i = 0; i < p->asset_count; ++i) ids[n++] = p->assets[i].id;
    for (size_t i = 0; i < p->count; ++i) {
        const Fl_Scene *s = &p->scenes[i]; ids[n++] = s->id;
        fl_timeline_ids(s, ids + n); n += fl_timeline_id_count(s);
        fl_manuscript_ids(s, ids + n); n += fl_manuscript_id_count(s);
        unsigned char *state = s->entity_count ? calloc(s->entity_count, 1) : NULL;
        if (s->entity_count && !state) { free(ids); return false; }
        for (size_t j = 0; j < s->entity_count; ++j) {
            const Fl_Entity *e = &s->entities[j]; ids[n++] = e->id;
            if (!references_valid(p, s, e->components, e->component_count) || !fl_entity_component(e, FL_COMPONENT_POSE)) { free(state); free(ids); return false; }
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

/* Load a candidate document without publishing events or mutating the disk. */
static bool read_project(Fl_Project *p)
{
    char path[FL_PATH_MAX]; Fl_Json c={0}; size_t size;
    if (!project_path(p,0,0,path)) return false;
    char *text=fl_fs_read_file(path,FL_DOCUMENT_LIMIT,&size);
    cJSON *root=text ? fl_json_parse(&c,text,size) : NULL;
    uint64_t version=0;
    bool ok=root && fl_json_object(&c,root,"version name next_id next_component_id scenes assets") &&
        fl_json_uint(&c,fl_json_member(&c,root,"version",0),&version) && version==1 &&
        fl_json_uint(&c,fl_json_member(&c,root,"next_id",0),&p->next_id) && p->next_id &&
        fl_json_uint(&c,fl_json_member(&c,root,"next_component_id",0),&p->next_component_id) && p->next_component_id;
    p->name=fl_json_string(&c,fl_json_member(&c,root,"name",cJSON_String));
    const cJSON *scenes=fl_json_member(&c,root,"scenes",cJSON_Array);
    if (!p->name || !p->name[0] || !scenes) ok=false;
    size_t count=scenes ? (size_t)cJSON_GetArraySize(scenes) : 0;
    if (count>FL_DOCUMENT_LIMIT/sizeof(Fl_Scene)) ok=false;
    if (ok) {
        p->scenes=count ? calloc(count,sizeof(*p->scenes)) : NULL; p->saved=count ? calloc(count,sizeof(*p->saved)) : NULL;
        if (count && (!p->scenes || !p->saved)) ok=false;
        else p->count=count;
    }
    if (ok) ok=load_assets(p,&c,fl_json_member(&c,root,"assets",cJSON_Array));
    size_t i=0;
    for (const cJSON *id=scenes ? scenes->child : NULL;ok && id;id=id->next,++i) {
        ok=fl_json_uint(&c,id,&p->scenes[i].id) && p->scenes[i].id && load_scene(p,&p->scenes[i]);
        p->saved[i]=p->scenes[i].revision;
    }
    ok=ok && !c.error[0] && validate_project(p) && track_file(p,"project.flow.json",text,size);
    for (size_t n=0;ok && n<p->asset_count;++n) if (p->assets[n].kind==FL_ASSET_LUA) {
        char diagnostic[192]; const Fl_Asset *asset=&p->assets[n];
        if (!fl_script_check(asset->source,(size_t)asset->size,diagnostic,sizeof(diagnostic))) {
            char message[256]; snprintf(message,sizeof(message),"%s: %s",asset->path,diagnostic); ok=fl_json_fail(&c,message);
        }
    }
    for (size_t n=0;ok && n<p->count;++n) {
        char diagnostic[192];
        if (!fl_script_validate_scene(&p->scenes[n],NULL,diagnostic,sizeof(diagnostic))) ok=fl_json_fail(&c,diagnostic);
    }
    if (ok && !validate_prototypes(p)) ok=fl_json_fail(&c,p->diagnostic);
    for (size_t n=0;ok && n<p->count;++n) if (fl_composition_required(&p->scenes[n])) {
        Fl_Composition *composition=fl_composition_create(&p->scenes[n],NULL,p->diagnostic,sizeof(p->diagnostic));
        if (!composition) ok=fl_json_fail(&c,p->diagnostic);
        fl_composition_destroy(composition);
    }
    ok=ok && files_unchanged(p);
    cJSON_Delete(root); free(text);
    if (!ok) snprintf(p->diagnostic,sizeof(p->diagnostic),"%s",c.error[0] ? c.error : "Invalid document references, identities or hierarchy");
    return ok;
}

Fl_Project *fl_project_open(const char *directory, Fl_EventBus *bus)
{
    if (!directory) return NULL;
    Fl_Project *p=allocate_project(directory,bus); if (!p) return NULL;
    Fl_Json c={0};
    if (!fl_document_recover(&c,p->directory) || !read_project(p)) {fl_project_destroy(p); return NULL;}
    sweep_asset_store(p); return p;
}

const char *fl_project_diagnostic(const Fl_Project *p) {return p ? p->diagnostic : "No project";}

Fl_ReloadResult fl_project_reload(Fl_Project *p)
{
    if (!p || p->read_only || p->publishing || p->owner!=fl_thread_current_id()) return FL_RELOAD_INVALID;
    if (files_unchanged(p)) return FL_RELOAD_UNCHANGED;
    if (p->dirty || p->editing) {snprintf(p->diagnostic,sizeof(p->diagnostic),"External edits conflict with unsaved project changes"); return FL_RELOAD_CONFLICT;}
    Fl_Project *candidate=calloc(1,sizeof(*candidate));
    if (!candidate) {snprintf(p->diagnostic,sizeof(p->diagnostic),"Cannot allocate document reload"); return FL_RELOAD_INVALID;}
    candidate->owner=p->owner; candidate->directory=copy_string(p->directory);
    if (!candidate->directory || !read_project(candidate)) {
        snprintf(p->diagnostic,sizeof(p->diagnostic),"%s",candidate->diagnostic[0] ? candidate->diagnostic : "Cannot read project documents");
        fl_project_destroy(candidate); return FL_RELOAD_INVALID;
    }
    if (p->generation==UINT64_MAX) {fl_project_destroy(candidate); return FL_RELOAD_INVALID;}
    for (size_t i=0;i<candidate->count;++i) {
        Fl_Scene *scene=&candidate->scenes[i]; const Fl_Scene *old=fl_project_scene(p,scene->id);
        uint64_t revision=old && old->revision>scene->revision ? old->revision : scene->revision;
        if (revision==UINT64_MAX) {fl_project_destroy(candidate); return FL_RELOAD_INVALID;}
        scene->revision=revision+1; candidate->saved[i]=scene->revision; scene->project=p;
    }
    if (candidate->next_id<p->next_id) candidate->next_id=p->next_id;
    if (candidate->next_component_id<p->next_component_id) candidate->next_component_id=p->next_component_id;
    if (!fl_history_begin(p->history,p,"External file edit")) {fl_project_destroy(candidate); return FL_RELOAD_INVALID;}
    /* Exchange complete owned documents while preserving the host, process lock and observers. */
    Fl_Project previous=*p;
    p->name=candidate->name; p->scenes=candidate->scenes; p->saved=candidate->saved; p->count=candidate->count;
    p->assets=candidate->assets; p->asset_count=candidate->asset_count; p->next_id=candidate->next_id; p->next_component_id=candidate->next_component_id;
    p->files=candidate->files; p->file_count=candidate->file_count; p->retired=NULL; p->retired_count=0;
    p->retired_assets=NULL; p->retired_asset_count=0; ++p->generation; p->diagnostic[0]=0;
    candidate->name=previous.name; candidate->scenes=previous.scenes; candidate->saved=previous.saved; candidate->count=previous.count;
    candidate->assets=previous.assets; candidate->asset_count=previous.asset_count; candidate->files=previous.files; candidate->file_count=previous.file_count;
    candidate->retired=previous.retired; candidate->retired_count=previous.retired_count;
    candidate->retired_assets=previous.retired_assets; candidate->retired_asset_count=previous.retired_asset_count;
    (void)fl_history_commit(p->history,p);
    if (p->bus) {
        p->publishing=true;
        for (size_t i=0;i<candidate->count;++i) if (!fl_project_scene(p,candidate->scenes[i].id)) {
            Fl_ProjectChange change={p,FL_OP_RESTORE,candidate->scenes[i].id,0,0}; fl_event_publish(p->bus,p->changed,&change,sizeof(change));
        }
        for (size_t i=0;i<p->count;++i) {
            Fl_ProjectChange change={p,FL_OP_RESTORE,p->scenes[i].id,0,0}; fl_event_publish(p->bus,p->changed,&change,sizeof(change));
        }
        if (!p->count && !candidate->count) {Fl_ProjectChange change={p,FL_OP_RESTORE,0,0,0}; fl_event_publish(p->bus,p->changed,&change,sizeof(change));}
        p->publishing=false;
    }
    fl_project_destroy(candidate); return FL_RELOAD_APPLIED;
}

void fl_project_destroy(Fl_Project *p)
{
    if (!p || p->publishing || p->owner != fl_thread_current_id()) return;
    for (size_t i = 0; i < p->count; ++i) fl_scene_storage_clear(&p->scenes[i]);
    for (size_t i = 0; i < p->asset_count; ++i) free_asset(&p->assets[i]);
    free(p->scenes); free(p->saved); free(p->retired); free(p->directory); free(p->name);
    free(p->assets); free(p->retired_assets); free(p->files); free(p->changes); fl_history_destroy(p->history); fl_project_destroy(p->edit_backup);
    if (p->lock) fclose(p->lock);
    free(p);
}
