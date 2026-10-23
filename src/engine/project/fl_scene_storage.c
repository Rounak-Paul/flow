#include "fl_scene_storage.h"
#include "fl_timeline_internal.h"
#include "fl_manuscript.h"
#include <stdlib.h>
#include <string.h>

struct Fl_EntityIndex { Fl_Id id; size_t position; };

/* Release one entity and all of its component payloads. */
void fl_entity_storage_clear(Fl_Entity *entity)
{
    for (size_t i = 0; i < entity->component_count; ++i) fl_component_clear((Fl_Component *)&entity->components[i]);
    free((void *)entity->components); free((void *)entity->name);
    *entity=(Fl_Entity){0};
}

/* Release a scene's owned document storage. */
void fl_scene_storage_clear(Fl_Scene *scene)
{
    for (size_t i = 0; i < scene->entity_count; ++i) fl_entity_storage_clear((Fl_Entity *)&scene->entities[i]);
    free((void *)scene->entities); free((void *)scene->identity_index); free((void *)scene->name);
    fl_timeline_free(scene);
    fl_manuscript_free(scene);
    *scene=(Fl_Scene){0};
}

/* Mix stable ids into a power-of-two identity table. */
static size_t identity_hash(Fl_Id id, size_t capacity)
{
    id ^= id >> 30; id *= UINT64_C(0xbf58476d1ce4e5b9);
    id ^= id >> 27; id *= UINT64_C(0x94d049bb133111eb);
    return (size_t)(id ^ (id >> 31)) & (capacity - 1);
}

void fl_scene_storage_index_entity(Fl_Scene *s, size_t position)
{
    Fl_EntityIndex *index=(Fl_EntityIndex *)s->identity_index;
    size_t slot=identity_hash(s->entities[position].id,s->identity_capacity);
    while (index[slot].id) slot=(slot+1)&(s->identity_capacity-1);
    index[slot]=(Fl_EntityIndex){s->entities[position].id,position};
}

/* Reserve lookup capacity before document mutations, so rebuilding cannot fail. */
bool fl_scene_storage_reserve_index(Fl_Scene *s, size_t count)
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
void fl_scene_storage_rebuild_index(Fl_Scene *s)
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
