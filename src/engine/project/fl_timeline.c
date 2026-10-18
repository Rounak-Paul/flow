#include "fl_timeline_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TICK_LIMIT (INT64_C(1) << 62)

/* Duplicate a caller string into owned storage. */
static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

/* Write/read IEEE754 doubles and signed ticks through the u64 stream encoding. */
static void put_double(Fl_Binary *b, double value) { uint64_t bits; memcpy(&bits, &value, 8); fl_binary_u64(b, bits); }
static double get_double(Fl_Binary *b) { uint64_t bits = fl_binary_get_u64(b); double value; memcpy(&value, &bits, 8); return value; }
static void put_tick(Fl_Binary *b, Fl_Tick value) { fl_binary_u64(b, (uint64_t)value); }
static Fl_Tick get_tick(Fl_Binary *b) { uint64_t bits = fl_binary_get_u64(b); Fl_Tick value; memcpy(&value, &bits, 8); return value; }

Fl_Tick fl_scene_length(const Fl_Scene *s)
{
    if (!s) return 0;
    if (!s->beat_count) return s->duration <= INT64_MAX ? (Fl_Tick)s->duration : INT64_MAX;
    Fl_Tick total = 0;
    for (size_t i = 0; i < s->beat_count; ++i) total += (Fl_Tick)s->beats[i].duration;
    return total;
}

bool fl_scene_beat_range(const Fl_Scene *s, Fl_Id beat, Fl_Tick *start, Fl_Tick *end)
{
    Fl_Tick at = 0;
    for (size_t i = 0; s && i < s->beat_count; ++i) {
        if (s->beats[i].id == beat) {
            if (start) *start = at;
            if (end) *end = at + (Fl_Tick)s->beats[i].duration;
            return true;
        }
        at += (Fl_Tick)s->beats[i].duration;
    }
    return false;
}

Fl_Tick fl_anchor_resolve(const Fl_Scene *s, const Fl_TimeAnchor *a)
{
    if (!a) return 0;
    Fl_Tick start, end;
    if (a->kind == FL_ANCHOR_BEAT && fl_scene_beat_range(s, a->beat, &start, &end)) return (a->end ? end : start) + a->offset;
    return a->offset;
}

Fl_TimeAnchor fl_anchor_at(const Fl_Scene *s, Fl_Tick tick)
{
    Fl_Tick at = 0;
    for (size_t i = 0; s && i < s->beat_count; ++i) {
        Fl_Tick end = at + (Fl_Tick)s->beats[i].duration;
        if (tick < end || i + 1 == s->beat_count) return (Fl_TimeAnchor){FL_ANCHOR_BEAT, s->beats[i].id, false, tick - at};
        at = end;
    }
    return (Fl_TimeAnchor){FL_ANCHOR_ABSOLUTE, 0, false, tick};
}

const Fl_Track *fl_scene_track(const Fl_Scene *s, Fl_Id entity, Fl_AnimProperty property)
{
    for (size_t i = 0; s && i < s->track_count; ++i)
        if (s->tracks[i].entity == entity && s->tracks[i].property == property) return &s->tracks[i];
    return NULL;
}

const Fl_Keyframe *fl_scene_key(const Fl_Scene *s, Fl_Id key, const Fl_Track **track)
{
    for (size_t i = 0; s && key && i < s->track_count; ++i)
        for (size_t j = 0; j < s->tracks[i].key_count; ++j)
            if (s->tracks[i].keys[j].id == key) { if (track) *track = &s->tracks[i]; return &s->tracks[i].keys[j]; }
    return NULL;
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
    default: return false;
    }
}

/* Validate an anchor against the scene's beats. */
static bool anchor_valid(const Fl_Scene *s, const Fl_TimeAnchor *a)
{
    if (a->offset < -TICK_LIMIT || a->offset > TICK_LIMIT) return false;
    if (a->kind == FL_ANCHOR_ABSOLUTE) return a->beat == 0 && !a->end;
    return a->kind == FL_ANCHOR_BEAT && fl_scene_beat_range(s, a->beat, NULL, NULL);
}

