#include "fl_timeline_internal.h"
#include "components/fl_animation.h"
#include "components/fl_component.h"
#include "components/fl_script.h"
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
    if (a->kind == FL_ANCHOR_BEAT && fl_scene_beat_range(s, a->beat, &start, &end)) {
        Fl_Tick base = a->end ? end : start;
        if (a->offset > 0 && base > INT64_MAX - a->offset) return INT64_MAX;
        return base + a->offset;
    }
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

const Fl_Track *fl_scene_track_next(Fl_TrackIterator *it)
{
    while (it && it->scene && it->entity < it->scene->entity_count) {
        const Fl_Animation *a = fl_entity_animation(&it->scene->entities[it->entity]);
        if (a && it->track < a->timeline.track_count) return &a->timeline.tracks[it->track++];
        ++it->entity; it->track = 0;
    }
    return NULL;
}

size_t fl_scene_track_count(const Fl_Scene *s)
{
    size_t count = 0;
    for (size_t i = 0; s && i < s->entity_count; ++i) {
        const Fl_Animation *a = fl_entity_animation(&s->entities[i]);
        if (a) count += a->timeline.track_count;
    }
    return count;
}

const Fl_Track *fl_scene_track_at(const Fl_Scene *s, size_t index)
{
    for (size_t i = 0; s && i < s->entity_count; ++i) {
        const Fl_Animation *a = fl_entity_animation(&s->entities[i]);
        if (!a) continue;
        if (index < a->timeline.track_count) return &a->timeline.tracks[index];
        index -= a->timeline.track_count;
    }
    return NULL;
}

const Fl_Track *fl_scene_track(const Fl_Scene *s, Fl_Id entity, Fl_AnimProperty property)
{
    const Fl_Animation *a = fl_entity_animation(fl_scene_entity(s, entity));
    for (size_t i = 0; a && i < a->timeline.track_count; ++i)
        if (a->timeline.tracks[i].property == property) return &a->timeline.tracks[i];
    return NULL;
}

