#include "fl_animation.h"
#include "project/fl_project.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const Fl_Animation *fl_entity_animation(const Fl_Entity *e)
{
    const Fl_Component *c = fl_entity_component(e, FL_COMPONENT_ANIMATION);
    return c && c->version == FL_ANIMATION_VERSION && c->size == sizeof(Fl_Animation) ? (const Fl_Animation *)c->data : NULL;
}

Fl_Animation *fl_animation_require(Fl_Entity *e)
{
    Fl_Animation *a = (Fl_Animation *)fl_entity_animation(e);
    if (a || !e || e->component_count >= SIZE_MAX / sizeof(Fl_Component) - 1) return a;
    a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    Fl_Component *components = realloc((void *)e->components, (e->component_count + 1) * sizeof(*components));
    if (!components) {free(a); return NULL;}
    e->components = components;
    components[e->component_count++] = (Fl_Component){FL_COMPONENT_ANIMATION, FL_ANIMATION_VERSION, sizeof(*a), (unsigned char *)a};
    return a;
}

void fl_animation_destroy(Fl_Animation *a)
{
    if (!a) return;
    for (size_t i = 0; i < a->track_count; ++i) free(a->tracks[i].keys);
    free(a);
}

Fl_Animation *fl_animation_clone(const Fl_Animation *a)
{
    if (!a) return NULL;
    Fl_Binary bytes = {0}; fl_animation_write(a, &bytes);
    Fl_Animation *copy = bytes.failed ? NULL : fl_animation_read(&bytes);
    free(bytes.data);
    if (copy) for (size_t i = 0; i < copy->track_count; ++i) copy->tracks[i].entity = a->tracks[i].entity;
    return copy;
}

bool fl_animation_valid(const Fl_Animation *a)
{
    if (!a || a->track_count > FL_PROP_COUNT) return false;
    uint32_t properties = 0;
    for (size_t i = 0; i < a->track_count; ++i) {
        const Fl_Track *t = &a->tracks[i];
        if (!t->id || !t->entity || t->property < 0 || t->property >= FL_PROP_COUNT ||
            !t->key_count || t->key_count > t->key_capacity || !t->keys || (properties & (1u << t->property))) return false;
        properties |= 1u << t->property;
        for (size_t k = 0; k < t->key_count; ++k)
            if (!t->keys[k].id || t->keys[k].ease < 0 || t->keys[k].ease >= FL_EASE_COUNT ||
                !fl_property_value_valid(t->property, t->keys[k].value)) return false;
    }
    return true;
}

/* Encode/decode binary64 values independently of native structure layout. */
static void put_double(Fl_Binary *b, double v) {uint64_t bits; memcpy(&bits, &v, 8); fl_binary_u64(b, bits);}
static double get_double(Fl_Binary *b) {uint64_t bits = fl_binary_get_u64(b); double v; memcpy(&v, &bits, 8); return v;}

/* Serialize the track table with explicit little-endian fields. */
static void write_tracks(const Fl_Animation *a, Fl_Binary *b)
{
    fl_binary_u64(b, a->track_count);
    for (size_t i = 0; i < a->track_count; ++i) {
        const Fl_Track *t = &a->tracks[i];
        fl_binary_u64(b, t->id); fl_binary_u64(b, t->property); fl_binary_u64(b, t->key_count);
        for (size_t k = 0; k < t->key_count; ++k) {
            const Fl_Keyframe *key = &t->keys[k];
            fl_binary_u64(b, key->id); fl_binary_u64(b, key->at.kind); fl_binary_u64(b, key->at.beat);
            fl_binary_u64(b, key->at.end); fl_binary_u64(b, (uint64_t)key->at.offset);
            for (int c = 0; c < 4; ++c) put_double(b, key->value[c]);
            fl_binary_u64(b, key->ease);
        }
    }
}

/* Deserialize a bounded track table into an empty animation. */
static bool read_tracks(Fl_Animation *a, Fl_Binary *b)
{
    uint64_t count = fl_binary_get_u64(b);
    if (b->failed || count > FL_PROP_COUNT) return false;
    for (size_t i = 0; i < count && !b->failed; ++i) {
        Fl_Track *t = &a->tracks[a->track_count++];
        t->id = fl_binary_get_u64(b);
        uint64_t property = fl_binary_get_u64(b), keys = fl_binary_get_u64(b);
        if (b->failed || property >= FL_PROP_COUNT || keys > SIZE_MAX / sizeof(Fl_Keyframe) || keys > (b->size - b->cursor) / 80) {b->failed = true; break;}
        t->property = (Fl_AnimProperty)property;
        t->keys = keys ? calloc((size_t)keys, sizeof(*t->keys)) : NULL;
        if (keys && !t->keys) {b->failed = true; break;}
        t->key_count = t->key_capacity = (size_t)keys;
        for (size_t k = 0; k < keys && !b->failed; ++k) {
            Fl_Keyframe *key = &t->keys[k];
            key->id = fl_binary_get_u64(b);
            uint64_t kind = fl_binary_get_u64(b); key->at.beat = fl_binary_get_u64(b);
            uint64_t end = fl_binary_get_u64(b), offset = fl_binary_get_u64(b);
            memcpy(&key->at.offset, &offset, sizeof(offset));
            key->at.kind = (Fl_AnchorKind)kind; key->at.end = end != 0;
            for (int c = 0; c < 4; ++c) key->value[c] = get_double(b);
            uint64_t ease = fl_binary_get_u64(b); key->ease = (Fl_Ease)ease;
            if (kind > FL_ANCHOR_BEAT || end > 1 || ease >= FL_EASE_COUNT) b->failed = true;
        }
    }
    return !b->failed;
}

bool fl_property_value_valid(Fl_AnimProperty property, const double v[4])
{
    if (!v) return false;
    for (int i = 0; i < 4; ++i) if (!isfinite(v[i])) return false;
    switch (property) {
    case FL_PROP_X: case FL_PROP_Y: case FL_PROP_ROTATION: case FL_PROP_SCALE_X: case FL_PROP_SCALE_Y:
        return fabs(v[0]) <= 1e9;
    case FL_PROP_WIDTH: case FL_PROP_HEIGHT: return v[0] >= 0 && v[0] <= 1e9;
    case FL_PROP_OPACITY: return v[0] >= 0 && v[0] <= 1;
    case FL_PROP_FILL: case FL_PROP_STROKE:
        for (int i = 0; i < 4; ++i) if (v[i] < 0 || v[i] > 255) return false;
        return true;
    case FL_PROP_STROKE_WIDTH: return v[0] >= 0 && v[0] <= 65536;
    case FL_PROP_FONT_SIZE: return v[0] >= 1 && v[0] <= 2048;
    case FL_PROP_CROP_X: case FL_PROP_CROP_Y: return v[0] >= 0 && v[0] <= 1;
    case FL_PROP_CROP_W: case FL_PROP_CROP_H: return v[0] >= 0.001 && v[0] <= 1;
    default: return false;
    }
}


void fl_animation_write(const Fl_Animation *a, Fl_Binary *b) {write_tracks(a, b);}

Fl_Animation *fl_animation_read(Fl_Binary *b)
{
    Fl_Animation *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    if (!read_tracks(a, b)) {fl_animation_destroy(a); return NULL;}
    return a;
}