/* Validate a clip record's references and ranges. */
static bool clip_valid(const Fl_Project *p, const Fl_Scene *s, const Fl_AudioClip *c)
{
    const Fl_Asset *asset = fl_project_asset(p, c->asset);
    return asset && asset->kind == FL_ASSET_AUDIO && anchor_valid(s, &c->start) && isfinite(c->gain) && c->gain >= 0 && c->gain <= 4 &&
           c->source_offset <= (uint64_t)TICK_LIMIT && c->duration <= (uint64_t)TICK_LIMIT &&
           c->fade_in <= (uint64_t)TICK_LIMIT && c->fade_out <= (uint64_t)TICK_LIMIT;
}

/* Recompute the scene duration from its beats. */
static void sync_duration(Fl_Scene *s)
{
    if (s->beat_count) s->duration = (uint64_t)fl_scene_length(s);
}

/* Sum of beat durations if beat index skip were replaced by duration; false on overflow. */
static bool length_fits(const Fl_Scene *s, size_t skip, uint64_t duration)
{
    uint64_t total = duration;
    for (size_t i = 0; i < s->beat_count; ++i) {
        if (i == skip) continue;
        if (s->beats[i].duration > (uint64_t)TICK_LIMIT - total) return false;
        total += s->beats[i].duration;
    }
    return total <= (uint64_t)TICK_LIMIT;
}

/* Scale keys inside this beat, preserving their chosen start/end edge and IDs. */
static void scale_beat_keys(Fl_Scene *s, Fl_Id beat, uint64_t before, uint64_t after)
{
    if (before == after) return;
    for (size_t i = 0; i < s->track_count; ++i) {
        Fl_Track *track = (Fl_Track *)&s->tracks[i];
        for (size_t k = 0; k < track->key_count; ++k) {
            Fl_TimeAnchor *at = &track->keys[k].at;
            if (at->kind != FL_ANCHOR_BEAT || at->beat != beat) continue;
            if (at->end ? at->offset > 0 || at->offset < -(Fl_Tick)before :
                at->offset < 0 || (uint64_t)at->offset > before) continue;
            Fl_Tick offset = (Fl_Tick)llroundl((long double)at->offset / before * after);
            if (!at->end && offset > (Fl_Tick)after) offset = (Fl_Tick)after;
            if (at->end && offset < -(Fl_Tick)after) offset = -(Fl_Tick)after;
            at->offset = offset;
        }
    }
}

/* Rewrite anchors on beat as absolute times so removing it keeps their timing. */
static void detach_anchors(Fl_Scene *s, Fl_Id beat)
{
    for (size_t i = 0; i < s->track_count; ++i) {
        Fl_Track *t = (Fl_Track *)&s->tracks[i];
        for (size_t j = 0; j < t->key_count; ++j)
            if (t->keys[j].at.kind == FL_ANCHOR_BEAT && t->keys[j].at.beat == beat)
                t->keys[j].at = (Fl_TimeAnchor){FL_ANCHOR_ABSOLUTE, 0, false, fl_anchor_resolve(s, &t->keys[j].at)};
    }
    for (size_t i = 0; i < s->clip_count; ++i) {
        Fl_AudioClip *c = (Fl_AudioClip *)&s->clips[i];
        if (c->start.kind == FL_ANCHOR_BEAT && c->start.beat == beat)
            c->start = (Fl_TimeAnchor){FL_ANCHOR_ABSOLUTE, 0, false, fl_anchor_resolve(s, &c->start)};
    }
}

/* Locate a beat's index; SIZE_MAX when absent. */
static size_t beat_index(const Fl_Scene *s, Fl_Id beat)
{
    for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == beat) return i;
    return SIZE_MAX;
}

/* Grow beat storage for extra beats; the scene's beats are unchanged. */
static bool reserve_beats(Fl_Scene *s, size_t extra)
{
    if (s->beat_count > SIZE_MAX / sizeof(Fl_Beat) - extra) return false;
    Fl_Beat *beats = realloc((void *)s->beats, (s->beat_count + extra) * sizeof(*beats));
    if (beats) s->beats = beats;
    return beats != NULL;
}