const Fl_Keyframe *fl_scene_key(const Fl_Scene *s, Fl_Id key, const Fl_Track **track)
{
    Fl_TrackIterator it = {.scene = s};
    for (const Fl_Track *t; key && (t = fl_scene_track_next(&it));)
        for (size_t k = 0; k < t->key_count; ++k)
            if (t->keys[k].id == key) {if (track) *track = t; return &t->keys[k];}
    return NULL;
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
    if (duration > (uint64_t)TICK_LIMIT) return false;
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
    Fl_TrackIterator tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&tracks));) {
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

/* Reassign a split beat's anchor while preserving its resolved time and edge semantics. */
static void split_anchor(Fl_TimeAnchor *at, Fl_Id beat, Fl_Id right, Fl_Tick left_duration, Fl_Tick right_duration)
{
    if (at->kind != FL_ANCHOR_BEAT || at->beat != beat) return;
    if (at->end) {
        if (at->offset >= -right_duration) at->beat = right;
        else at->offset += right_duration;
    } else if (at->offset >= left_duration) {
        at->beat = right; at->offset -= left_duration;
    }
}

/* Delete animation owned by this beat; preserve audio starts as absolute times. */
static void remove_beat_content(Fl_Scene *s, Fl_Id beat)
{
    Fl_TrackIterator it = {.scene = s};
    for (const Fl_Track *value; (value = fl_scene_track_next(&it));) {
        Fl_Track *track = (Fl_Track *)value;
        size_t kept = 0;
        for (size_t k = 0; k < track->key_count; ++k) {
            const Fl_Keyframe *key = &track->keys[k];
            if (key->at.kind != FL_ANCHOR_BEAT || key->at.beat != beat) track->keys[kept++] = *key;
        }
        track->key_count = kept;
    }
    for (size_t i = 0; i < s->clip_count; ++i) {
        Fl_AudioClip *clip = (Fl_AudioClip *)&s->clips[i];
        if (clip->start.kind == FL_ANCHOR_BEAT && clip->start.beat == beat)
            clip->start = (Fl_TimeAnchor){FL_ANCHOR_ABSOLUTE, 0, false, fl_anchor_resolve(s, &clip->start)};
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

/* Append an empty property track to its entity's component. */
static Fl_Track *add_track(Fl_Scene *s, Fl_Id entity, Fl_AnimProperty property)
{
    Fl_Entity *e = (Fl_Entity *)fl_scene_entity(s, entity);
    Fl_Animation *a = fl_animation_require(e);
    if (!a || a->timeline.track_count == FL_PROP_COUNT) return NULL;
    Fl_Track *t = &a->timeline.tracks[a->timeline.track_count++];
    *t = (Fl_Track){.entity = entity, .property = property};
    return t;
}

/* Ensure a track can hold one more key. */
static bool grow_keys(Fl_Track *t)
{
    if (t->key_count < t->key_capacity) return true;
    if (t->key_capacity > SIZE_MAX / sizeof(Fl_Keyframe) / 2) return false;
    size_t capacity = t->key_capacity ? t->key_capacity * 2 : 4;
    Fl_Keyframe *keys = realloc(t->keys, capacity * sizeof(*keys));
    if (!keys) return false;
    t->keys = keys; t->key_capacity = capacity;
    return true;
}

/* Remove a track from the owning component and release an empty component. */
static void remove_track(Fl_Scene *s, const Fl_Track *track)
{
    Fl_Entity *e = (Fl_Entity *)fl_scene_entity(s, track->entity);
    Fl_Animation *a = (Fl_Animation *)fl_entity_animation(e);
    size_t index = (size_t)(track - a->timeline.tracks);
    free(a->timeline.tracks[index].keys);
    memmove(a->timeline.tracks + index, a->timeline.tracks + index + 1, (a->timeline.track_count - index - 1) * sizeof(*track));
    if (!--a->timeline.track_count&&!a->state_count&&!a->parameter_count&&!a->transition_count) fl_entity_remove_component(e, FL_COMPONENT_ANIMATION);
}

/* Validate ease/anchor/value of a key operation for entity's property. */
static bool key_op_valid(const Fl_Scene *s, const Fl_Op *op)
{
    return fl_scene_entity(s, op->entity) && op->property >= 0 && op->property < FL_PROP_COUNT &&
           op->ease >= 0 && op->ease < FL_EASE_COUNT && anchor_valid(s, &op->anchor) &&
           fl_anchor_resolve(s, &op->anchor) >= 0 && fl_anchor_resolve(s, &op->anchor) <= fl_scene_length(s) &&
           fl_property_value_valid(op->property, op->value);
}

bool fl_timeline_apply(const Fl_Project *p, Fl_Scene *s, const Fl_Op *op, uint64_t *next_id, Fl_Id *created)
{
    if (!s) return false;
    switch (op->kind) {
    case FL_OP_BEAT_ADD: {
        size_t needed = s->beat_count ? 1 : 2;
        if (!op->name || !op->name[0] || !op->beat_duration || op->beat_duration > (uint64_t)TICK_LIMIT ||
            (!s->beat_count && s->duration > (uint64_t)TICK_LIMIT - op->beat_duration) || op->position > s->beat_count ||
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
    case FL_OP_BEAT_SPLIT: {
        size_t i = beat_index(s, op->beat);
        Fl_Tick start, end;
        if (i == SIZE_MAX || !op->name || !op->name[0] || *next_id == UINT64_MAX ||
            !fl_scene_beat_range(s, op->beat, &start, &end) || op->tick <= start || op->tick >= end) return false;
        char *name = copy_text(op->name);
        if (!name || !reserve_beats(s, 1)) {free(name); return false;}
        Fl_Tick left = op->tick - start, right = end - op->tick;
        Fl_Beat *beat = (Fl_Beat *)&s->beats[i];
        Fl_Id id = (*next_id)++;
        bool auto_advance = beat->auto_advance;
        Fl_TrackIterator it = {.scene = s};
        for (const Fl_Track *t; (t = fl_scene_track_next(&it));)
            for (size_t k = 0; k < t->key_count; ++k)
                split_anchor(&t->keys[k].at, op->beat, id, left, right);
        for (size_t c = 0; c < s->clip_count; ++c)
            split_anchor(&((Fl_AudioClip *)s->clips)[c].start, op->beat, id, left, right);
        beat->duration = (uint64_t)left;
        place_beat(s, i + 1, (Fl_Beat){id, name, (uint64_t)right, auto_advance});
        *created = id;
        return true;
    }
    case FL_OP_BEAT_REMOVE: {
        size_t i = beat_index(s, op->beat);
        if (i == SIZE_MAX || s->beat_count < 2) return false;
        remove_beat_content(s, op->beat);
        for (size_t e = 0; e < s->entity_count; ++e) {
            Fl_Animation *a = (Fl_Animation *)fl_entity_animation(&s->entities[e]);
            for (size_t t = a ? a->timeline.track_count : 0; t > 0; --t)
                if (!a->timeline.tracks[t - 1].key_count) remove_track(s, &a->timeline.tracks[t - 1]);
        }
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
        scale_beat_keys(s, b->id, b->duration, op->beat_duration);
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
        Fl_TimeAnchor anchor = op->anchor;
        if (anchor.kind == FL_ANCHOR_ABSOLUTE) anchor = fl_anchor_at(s, anchor.offset);
        else {
            Fl_Tick start, end, time = fl_anchor_resolve(s, &anchor);
            if (!fl_scene_beat_range(s, anchor.beat, &start, &end) || time < start || time > end) return false;
        }
        Fl_Keyframe value = {0, anchor, {op->value[0], op->value[1], op->value[2], op->value[3]}, op->ease};
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
            if (!keys) return false;
            t = add_track(s, op->entity, op->property);
            if (!t) {free(keys); return false;}
            t->id = (*next_id)++; t->keys = keys; t->key_capacity = 4;
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
        if (!--t->key_count) remove_track(s, t);
        return true;
    }
    case FL_OP_TRACK_REMOVE: {
        Fl_TrackIterator it = {.scene = s};
        for (const Fl_Track *t; (t = fl_scene_track_next(&it));)
            if (t->id == op->track) {remove_track(s, t); return true;}
        return false;
    }
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

/* Discard uncommitted tracks after reservation failure. */
static void rollback_tracks(Fl_Scene *s)
{
    for (size_t e = 0; e < s->entity_count; ++e) {
        Fl_Animation *a = (Fl_Animation *)fl_entity_animation(&s->entities[e]);
        for (size_t i = a ? a->timeline.track_count : 0; i > 0; --i) {
            if (!a->timeline.tracks[i - 1].id) {
                bool last = a->timeline.track_count == 1;
                remove_track(s, &a->timeline.tracks[i - 1]);
                if (last) break;
            }
        }
    }
}

bool fl_timeline_reserve(Fl_Scene *s, const Fl_KeyWrite *writes, size_t count, uint64_t next_id)
{
    if (count > (UINT64_MAX - next_id) / 2) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!fl_property_value_valid(writes[i].property, writes[i].value)) { rollback_tracks(s); return false; }
        Fl_Track *t = (Fl_Track *)fl_scene_track(s, writes[i].entity, writes[i].property);
        if (!t) {
            t = add_track(s, writes[i].entity, writes[i].property);
            if (!t) {rollback_tracks(s); return false;}
        }
        if (!grow_keys(t)) { rollback_tracks(s); return false; }
    }
    return true;
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
    Fl_TrackIterator tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&tracks));) {
        const Fl_Track *t = track;
        if (!t->id || t->id >= next_id || !fl_scene_entity(s, t->entity) || t->property < 0 || t->property >= FL_PROP_COUNT || !t->key_count) return false;
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
    size_t n = s->beat_count + fl_scene_track_count(s) + s->clip_count;
    Fl_TrackIterator it = {.scene = s};
    for (const Fl_Track *t; (t = fl_scene_track_next(&it));) n += t->key_count;
    for (size_t e=0;e<s->entity_count;++e) {const Fl_Animation *a=fl_entity_animation(&s->entities[e]);if (a) n+=fl_animation_id_count(a);}
    return n;
}

void fl_timeline_ids(const Fl_Scene *s, Fl_Id *ids)
{
    size_t n = 0;
    for (size_t i = 0; i < s->beat_count; ++i) ids[n++] = s->beats[i].id;
    Fl_TrackIterator tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&tracks));) {
        ids[n++] = track->id;
        for (size_t k = 0; k < track->key_count; ++k) ids[n++] = track->keys[k].id;
    }
    for (size_t i = 0; i < s->clip_count; ++i) ids[n++] = s->clips[i].id;
    for (size_t e=0;e<s->entity_count;++e) {const Fl_Animation *a=fl_entity_animation(&s->entities[e]);if (a) {fl_animation_ids(a,ids+n);n+=fl_animation_id_count(a);}}
}

