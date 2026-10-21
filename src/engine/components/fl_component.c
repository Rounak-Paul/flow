#include "fl_component.h"
#include "project/fl_project.h"
#include "fl_animation.h"
#include "fl_script.h"
#include <stdlib.h>
#include <string.h>

bool fl_component_clone(const Fl_Component *c, Fl_Component *out)
{
    Fl_Component copy = *c;
    if (c->type == FL_COMPONENT_ANIMATION) {
        copy.data = (unsigned char *)fl_animation_clone((const Fl_Animation *)c->data);
        if (!copy.data) return false;
    } else {
        unsigned char *bytes = c->size ? malloc(c->size) : NULL;
        if (c->size && !bytes) return false;
        if (c->size) memcpy(bytes, c->data, c->size);
        copy.data = bytes;
    }
    *out = copy;
    return true;
}

void fl_component_clear(Fl_Component *c)
{
    if (c->type == FL_COMPONENT_ANIMATION) fl_animation_destroy((Fl_Animation *)c->data);
    else free((void *)c->data);
    *c = (Fl_Component){0};
}

void fl_component_write(const Fl_Component *c, Fl_Binary *b)
{
    Fl_Binary payload = {0};
    const void *data = c->data; size_t size = c->size;
    if (c->type == FL_COMPONENT_ANIMATION) {
        fl_animation_write((const Fl_Animation *)c->data, &payload);
        data = payload.data; size = payload.size;
        if (payload.failed) b->failed = true;
    }
    fl_binary_u64(b,c->id);
    fl_binary_u64(b, c->type); fl_binary_u64(b, c->version); fl_binary_u64(b, size);
    fl_binary_write(b, data, size);
    free(payload.data);
}

bool fl_component_read(Fl_Component *c, Fl_Binary *b)
{
    c->id=fl_binary_get_u64(b);
    c->type = fl_binary_get_u64(b); c->version = fl_binary_get_u64(b);
    uint64_t size = fl_binary_get_u64(b);
    if (b->failed || size > SIZE_MAX || size > b->size - b->cursor) return false;
    if (c->type == FL_COMPONENT_ANIMATION) {
        if (c->version != FL_ANIMATION_VERSION) return false;
        Fl_Binary payload = {.data = b->data + b->cursor, .size = (size_t)size};
        Fl_Animation *a = fl_animation_read(&payload);
        if (!a || payload.failed || payload.cursor != payload.size) {fl_animation_destroy(a); return false;}
        c->data = (unsigned char *)a; c->size = sizeof(*a);
    } else {
        unsigned char *data = size ? malloc((size_t)size) : NULL;
        if (size && !data) return false;
        if (size) memcpy(data, b->data + b->cursor, (size_t)size);
        c->data = data; c->size = (size_t)size;
    }
    b->cursor += (size_t)size;
    return true;
}

bool fl_entity_remove_component(Fl_Entity *e, uint64_t type)
{
    if (!e || type == FL_COMPONENT_TRANSFORM) return false;
    Fl_Component *c = (Fl_Component *)fl_entity_component(e, type);
    if (!c) return false;
    size_t index = (size_t)(c - e->components);
    fl_component_clear(c);
    memmove(c, c + 1, (e->component_count - index - 1) * sizeof(*c));
    --e->component_count;
    return true;
}

const char *fl_component_name(uint64_t type)
{
    static const char *names[] = {NULL, "Transform", "Rectangle", "Text", "Appearance", "Geometry", "Typography", "Image", "Opacity", "Animation", "Lua Script", "Style", "Video", "Flow Prototype"};
    return type < sizeof(names) / sizeof(*names) ? names[type] : NULL;
}

const Fl_Component *fl_entity_component(const Fl_Entity *e, uint64_t type)
{
    if (e) for (size_t i = 0; i < e->component_count; ++i) if (e->components[i].type == type) return &e->components[i];
    return NULL;
}


const Fl_Component *fl_entity_component_instance(const Fl_Entity *entity, Fl_Id id)
{
    for (size_t i=0;entity && id && i<entity->component_count;++i) if (entity->components[i].id==id) return &entity->components[i];
    return NULL;
}

bool fl_entity_remove_component_instance(Fl_Entity *entity, Fl_Id id)
{
    Fl_Component *c=(Fl_Component *)fl_entity_component_instance(entity,id);
    if (!c || c->type==FL_COMPONENT_TRANSFORM) return false;
    size_t index=(size_t)(c-entity->components); fl_component_clear(c);
    memmove(c,c+1,(entity->component_count-index-1)*sizeof(*c)); --entity->component_count; return true;
}