/* Insert into reserved storage at position. */
static void place_beat(Fl_Scene *s, size_t position, Fl_Beat beat)
{
    Fl_Beat *beats = (Fl_Beat *)s->beats;
    memmove(beats + position + 1, beats + position, (s->beat_count - position) * sizeof(*beats));
    beats[position] = beat; ++s->beat_count;
}

/* Ensure capacity for one more track. */
static bool grow_tracks(Fl_Scene *s, size_t extra)
{
    if (s->track_count + extra <= s->track_capacity) return true;
    size_t capacity = s->track_capacity ? s->track_capacity : 4;
    while (capacity < s->track_count + extra) capacity *= 2;
    if (capacity > SIZE_MAX / sizeof(Fl_Track)) return false;
    Fl_Track *tracks = realloc((void *)s->tracks, capacity * sizeof(*tracks));
    if (!tracks) return false;
    s->tracks = tracks; s->track_capacity = capacity;
    return true;
}

/* Ensure a track can hold one more key. */
static bool grow_keys(Fl_Track *t)
{
    if (t->key_count < t->key_capacity) return true;
    size_t capacity = t->key_capacity ? t->key_capacity * 2 : 4;
    if (capacity > SIZE_MAX / sizeof(Fl_Keyframe)) return false;
    Fl_Keyframe *keys = realloc(t->keys, capacity * sizeof(*keys));
    if (!keys) return false;
    t->keys = keys; t->key_capacity = capacity;
    return true;
}

/* Remove track index i, releasing its keys. */
static void remove_track(Fl_Scene *s, size_t i)
{
    Fl_Track *tracks = (Fl_Track *)s->tracks;
    free(tracks[i].keys);
    memmove(tracks + i, tracks + i + 1, (s->track_count - i - 1) * sizeof(*tracks));
    --s->track_count;
}

/* Validate ease/anchor/value of a key operation for entity's property. */
static bool key_op_valid(const Fl_Scene *s, const Fl_Op *op)
{
    return fl_scene_entity(s, op->entity) && op->property >= 0 && op->property < FL_PROP_COUNT &&
           op->ease >= 0 && op->ease < FL_EASE_COUNT && anchor_valid(s, &op->anchor) && fl_property_value_valid(op->property, op->value);
}