void fl_timeline_free(Fl_Scene *s)
{
    for (size_t i = 0; i < s->beat_count; ++i) free((void *)s->beats[i].name);
    free((void *)s->beats); free((void *)s->clips);
    s->beats = NULL; s->clips = NULL;
    s->beat_count = s->clip_count = 0;
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

bool fl_timeline_load(Fl_Scene *s, Fl_Binary *b, bool legacy_tracks)
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
    size_t tracks = b->failed || !legacy_tracks ? 0 : get_count(b, sizeof(Fl_Track), 32);
    for (size_t i = 0; !b->failed && i < tracks; ++i) {
        Fl_Id id = fl_binary_get_u64(b), entity = fl_binary_get_u64(b);
        uint64_t property = fl_binary_get_u64(b);
        if (b->failed || property >= FL_PROP_COUNT || fl_scene_track(s, entity, (Fl_AnimProperty)property)) return false;
        Fl_Track *t = add_track(s, entity, (Fl_AnimProperty)property);
        if (!t) return false;
        t->id = id;
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

/* Recover old orphaned key timing without deleting keys or changing nonnegative key times. */
bool fl_timeline_repair(Fl_Scene *s, uint64_t *next_id, bool *changed)
{
    *changed = false;
    size_t count = s->clip_count;
    Fl_TrackIterator count_tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&count_tracks));) {
        if (track->key_count > SIZE_MAX - count) return false;
        count += track->key_count;
    }
    if (count == s->clip_count) return true;
    if (count > SIZE_MAX / sizeof(Fl_Tick)) return false;
    Fl_Tick *times = malloc(count * sizeof(*times));
    if (!times) return false;
    size_t at = 0;
    bool repair = !s->beat_count;
    Fl_Tick length = fl_scene_length(s), maximum = length;
    Fl_TrackIterator it = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&it));) for (size_t k = 0; k < track->key_count; ++k) {
        const Fl_TimeAnchor *anchor = &track->keys[k].at;
        Fl_Tick time = fl_anchor_resolve(s, anchor), start, end;
        times[at++] = time < 0 ? 0 : time;
        if (time > maximum) maximum = time;
        if (anchor->kind != FL_ANCHOR_BEAT || !fl_scene_beat_range(s, anchor->beat, &start, &end) || time < start || time > end)
            repair = true;
    }
    if (!repair) {free(times); return true;}
    if (maximum > TICK_LIMIT || s->revision == UINT64_MAX) {free(times); return false;}
    for (size_t c = 0; c < s->clip_count; ++c) times[at++] = fl_anchor_resolve(s, &s->clips[c].start);
    if (!s->beat_count) {
        char *name = copy_text("Beat 1");
        if (*next_id == UINT64_MAX || !name || !reserve_beats(s, 1)) {free(name); free(times); return false;}
        place_beat(s, 0, (Fl_Beat){(*next_id)++, name, (uint64_t)length, false});
    }
    Fl_Beat *last = (Fl_Beat *)&s->beats[s->beat_count - 1];
    last->duration += (uint64_t)(maximum - length);
    sync_duration(s);
    at = 0;
    it = (Fl_TrackIterator){.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&it));) for (size_t k = 0; k < track->key_count; ++k)
        track->keys[k].at = fl_anchor_at(s, times[at++]);
    for (size_t c = 0; c < s->clip_count; ++c) {
        Fl_AudioClip *clip = (Fl_AudioClip *)&s->clips[c];
        if (clip->start.kind == FL_ANCHOR_BEAT && clip->start.beat == last->id && clip->start.end) {
            clip->start.end = false;
            Fl_Tick start;
            fl_scene_beat_range(s, last->id, &start, NULL);
            clip->start.offset = times[at] - start;
        }
        ++at;
    }
    free(times); ++s->revision; *changed = true;
    return true;
}