bool fl_timeline_apply(const Fl_Project *p, Fl_Scene *s, const Fl_Op *op, uint64_t *next_id, Fl_Id *created)
{
    if (!s) return false;
    switch (op->kind) {
    case FL_OP_BEAT_ADD: {
        size_t needed = s->beat_count ? 1 : 2;
        if (!op->name || !op->name[0] || !op->beat_duration || op->position > s->beat_count ||
            *next_id > UINT64_MAX - needed || !length_fits(s, SIZE_MAX, op->beat_duration + (s->beat_count ? 0 : s->duration))) return false;
        char *name = copy_text(op->name), *first = s->beat_count ? NULL : copy_text("Beat 1");
        if (!name || (!s->beat_count && !first) || !reserve_beats(s, needed)) { free(name); free(first); return false; }
        size_t position = op->position;
        if (first) { place_beat(s, 0, (Fl_Beat){(*next_id)++, first, s->duration, false}); position = 1; }
        place_beat(s, position, (Fl_Beat){*next_id, name, op->beat_duration, op->auto_advance});
        *created = (*next_id)++;
        sync_duration(s);
        return true;
    }
    case FL_OP_BEAT_REMOVE: {
        size_t i = beat_index(s, op->beat);
        if (i == SIZE_MAX || s->beat_count < 2) return false;
        detach_anchors(s, op->beat);
        Fl_Beat *beats = (Fl_Beat *)s->beats;
        free((void *)beats[i].name);
        memmove(beats + i, beats + i + 1, (s->beat_count - i - 1) * sizeof(*beats));
        --s->beat_count;
        sync_duration(s);
        return true;
    }
    case FL_OP_BEAT_UPDATE: {
        size_t i = beat_index(s, op->beat);
        if (i == SIZE_MAX || !op->name || !op->name[0] || !op->beat_duration || !length_fits(s, i, op->beat_duration)) return false;
        char *name = copy_text(op->name);
        if (!name) return false;
        Fl_Beat *b = (Fl_Beat *)&s->beats[i];
        if (op->scale_keys) scale_beat_keys(s, b->id, b->duration, op->beat_duration);
        free((void *)b->name);
        b->name = name; b->duration = op->beat_duration; b->auto_advance = op->auto_advance;
        sync_duration(s);
        return true;
    }
    case FL_OP_BEAT_MOVE: {
        size_t from = beat_index(s, op->beat), to = op->position;
        if (from == SIZE_MAX || to >= s->beat_count) return false;
        Fl_Beat *beats = (Fl_Beat *)s->beats, moved = beats[from];
        if (from < to) memmove(beats + from, beats + from + 1, (to - from) * sizeof(*beats));
        else memmove(beats + to + 1, beats + to, (from - to) * sizeof(*beats));
        beats[to] = moved;
        return true;
    }
    case FL_OP_KEY_SET: {
        if (!key_op_valid(s, op)) return false;
        Fl_Keyframe value = {0, op->anchor, {op->value[0], op->value[1], op->value[2], op->value[3]}, op->ease};
        if (op->key) {
            const Fl_Track *owner;
            Fl_Keyframe *key = (Fl_Keyframe *)fl_scene_key(s, op->key, &owner);
            if (!key || owner->entity != op->entity || owner->property != op->property) return false;
            value.id = key->id; *key = value;
            return true;
        }
        if (*next_id >= UINT64_MAX - 1) return false;
        Fl_Track *t = (Fl_Track *)fl_scene_track(s, op->entity, op->property);
        Fl_Tick at = fl_anchor_resolve(s, &op->anchor);
        for (size_t i = 0; t && i < t->key_count; ++i) if (fl_anchor_resolve(s, &t->keys[i].at) == at) {
            value.id = t->keys[i].id; t->keys[i] = value; *created = value.id;
            return true;
        }
        if (!t) {
            Fl_Keyframe *keys = malloc(4 * sizeof(*keys));
            if (!keys || !grow_tracks(s, 1)) { free(keys); return false; }
            t = (Fl_Track *)&s->tracks[s->track_count++];
            *t = (Fl_Track){(*next_id)++, op->entity, op->property, keys, 0, 4};
        } else if (!grow_keys(t)) return false;
        value.id = (*next_id)++;
        t->keys[t->key_count++] = value;
        *created = value.id;
        return true;
    }
    case FL_OP_KEY_REMOVE: {
        const Fl_Track *owner;
        const Fl_Keyframe *key = fl_scene_key(s, op->key, &owner);
        if (!key) return false;
        Fl_Track *t = (Fl_Track *)owner;
        size_t index = (size_t)(key - t->keys);
        memmove(t->keys + index, t->keys + index + 1, (t->key_count - index - 1) * sizeof(*t->keys));
        if (!--t->key_count) remove_track(s, (size_t)(t - s->tracks));
        return true;
    }
    case FL_OP_TRACK_REMOVE:
        for (size_t i = 0; i < s->track_count; ++i) if (s->tracks[i].id == op->track) { remove_track(s, i); return true; }
        return false;
    case FL_OP_CLIP_ADD: case FL_OP_CLIP_UPDATE: {
        if (!op->clip_record || !clip_valid(p, s, op->clip_record)) return false;
        if (op->kind == FL_OP_CLIP_UPDATE) {
            for (size_t i = 0; i < s->clip_count; ++i) if (s->clips[i].id == op->clip) {
                Fl_AudioClip *c = (Fl_AudioClip *)&s->clips[i];
                *c = *op->clip_record; c->id = op->clip;
                return true;
            }
            return false;
        }
        if (*next_id == UINT64_MAX || s->clip_count >= SIZE_MAX / sizeof(Fl_AudioClip) - 1) return false;
        Fl_AudioClip *clips = realloc((void *)s->clips, (s->clip_count + 1) * sizeof(*clips));
        if (!clips) return false;
        s->clips = clips;
        clips[s->clip_count] = *op->clip_record;
        clips[s->clip_count].id = *created = (*next_id)++;
        ++s->clip_count;
        return true;
    }
    case FL_OP_CLIP_REMOVE:
        for (size_t i = 0; i < s->clip_count; ++i) if (s->clips[i].id == op->clip) {
            Fl_AudioClip *clips = (Fl_AudioClip *)s->clips;
            memmove(clips + i, clips + i + 1, (s->clip_count - i - 1) * sizeof(*clips));
            --s->clip_count;
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* Find the key of track resolving exactly to tick; NULL when none. */
static Fl_Keyframe *key_at(const Fl_Scene *s, Fl_Track *t, Fl_Tick tick)
{
    for (size_t i = 0; i < t->key_count; ++i) if (fl_anchor_resolve(s, &t->keys[i].at) == tick) return &t->keys[i];
    return NULL;
}

bool fl_timeline_reserve(Fl_Scene *s, const Fl_KeyWrite *writes, size_t count, uint64_t next_id, size_t *track_mark)
{
    *track_mark = s->track_count;
    if (count > (UINT64_MAX - next_id) / 2 || !grow_tracks(s, count)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!fl_property_value_valid(writes[i].property, writes[i].value)) { fl_timeline_rollback(s, *track_mark); return false; }
        Fl_Track *t = (Fl_Track *)fl_scene_track(s, writes[i].entity, writes[i].property);
        if (!t) {
            t = (Fl_Track *)&s->tracks[s->track_count++];
            *t = (Fl_Track){0, writes[i].entity, writes[i].property, NULL, 0, 0};
        }
        if (!grow_keys(t)) { fl_timeline_rollback(s, *track_mark); return false; }
    }
    return true;
}

void fl_timeline_rollback(Fl_Scene *s, size_t track_mark)
{
    while (s->track_count > track_mark) { Fl_Track *t = (Fl_Track *)&s->tracks[--s->track_count]; free(t->keys); }
}

void fl_timeline_commit(Fl_Scene *s, const Fl_KeyWrite *writes, size_t count, Fl_Tick tick, uint64_t *next_id)
{
    for (size_t i = 0; i < count; ++i) {
        Fl_Track *t = (Fl_Track *)fl_scene_track(s, writes[i].entity, writes[i].property);
        if (!t->id) t->id = (*next_id)++;
        Fl_Keyframe *key = key_at(s, t, tick);
        if (key) { memcpy(key->value, writes[i].value, sizeof(key->value)); continue; }
        Fl_Ease ease = t->key_count ? t->keys[t->key_count - 1].ease : FL_EASE_IN_OUT;
        Fl_Keyframe added = {(*next_id)++, fl_anchor_at(s, tick), {0}, ease};
        memcpy(added.value, writes[i].value, sizeof(added.value));
        t->keys[t->key_count++] = added;
    }
}

void fl_timeline_prune(Fl_Scene *s)
{
    for (size_t i = s->track_count; i > 0; --i) if (!fl_scene_entity(s, s->tracks[i - 1].entity)) remove_track(s, i - 1);
}

size_t fl_timeline_asset_uses(const Fl_Scene *s, Fl_Id asset)
{
    size_t uses = 0;
    for (size_t i = 0; s && i < s->clip_count; ++i) uses += s->clips[i].asset == asset;
    return uses;
}

bool fl_timeline_valid(const Fl_Project *p, const Fl_Scene *s, uint64_t next_id)
{
    uint64_t total = 0;
    for (size_t i = 0; i < s->beat_count; ++i) {
        const Fl_Beat *b = &s->beats[i];
        if (!b->id || b->id >= next_id || !b->name || !b->name[0] || !b->duration || b->duration > (uint64_t)TICK_LIMIT - total) return false;
        total += b->duration;
    }
    if (s->beat_count && total != s->duration) return false;
    for (size_t i = 0; i < s->track_count; ++i) {
        const Fl_Track *t = &s->tracks[i];
        if (!t->id || t->id >= next_id || !fl_scene_entity(s, t->entity) || t->property < 0 || t->property >= FL_PROP_COUNT || !t->key_count) return false;
        for (size_t j = 0; j < i; ++j) if (s->tracks[j].entity == t->entity && s->tracks[j].property == t->property) return false;
        for (size_t k = 0; k < t->key_count; ++k) {
            const Fl_Keyframe *key = &t->keys[k];
            if (!key->id || key->id >= next_id || key->ease < 0 || key->ease >= FL_EASE_COUNT || !anchor_valid(s, &key->at) ||
                !fl_property_value_valid(t->property, key->value)) return false;
        }
    }
    for (size_t i = 0; i < s->clip_count; ++i)
        if (!s->clips[i].id || s->clips[i].id >= next_id || !clip_valid(p, s, &s->clips[i])) return false;
    return true;
}

size_t fl_timeline_id_count(const Fl_Scene *s)
{
    size_t n = s->beat_count + s->track_count + s->clip_count;
    for (size_t i = 0; i < s->track_count; ++i) n += s->tracks[i].key_count;
    return n;
}

void fl_timeline_ids(const Fl_Scene *s, Fl_Id *ids)
{
    size_t n = 0;
    for (size_t i = 0; i < s->beat_count; ++i) ids[n++] = s->beats[i].id;
    for (size_t i = 0; i < s->track_count; ++i) {
        ids[n++] = s->tracks[i].id;
        for (size_t k = 0; k < s->tracks[i].key_count; ++k) ids[n++] = s->tracks[i].keys[k].id;
    }
    for (size_t i = 0; i < s->clip_count; ++i) ids[n++] = s->clips[i].id;
}

void fl_timeline_free(Fl_Scene *s)
{
    for (size_t i = 0; i < s->beat_count; ++i) free((void *)s->beats[i].name);
    for (size_t i = 0; i < s->track_count; ++i) free(s->tracks[i].keys);
    free((void *)s->beats); free((void *)s->tracks); free((void *)s->clips);
    s->beats = NULL; s->tracks = NULL; s->clips = NULL;
    s->beat_count = s->track_count = s->track_capacity = s->clip_count = 0;
}

/* Encode an anchor as kind, beat, end, offset. */
static void put_anchor(Fl_Binary *b, const Fl_TimeAnchor *a)
{
    fl_binary_u64(b, (uint64_t)a->kind); fl_binary_u64(b, a->beat); fl_binary_u64(b, a->end); put_tick(b, a->offset);
}
static Fl_TimeAnchor get_anchor(Fl_Binary *b)
{
    uint64_t kind = fl_binary_get_u64(b), beat = fl_binary_get_u64(b), end = fl_binary_get_u64(b);
    Fl_TimeAnchor a = {kind == FL_ANCHOR_BEAT ? FL_ANCHOR_BEAT : FL_ANCHOR_ABSOLUTE, beat, end != 0, get_tick(b)};
    if (kind > FL_ANCHOR_BEAT || end > 1) b->failed = true;
    return a;
}

void fl_timeline_save(const Fl_Scene *s, Fl_Binary *b)
{
    fl_binary_u64(b, s->beat_count);
    for (size_t i = 0; i < s->beat_count; ++i) {
        fl_binary_u64(b, s->beats[i].id); fl_binary_string(b, s->beats[i].name);
        fl_binary_u64(b, s->beats[i].duration); fl_binary_u64(b, s->beats[i].auto_advance);
    }
    fl_binary_u64(b, s->track_count);
    for (size_t i = 0; i < s->track_count; ++i) {
        const Fl_Track *t = &s->tracks[i];
        fl_binary_u64(b, t->id); fl_binary_u64(b, t->entity); fl_binary_u64(b, (uint64_t)t->property); fl_binary_u64(b, t->key_count);
        for (size_t k = 0; k < t->key_count; ++k) {
            fl_binary_u64(b, t->keys[k].id); put_anchor(b, &t->keys[k].at);
            for (int c = 0; c < 4; ++c) put_double(b, t->keys[k].value[c]);
            fl_binary_u64(b, (uint64_t)t->keys[k].ease);
        }
    }
    fl_binary_u64(b, s->clip_count);
    for (size_t i = 0; i < s->clip_count; ++i) {
        const Fl_AudioClip *c = &s->clips[i];
        fl_binary_u64(b, c->id); fl_binary_u64(b, c->asset); put_anchor(b, &c->start);
        fl_binary_u64(b, c->source_offset); fl_binary_u64(b, c->duration); put_double(b, c->gain);
        fl_binary_u64(b, c->fade_in); fl_binary_u64(b, c->fade_out);
    }
}

/* Read a count whose records need at least minimum bytes each. */
static size_t get_count(Fl_Binary *b, size_t stride, size_t minimum)
{
    uint64_t count = fl_binary_get_u64(b);
    if (b->failed || count > SIZE_MAX / stride || count > (b->size - b->cursor) / minimum) { b->failed = true; return 0; }
    return (size_t)count;
}

bool fl_timeline_load(Fl_Scene *s, Fl_Binary *b)
{
    size_t beats = get_count(b, sizeof(Fl_Beat), 32);
    Fl_Beat *beat_storage = beats ? calloc(beats, sizeof(*beat_storage)) : NULL;
    if (beats && !beat_storage) return false;
    s->beats = beat_storage;
    for (size_t i = 0; !b->failed && i < beats; ++i, ++s->beat_count) {
        Fl_Beat *beat = &beat_storage[i];
        beat->id = fl_binary_get_u64(b); beat->name = fl_binary_get_string(b);
        beat->duration = fl_binary_get_u64(b);
        uint64_t flags = fl_binary_get_u64(b);
        beat->auto_advance = flags & 1;
        if (flags > 1) b->failed = true;
    }
    size_t tracks = b->failed ? 0 : get_count(b, sizeof(Fl_Track), 32);
    if (tracks && !grow_tracks(s, tracks)) return false;
    for (size_t i = 0; !b->failed && i < tracks; ++i) {
        Fl_Track *t = (Fl_Track *)&s->tracks[s->track_count++];
        *t = (Fl_Track){0};
        t->id = fl_binary_get_u64(b); t->entity = fl_binary_get_u64(b);
        uint64_t property = fl_binary_get_u64(b);
        t->property = property < FL_PROP_COUNT ? (Fl_AnimProperty)property : FL_PROP_X;
        if (property >= FL_PROP_COUNT) b->failed = true;
        size_t keys = b->failed ? 0 : get_count(b, sizeof(Fl_Keyframe), 80);
        t->keys = keys ? calloc(keys, sizeof(*t->keys)) : NULL;
        if (keys && !t->keys) return false;
        t->key_capacity = keys;
        for (size_t k = 0; !b->failed && k < keys; ++k, ++t->key_count) {
            Fl_Keyframe *key = &t->keys[k];
            key->id = fl_binary_get_u64(b); key->at = get_anchor(b);
            for (int c = 0; c < 4; ++c) key->value[c] = get_double(b);
            uint64_t ease = fl_binary_get_u64(b);
            key->ease = ease < FL_EASE_COUNT ? (Fl_Ease)ease : FL_EASE_LINEAR;
            if (ease >= FL_EASE_COUNT) b->failed = true;
        }
    }
    size_t clips = b->failed ? 0 : get_count(b, sizeof(Fl_AudioClip), 88);
    Fl_AudioClip *clip_storage = clips ? calloc(clips, sizeof(*clip_storage)) : NULL;
    if (clips && !clip_storage) return false;
    s->clips = clip_storage;
    for (size_t i = 0; !b->failed && i < clips; ++i, ++s->clip_count) {
        Fl_AudioClip *c = &clip_storage[i];
        c->id = fl_binary_get_u64(b); c->asset = fl_binary_get_u64(b); c->start = get_anchor(b);
        c->source_offset = fl_binary_get_u64(b); c->duration = fl_binary_get_u64(b); c->gain = get_double(b);
        c->fade_in = fl_binary_get_u64(b); c->fade_out = fl_binary_get_u64(b);
    }
    return !b->failed;
}
