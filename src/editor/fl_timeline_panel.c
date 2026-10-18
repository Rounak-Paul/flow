#include "fl_timeline_panel.h"
#include "components/fl_script.h"
#include "style.h"
#include "core/fl_log.h"
#include <ca_icons.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_ID "timeline"

typedef enum Fl_ItemKind { ITEM_RULER, ITEM_BEAT, ITEM_BEAT_EDGE, ITEM_KEY, ITEM_TRACK, ITEM_CLIP } Fl_ItemKind;
/* Stable callback context for one timeline element of the current build. */
typedef struct Fl_TimelineItem { struct Fl_TimelinePanel *panel; Fl_ItemKind kind; Fl_Id id; } Fl_TimelineItem;
struct Fl_TimelinePanel {
    Fl_SceneEdit *edit;
    Fl_Playback *playback;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Ca_Div *playhead, *header_playhead, *header_content, *ruler, *scroll, *label_content, *lanes;
    Ca_Effect *scroll_effect;
    Ca_Div *beat_drop_marker;
    Fl_Id drag_beat, drag_scene;
    uint64_t drag_revision;
    size_t beat_drop;
    float drag_scroll;
    float label_height, content_width;
    Ca_Label *clock;
    double zoom;                /* pixels per second */
    Fl_TimelineItem *items;
    size_t item_count, item_capacity;
    Fl_Id selected_key, selected_beat, selected_clip;
    Fl_Id selection_entity;
    Fl_AnimProperty property;
    Fl_AnimProperty properties[FL_PROP_COUNT];
    size_t property_count;
    Fl_Tick drag_origin, drag_applied, drag_time;
    bool drag_moved;
    Ca_Window *rename_window;
    Ca_TextInput *rename_input;
    Fl_Id rename_beat;
};

static const char *const PROPERTY_NAMES[FL_PROP_COUNT] = {
    "X", "Y", "Width", "Height", "Rotation", "Scale X", "Scale Y", "Opacity", "Fill", "Stroke", "Stroke width", "Font size", "Crop X", "Crop Y", "Crop width", "Crop height"};

/* Align fixed headers and labels with the two axes of the key viewport. */
static void sync_scroll(void *data)
{
    Fl_TimelinePanel *p = data;
    Ca_Signal *offset = ca_get_scroll_y_signal(p->window, "timeline-scroll");
    float y = offset ? ca_signal_get_float(offset) : 0;
    Ca_Signal *size = ca_div_size_signal(p->scroll);
    if (size) (void)ca_signal_get(size);
    float scale = ca_instance_get_scale(ca_window_instance(p->window));
    ca_div_set_absolute_rect(p->label_content, 0, -y, FL_STYLE_TL_LABEL_W * scale, p->label_height * scale);
    Ca_Signal *horizontal = ca_get_scroll_x_signal(p->window, "timeline-scroll");
    float x = horizontal ? ca_signal_get_float(horizontal) : 0;
    ca_div_set_absolute_rect(p->header_content, -x, 0, p->content_width * scale,
        (FL_STYLE_TL_RULER_H + FL_STYLE_TL_ROW_H) * scale);
}

/* Wheel input over property names scrolls their matching key rows. */
static void labels_scrolled(double dx, double dy, void *data)
{
    (void)dx; Fl_TimelinePanel *p = data;
    ca_scroll_wheel(p->window, "timeline-scroll", dy);
}

/* Pan time with either wheel axis over the fixed ruler and beat strip. */
static void header_scrolled(double dx, double dy, void *data)
{
    Fl_TimelinePanel *p = data;
    ca_scroll_wheel_x(p->window, "timeline-scroll", fabs(dx) > fabs(dy) ? dx : dy);
}

/* Rebuild on the next frame. */
static void invalidate(Fl_TimelinePanel *p)
{
    if (p->dock) ca_dock_invalidate_panel(p->dock, PANEL_ID);
}

/* Borrow the active project, scene and its timeline length. */
static Fl_Project *project_of(const Fl_TimelinePanel *p) { return fl_scene_edit_project(p->edit); }
static const Fl_Scene *scene_of(const Fl_TimelinePanel *p) { return fl_project_scene(project_of(p), fl_scene_edit_scene(p->edit)); }

/* Input coordinates are resolved pixels; timeline zoom is expressed in logical pixels. */
static float panel_scale(const Fl_TimelinePanel *p)
{
    return p->window ? ca_instance_get_scale(ca_window_instance(p->window)) : 1;
}

/* Convert between ticks and lane pixels. */
static float x_of(const Fl_TimelinePanel *p, Fl_Tick tick)
{
    return FL_STYLE_TL_PAD + (float)((double)tick / FL_TICKS_PER_SECOND * p->zoom);
}
static Fl_Tick tick_of(const Fl_TimelinePanel *p, double x)
{
    return (Fl_Tick)llround((x / panel_scale(p) - FL_STYLE_TL_PAD) / p->zoom * FL_TICKS_PER_SECOND);
}

/* Format a scene time as m:ss.cc. */
static void format_time(Fl_Tick tick, char *out, size_t capacity)
{
    if (tick < 0) tick = 0;
    uint64_t centis = (uint64_t)(tick / (FL_TICKS_PER_SECOND / 100));
    snprintf(out, capacity, "%llu:%02llu.%02llu", (unsigned long long)(centis / 6000),
             (unsigned long long)(centis / 100 % 60), (unsigned long long)(centis % 100));
}

/* Apply an operation, logging rejections. */
static bool apply(Fl_TimelinePanel *p, Fl_Op op, Fl_Id *id)
{
    if (fl_project_apply(project_of(p), &op, id)) return true;
    FL_LOG_WARN("timeline", "timeline edit rejected");
    return false;
}

/* Reserve a callback context for this build. */
static Fl_TimelineItem *item(Fl_TimelinePanel *p, Fl_ItemKind kind, Fl_Id id)
{
    if (p->item_count == p->item_capacity) return NULL;
    Fl_TimelineItem *it = &p->items[p->item_count++];
    *it = (Fl_TimelineItem){p, kind, id};
    return it;
}

/* Move the playhead and clock without rebuilding the panel. */
static void sync_playhead(Fl_TimelinePanel *p)
{
    const Fl_Scene *scene = scene_of(p);
    Fl_Tick now = fl_scene_edit_time(p->edit);
    float scale = p->window ? ca_instance_get_scale(ca_window_instance(p->window)) : 1;
    float x = (x_of(p, now) - 1) * scale;
    if (p->playhead) ca_div_set_absolute_rect(p->playhead, x, 0, 2 * scale, p->label_height * scale);
    if (p->header_playhead) ca_div_set_absolute_rect(p->header_playhead, x, 0, 2 * scale,
        (FL_STYLE_TL_RULER_H + FL_STYLE_TL_ROW_H) * scale);
    if (p->clock) {
        char a[32], b[32], text[72];
        format_time(now, a, sizeof(a)); format_time(fl_scene_length(scene), b, sizeof(b));
        snprintf(text, sizeof(text), "%s / %s", a, b);
        ca_set_text(p->clock, text);
    }
}

/* Snap tick to beat edges or the playhead when within FL_STYLE_TL_SNAP pixels. */
static Fl_Tick snap(const Fl_TimelinePanel *p, const Fl_Scene *s, Fl_Tick tick)
{
    Fl_Tick limit = (Fl_Tick)(FL_STYLE_TL_SNAP / (p->zoom * panel_scale(p)) * FL_TICKS_PER_SECOND), best = tick, distance = limit + 1;
    Fl_Tick edge = 0, candidates[2] = {0, fl_scene_edit_time(p->edit)};
    for (int i = 0; i < 2; ++i) if (llabs(candidates[i] - tick) < distance) { distance = llabs(candidates[i] - tick); best = candidates[i]; }
    for (size_t i = 0; i < s->beat_count; ++i) {
        edge += (Fl_Tick)s->beats[i].duration;
        if (llabs(edge - tick) < distance) { distance = llabs(edge - tick); best = edge; }
    }
    return distance <= limit ? best : tick;
}

/* Length a clip occupies on the timeline. */
static Fl_Tick clip_length(const Fl_Project *project, const Fl_AudioClip *c)
{
    if (c->duration) return (Fl_Tick)c->duration;
    const Fl_Asset *a = fl_project_asset(project, c->asset);
    if (a && a->duration > c->source_offset) return (Fl_Tick)(a->duration - c->source_offset);
    return FL_TICKS_PER_SECOND;
}

static void transport_toggle(Ca_Button *b, void *data) { (void)b; Fl_TimelinePanel *p = data; fl_playback_toggle(p->playback); }
static void transport_stop(Ca_Button *b, void *data)
{
    (void)b; Fl_TimelinePanel *p = data;
    fl_playback_pause(p->playback); fl_playback_seek(p->playback, 0);
}
static void zoom_in(Ca_Button *b, void *data) { (void)b; Fl_TimelinePanel *p = data; p->zoom = fmin(FL_STYLE_TL_ZOOM_MAX, p->zoom * 1.5); invalidate(p); }
static void zoom_out(Ca_Button *b, void *data) { (void)b; Fl_TimelinePanel *p = data; p->zoom = fmax(FL_STYLE_TL_ZOOM_MIN, p->zoom / 1.5); invalidate(p); }

/* Add a beat after the one under the playhead. */
static void add_beat(Ca_Button *b, void *data)
{
    (void)b; Fl_TimelinePanel *p = data;
    const Fl_Scene *s = scene_of(p);
    if (!s) return;
    Fl_TimeAnchor at = fl_anchor_at(s, fl_scene_edit_time(p->edit));
    size_t position = s->beat_count;
    for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == at.beat) position = i + 1;
    char name[32]; snprintf(name, sizeof(name), "Beat %zu", (s->beat_count ? s->beat_count : 1) + 1);
    Fl_Id id;
    if (apply(p, (Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = s->id, .name = name, .position = position,
                         .beat_duration = FL_STYLE_TL_BEAT_DEFAULT}, &id)) {
        p->selected_beat = id; p->selected_key = p->selected_clip = 0;
        Fl_Tick start;
        if (fl_scene_beat_range(scene_of(p), id, &start, NULL)) fl_playback_seek(p->playback, start);
        invalidate(p);
    }
}

/* Report whether this entity exposes the property in the authoring UI. */
static bool property_supported(const Fl_Entity *entity, Fl_AnimProperty property)
{
    if (!entity) return false;
    if (property < 0 || property >= FL_PROP_COUNT) return false;
    if (property <= FL_PROP_OPACITY) return fl_entity_component(entity, FL_COMPONENT_TRANSFORM) != NULL;
    const Fl_WidgetDef *def = fl_widget_for_entity(entity);
    if (!def) return false;
    if (property == FL_PROP_FILL) return def->has_fill;
    if (property == FL_PROP_STROKE || property == FL_PROP_STROKE_WIDTH) return def->has_stroke;
    if (property >= FL_PROP_CROP_X) return def->has_image || def->has_video;
    return property == FL_PROP_FONT_SIZE && def->has_text;
}

/* Capture only the requested evaluated channel; same-time keys are updated in place. */
static void key_property(Fl_TimelinePanel *p, Fl_Id entity, Fl_AnimProperty property)
{
    const Fl_Scene *s = scene_of(p);
    const Fl_Entity *e = fl_scene_entity(s, entity);
    if (fl_playback_playing(p->playback) || !property_supported(e, property)) return;
    const Fl_SceneState *state = fl_scene_edit_state(p->edit);
    if (!state) return;
    const Fl_EntityState *v = &state->entities[e - s->entities];
    const double scalar[] = {v->transform.x, v->transform.y, v->transform.width, v->transform.height,
        v->transform.rotation, v->transform.scale_x, v->transform.scale_y, v->opacity};
    Fl_Tick now = fl_scene_edit_time(p->edit);
    Fl_Op op = {.kind = FL_OP_KEY_SET, .scene = s->id, .entity = entity, .property = property,
        .anchor = fl_anchor_at(s, now), .ease = FL_EASE_IN_OUT};
    if (property <= FL_PROP_OPACITY) op.value[0] = scalar[property];
    else if (property == FL_PROP_FILL || property == FL_PROP_STROKE) {
        const unsigned char *color = property == FL_PROP_FILL ? v->appearance.fill : v->appearance.stroke;
        for (size_t i = 0; i < 4; ++i) op.value[i] = color[i];
    } else if (property >= FL_PROP_CROP_X) op.value[0] = v->crop[property - FL_PROP_CROP_X];
    else op.value[0] = property == FL_PROP_STROKE_WIDTH ? v->appearance.stroke_width : v->appearance.font_size;
    const Fl_Track *track = fl_scene_track(s, entity, property);
    Fl_Tick previous = INT64_MIN;
    for (size_t i = 0; track && i < track->key_count; ++i) {
        Fl_Tick tick = fl_anchor_resolve(s, &track->keys[i].at);
        if (tick <= now && tick >= previous) {previous = tick; op.ease = track->keys[i].ease;}
    }
    Fl_Id id;
    if (apply(p, op, &id)) {p->selected_key = id; invalidate(p);}
}

/* Choose one property without creating any tracks. */
static void property_chosen(Ca_Select *select, void *data)
{
    Fl_TimelinePanel *p = data;
    int index = ca_select_get(select);
    if (index >= 0 && (size_t)index < p->property_count) p->property = p->properties[index];
}

/* Key one explicit property of the sole selected object. */
static void key_selection(Ca_Button *button, void *data)
{
    (void)button; Fl_TimelinePanel *p = data;
    if (fl_scene_edit_count(p->edit) == 1) key_property(p, fl_scene_edit_primary(p->edit), p->property);
}

/* Add or update a key on this row at the playhead. */
static void key_track(Ca_Button *button, void *data)
{
    (void)button; Fl_TimelineItem *it = data;
    const Fl_Scene *s = scene_of(it->panel);
    Fl_TrackIterator tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&tracks));) if (track->id == it->id) {
        key_property(it->panel, track->entity, track->property); return;
    }
}

/* Seek to the nearest key strictly before or after the playhead on this track. */
static void seek_track(Fl_TimelineItem *it, bool forward)
{
    Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    Fl_Tick now = fl_scene_edit_time(p->edit), best = forward ? INT64_MAX : INT64_MIN;
    Fl_Id id = 0;
    Fl_TrackIterator tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&tracks));) if (track->id == it->id)
        for (size_t k = 0; k < track->key_count; ++k) {
            const Fl_Keyframe *key = &track->keys[k];
            Fl_Tick tick = fl_anchor_resolve(s, &key->at);
            if (forward ? tick > now && tick < best : tick < now && tick > best) {best = tick; id = key->id;}
        }
    if (id) {p->selected_key = id; fl_playback_seek(p->playback, best); invalidate(p);}
}

/* Previous/next buttons borrow the row's stable track context. */
static void previous_key(Ca_Button *button, void *data) {(void)button; seek_track(data, false);}
static void next_key(Ca_Button *button, void *data) {(void)button; seek_track(data, true);}

/* Delete the selected key; the engine removes an empty track. */
static void delete_key(Ca_Button *button, void *data)
{
    (void)button; Fl_TimelinePanel *p = data;
    const Fl_Scene *s = scene_of(p);
    if (s && p->selected_key && !fl_playback_playing(p->playback))
        apply(p, (Fl_Op){.kind = FL_OP_KEY_REMOVE, .scene = s->id, .key = p->selected_key}, NULL);
}

/* Set easing for the selected key without changing its value or time. */
static void ease_chosen(Ca_Select *select, void *data)
{
    Fl_TimelinePanel *p = data;
    const Fl_Scene *s = scene_of(p);
    const Fl_Track *track;
    const Fl_Keyframe *key = fl_scene_key(s, p->selected_key, &track);
    int index = ca_select_get(select);
    if (!key || index < 0 || index >= FL_EASE_COUNT || fl_playback_playing(p->playback)) return;
    Fl_Op op = {.kind = FL_OP_KEY_SET, .scene = s->id, .entity = track->entity, .property = track->property,
        .key = key->id, .anchor = key->at, .ease = (Fl_Ease)index};
    memcpy(op.value, key->value, sizeof(op.value));
    apply(p, op, NULL);
}

/* Ruler: click or drag to scrub. */
static void ruler_drag(const Ca_DragEvent *event, void *data)
{
    Fl_TimelineItem *it = data;
    fl_playback_seek(it->panel->playback, tick_of(it->panel, event->local_x));
}

/* Read the native horizontal offset so panning during a beat drag remains coherent. */
static float horizontal_offset(Fl_TimelinePanel *p)
{
    Ca_Signal *offset = p->window ? ca_get_scroll_x_signal(p->window, "timeline-scroll") : NULL;
    return offset ? ca_signal_get_float(offset) : 0;
}

/* Preview an insertion slot without changing document order or callback ownership mid-drag. */
static void preview_beat_drop(Fl_TimelinePanel *p, Fl_Tick center)
{
    const Fl_Scene *s = scene_of(p);
    if (!s || s->id != p->drag_scene || s->revision != p->drag_revision) return;
    size_t from = SIZE_MAX, position = 0;
    Fl_Tick at = 0;
    for (size_t i = 0; i < s->beat_count; ++i) {
        Fl_Tick duration = (Fl_Tick)s->beats[i].duration;
        if (s->beats[i].id == p->drag_beat) from = i;
        else if (center > at + duration / 2) ++position;
        at += duration;
    }
    if (from == SIZE_MAX) return;
    p->beat_drop = position;
    Fl_Tick boundary = 0;
    size_t before = position > from ? position + 1 : position;
    for (size_t i = 0; i < before; ++i) boundary += (Fl_Tick)s->beats[i].duration;
    if (p->beat_drop_marker) {
        float scale = panel_scale(p);
        ca_div_set_absolute_rect(p->beat_drop_marker, (x_of(p, boundary) - 1) * scale,
            FL_STYLE_TL_RULER_H * scale, 2 * scale, FL_STYLE_TL_ROW_H * scale);
        ca_set_hidden(p->beat_drop_marker, position == from);
    }
}

/* Capture a beat reorder, key move, beat resize or clip move. */
static void item_drag_start(const Ca_DragEvent *event, void *data)
{
    (void)event; Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    if (!s) return;
    p->drag_beat = 0;
    p->drag_origin = 0; p->drag_moved = false; p->drag_time = fl_scene_edit_time(p->edit);
    if (it->kind == ITEM_BEAT) {
        Fl_Tick start, end;
        if (fl_scene_beat_range(s, it->id, &start, &end)) {
            fl_playback_pause(p->playback);
            p->drag_beat = it->id; p->drag_scene = s->id; p->drag_revision = s->revision;
            p->drag_origin = start + (end - start) / 2; p->drag_scroll = horizontal_offset(p);
            p->selected_beat = it->id; p->selected_key = p->selected_clip = 0;
            for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == it->id) p->beat_drop = i;
            invalidate(p);
        }
    } else if (it->kind == ITEM_KEY) {
        const Fl_Keyframe *k = fl_scene_key(s, it->id, NULL);
        if (k) {p->drag_origin = fl_anchor_resolve(s, &k->at); p->selected_key = k->id;}
    } else if (it->kind == ITEM_BEAT_EDGE) {
        for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == it->id) {p->drag_origin = (Fl_Tick)s->beats[i].duration; p->selected_beat = it->id;}
    } else if (it->kind == ITEM_CLIP) {
        for (size_t i = 0; i < s->clip_count; ++i) if (s->clips[i].id == it->id) p->drag_origin = fl_anchor_resolve(s, &s->clips[i].start);
    }
    p->drag_applied = p->drag_origin;
}

/* Resize snapping uses original boundaries, excluding the edge being dragged. */
static Fl_Tick snap_resize(const Fl_TimelinePanel *p, const Fl_Scene *s, Fl_Id beat, Fl_Tick tick)
{
    Fl_Tick limit = (Fl_Tick)(FL_STYLE_TL_SNAP / (p->zoom * panel_scale(p)) * FL_TICKS_PER_SECOND);
    Fl_Tick best = tick, distance = limit + 1, edge = 0, shift = 0;
    Fl_Tick candidates[] = {0, p->drag_time};
    for (size_t i = 0; i < 2; ++i) if (llabs(candidates[i] - tick) < distance) {
        distance = llabs(candidates[i] - tick); best = candidates[i];
    }
    for (size_t i = 0; i < s->beat_count; ++i) {
        edge += (Fl_Tick)s->beats[i].duration;
        if (s->beats[i].id == beat) {shift = p->drag_origin - (Fl_Tick)s->beats[i].duration; continue;}
        Fl_Tick candidate = edge + shift;
        if (llabs(candidate - tick) < distance) {distance = llabs(candidate - tick); best = candidate;}
    }
    return distance <= limit ? best : tick;
}

/* Preview beat insertion, move keys/clips with snapping, or resize beat edges. */
static void item_drag(const Ca_DragEvent *event, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    if (!s) return;
    float dx = (event->dx + (it->kind == ITEM_BEAT ? horizontal_offset(p) - p->drag_scroll : 0)) / panel_scale(p);
    bool at_origin = fabsf(dx) < FL_STYLE_WIDGET_DRAG_THRESHOLD;
    if (at_origin && !p->drag_moved) return;
    if (!at_origin) p->drag_moved = true;
    Fl_Tick target = p->drag_origin + (at_origin ? 0 : (Fl_Tick)llround(dx / p->zoom * FL_TICKS_PER_SECOND));
    if (it->kind == ITEM_BEAT) {
        p->drag_applied = target;
        if (p->drag_beat) preview_beat_drop(p, target);
        return;
    }
    if (it->kind == ITEM_BEAT_EDGE) {
        Fl_Tick start;
        if (!fl_scene_beat_range(s, it->id, &start, NULL)) return;
        Fl_Tick end = at_origin ? start + target : snap_resize(p, s, it->id, start + target);
        target = end - start < FL_STYLE_TL_BEAT_MIN ? FL_STYLE_TL_BEAT_MIN : end - start;
    } else if (!at_origin) target = snap(p, s, target < 0 ? 0 : target);
    if (it->kind == ITEM_KEY && target > fl_scene_length(s)) target = fl_scene_length(s);
    if (target == p->drag_applied) return;
    if (it->kind == ITEM_KEY) {
        const Fl_Track *t;
        const Fl_Keyframe *k = fl_scene_key(s, it->id, &t);
        if (!k) return;
        Fl_Op op = {.kind = FL_OP_KEY_SET, .scene = s->id, .entity = t->entity, .property = t->property, .key = k->id,
                    .anchor = fl_anchor_at(s, target), .ease = k->ease};
        memcpy(op.value, k->value, sizeof(op.value));
        if (apply(p, op, NULL)) p->drag_applied = target;
    } else if (it->kind == ITEM_BEAT_EDGE) {
        for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == it->id &&
            apply(p, (Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = s->id, .beat = it->id, .name = s->beats[i].name,
                             .beat_duration = (uint64_t)target, .auto_advance = s->beats[i].auto_advance}, NULL)) { p->drag_applied = target; break; }
    } else if (it->kind == ITEM_CLIP) {
        for (size_t i = 0; i < s->clip_count; ++i) if (s->clips[i].id == it->id) {
            Fl_AudioClip clip = s->clips[i];
            clip.start = fl_anchor_at(s, target);
            if (apply(p, (Fl_Op){.kind = FL_OP_CLIP_UPDATE, .scene = s->id, .clip = it->id, .clip_record = &clip}, NULL)) p->drag_applied = target;
            break;
        }
    }
}

/* Commit a beat drop or final drag delta; a click selects and seeks. */
static void item_drag_end(const Ca_DragEvent *event, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    if (!s) return;
    if (it->kind == ITEM_BEAT || it->kind == ITEM_KEY || it->kind == ITEM_BEAT_EDGE || it->kind == ITEM_CLIP) item_drag(event, data);
    if (it->kind == ITEM_BEAT && p->drag_beat) {
        Fl_Id beat = p->drag_beat; p->drag_beat = 0;
        if (p->beat_drop_marker) ca_set_hidden(p->beat_drop_marker, true);
        if (p->drag_moved) {
            if (s->id != p->drag_scene || s->revision != p->drag_revision) return;
            size_t from = SIZE_MAX;
            for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == beat) from = i;
            if (from != SIZE_MAX && from != p->beat_drop && apply(p, (Fl_Op){.kind = FL_OP_BEAT_MOVE,
                .scene = s->id, .beat = beat, .position = p->beat_drop}, NULL)) {
                Fl_Tick start;
                if (fl_scene_beat_range(scene_of(p), beat, &start, NULL)) fl_playback_seek(p->playback, start);
            }
            invalidate(p); return;
        }
    }
    if (p->drag_moved || hypotf(event->dx, event->dy) / panel_scale(p) >= FL_STYLE_WIDGET_DRAG_THRESHOLD) return;
    Fl_Tick start;
    p->selected_key = p->selected_beat = p->selected_clip = 0;
    if (it->kind == ITEM_KEY) {
        const Fl_Keyframe *k = fl_scene_key(s, it->id, NULL);
        p->selected_key = it->id;
        if (k) fl_playback_seek(p->playback, fl_anchor_resolve(s, &k->at));
    } else if ((it->kind == ITEM_BEAT || it->kind == ITEM_BEAT_EDGE) && fl_scene_beat_range(s, it->id, &start, NULL)) {
        p->selected_beat = it->id;
        if (it->kind == ITEM_BEAT) fl_playback_seek(p->playback, start);
    } else if (it->kind == ITEM_CLIP) p->selected_clip = it->id;
    invalidate(p);
}

/* Release rename handles when the utility window closes. */
static void rename_closed(Ca_Window *window, void *data) { (void)window; Fl_TimelinePanel *p = data; p->rename_window = NULL; p->rename_input = NULL; }

/* Commit a beat rename captured when the window opened. */
static void rename_apply(Ca_Button *b, void *data)
{
    (void)b; Fl_TimelinePanel *p = data;
    const Fl_Scene *s = scene_of(p);
    for (size_t i = 0; s && i < s->beat_count; ++i) if (s->beats[i].id == p->rename_beat) {
        if (apply(p, (Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = s->id, .beat = p->rename_beat, .name = ca_input_text(p->rename_input),
                             .beat_duration = s->beats[i].duration, .auto_advance = s->beats[i].auto_advance}, NULL)) ca_window_close(p->rename_window);
        return;
    }
}

/* Open the beat rename form. */
static void rename_beat(Fl_TimelinePanel *p, const Fl_Beat *beat)
{
    if (!p->window) return;
    if (p->rename_window) ca_window_destroy(p->rename_window);
    p->rename_beat = beat->id;
    p->rename_window = ca_window_create(ca_window_instance(p->window), &(Ca_WindowDesc){.title = "Rename beat",
        .width = FL_STYLE_FORM_W, .height = FL_STYLE_RENAME_H, .on_close = rename_closed, .on_close_data = p});
    if (!p->rename_window) return;
    ca_ui_begin(p->rename_window, &(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_FORM});
    p->rename_input = ca_input(&(Ca_InputDesc){.text = beat->name, .style = FL_CLASS_INPUT});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Rename", .on_click = rename_apply, .click_data = p, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_ui_end(); ca_input_focus(p->rename_input);
}

/* Beat actions: rename, split at the playhead, delete. */
static void beat_menu(int index, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    Fl_Tick start, end, now = fl_scene_edit_time(p->edit);
    size_t at = SIZE_MAX;
    for (size_t i = 0; s && i < s->beat_count; ++i) if (s->beats[i].id == it->id) at = i;
    if (at == SIZE_MAX || !fl_scene_beat_range(s, it->id, &start, &end)) return;
    if (index == 0) rename_beat(p, &s->beats[at]);
    else if (index == 1 && now > start && now < end) {
        char name[160]; snprintf(name, sizeof(name), "%s b", s->beats[at].name);
        Fl_Id id;
        if (apply(p, (Fl_Op){.kind = FL_OP_BEAT_SPLIT, .scene = s->id, .beat = it->id,
            .name = name, .tick = now}, &id)) {p->selected_beat = id; invalidate(p);}
    } else if (index == 2) apply(p, (Fl_Op){.kind = FL_OP_BEAT_REMOVE, .scene = s->id, .beat = it->id}, NULL);
}

/* Key actions: easing choices, delete. */
static void key_menu(int index, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    const Fl_Track *t;
    const Fl_Keyframe *k = fl_scene_key(s, it->id, &t);
    if (!k) return;
    if (index == FL_EASE_COUNT) { apply(p, (Fl_Op){.kind = FL_OP_KEY_REMOVE, .scene = s->id, .key = it->id}, NULL); return; }
    Fl_Op op = {.kind = FL_OP_KEY_SET, .scene = s->id, .entity = t->entity, .property = t->property, .key = k->id,
                .anchor = k->at, .ease = (Fl_Ease)index};
    memcpy(op.value, k->value, sizeof(op.value));
    apply(p, op, NULL);
}

/* Track label action: stop animating the property. */
static void track_menu(int index, void *data)
{
    Fl_TimelineItem *it = data;
    const Fl_Scene *s = scene_of(it->panel);
    if (index == 0 && s) apply(it->panel, (Fl_Op){.kind = FL_OP_TRACK_REMOVE, .scene = s->id, .track = it->id}, NULL);
}

/* Clip actions: gain steps, fades, delete. */
static void clip_menu(int index, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    for (size_t i = 0; s && i < s->clip_count; ++i) if (s->clips[i].id == it->id) {
        if (index == 4) { apply(p, (Fl_Op){.kind = FL_OP_CLIP_REMOVE, .scene = s->id, .clip = it->id}, NULL); return; }
        Fl_AudioClip clip = s->clips[i];
        if (index == 0) clip.gain = fmin(4, fmax(0.01, clip.gain) * 1.4125375446);
        else if (index == 1) clip.gain = clip.gain / 1.4125375446;
        else if (index == 2) clip.gain = 1;
        else if (index == 3) clip.fade_in = clip.fade_out = clip.fade_in || clip.fade_out ? 0 : FL_STYLE_TL_FADE;
        apply(p, (Fl_Op){.kind = FL_OP_CLIP_UPDATE, .scene = s->id, .clip = it->id, .clip_record = &clip}, NULL);
        return;
    }
}

/* Rebuild on document and selection changes; follow time with the playhead only. */
static bool changed(const Fl_Event *event, void *data)
{
    (void)event; Fl_TimelinePanel *p = data;
    Fl_Id primary = fl_scene_edit_primary(p->edit);
    if (primary != p->selection_entity) {
        p->selection_entity = primary; p->property = FL_PROP_X;
        const Fl_Scene *scene = scene_of(p);
        Fl_TrackIterator tracks = {.scene = scene};
        for (const Fl_Track *track; (track = fl_scene_track_next(&tracks));) if (track->entity == primary) {
            p->property = track->property; break;
        }
    }
    const Fl_Track *track;
    if (!fl_scene_key(scene_of(p), p->selected_key, &track) || !fl_scene_edit_contains(p->edit, track->entity))
        p->selected_key = 0;
    invalidate(p); return false;
}
static bool time_changed(const Fl_Event *event, void *data) { (void)event; sync_playhead(data); return false; }

Fl_TimelinePanel *fl_timeline_panel_create(Fl_SceneEdit *edit, Fl_Playback *playback, Fl_EventBus *bus)
{
    if (!edit || !playback || !bus) return NULL;
    Fl_TimelinePanel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = edit; p->playback = playback; p->bus = bus; p->zoom = FL_STYLE_TL_ZOOM;
    const char *rebuild[] = {FL_EVENT_PROJECT_CHANGED, FL_EVENT_SELECTION_CHANGED, FL_EVENT_PLAYBACK_CHANGED};
    for (size_t i = 0; i < 3; ++i)
        if (!fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, rebuild[i]), .fn = changed, .user_data = p, .owner = p}).generation) {
            fl_timeline_panel_destroy(p); return NULL;
        }
    if (!fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_TIME_CHANGED), .fn = time_changed, .user_data = p, .owner = p}).generation) {
        fl_timeline_panel_destroy(p); return NULL;
    }
    return p;
}

void fl_timeline_panel_teardown(void *data)
{
    Fl_TimelinePanel *p = data;
    ca_effect_destroy(p->scroll_effect); p->scroll_effect = NULL; p->label_content = NULL; p->header_content = NULL; p->header_playhead = NULL; p->beat_drop_marker = NULL;
    p->dock = NULL; p->window = NULL; p->playhead = NULL; p->ruler = NULL; p->scroll = NULL; p->lanes = NULL; p->clock = NULL;
}

void fl_timeline_panel_destroy(Fl_TimelinePanel *p)
{
    if (!p) return;
    ca_effect_destroy(p->scroll_effect);
    if (p->rename_window) ca_window_destroy(p->rename_window);
    fl_event_unsubscribe_owner(p->bus, p);
    free(p->items); free(p);
}

bool fl_timeline_panel_add_clip(Fl_TimelinePanel *p, Fl_Id asset, Fl_Tick tick)
{
    const Fl_Scene *s = p ? scene_of(p) : NULL;
    const Fl_Asset *a = s ? fl_project_asset(project_of(p), asset) : NULL;
    if (!a || a->kind != FL_ASSET_AUDIO) return false;
    Fl_AudioClip clip = {.asset = asset, .start = fl_anchor_at(s, tick < 0 ? 0 : tick), .gain = 1};
    Fl_Id id;
    if (!apply(p, (Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = s->id, .clip_record = &clip}, &id)) return false;
    p->selected_clip = id;
    return true;
}

bool fl_timeline_panel_drop_asset(Fl_TimelinePanel *p, Fl_Id asset, double screen_x, double screen_y)
{
    if (!p || !p->window || !p->ruler || !p->scroll || !p->lanes) return false;
    int wx, wy, width, height;
    ca_window_get_rect(p->window, &wx, &wy, &width, &height);
    float bx, by, w, h;
    ca_div_screen_rect(ca_window_content_div(p->window), &bx, &by, &w, &h);
    if (width <= 0) return false;
    double scale = w / width, x = (screen_x - wx) * scale, y = (screen_y - wy) * scale;
    float sx, sy, sw, sh;
    ca_div_content_screen_rect(p->scroll, &sx, &sy, &sw, &sh);
    if (x < sx || x > sx + sw || y < sy || y > sy + sh) return false;
    float lx, ly, lw, lh;
    ca_div_content_screen_rect(p->lanes, &lx, &ly, &lw, &lh);
    if (x < lx || x > lx + lw || y < ly || y > ly + lh) return false;
    float rx, rw;
    ca_div_screen_rect(p->ruler, &rx, NULL, &rw, NULL);
    if (x < rx || x > rx + rw) return false;
    return fl_timeline_panel_add_clip(p, asset, snap(p, scene_of(p), tick_of(p, x - rx)));
}

/* Emit an absolutely positioned, draggable element inside the current lane. */
static void element(Fl_TimelinePanel *p, Fl_TimelineItem *it, const char *id, const char *style, float x, float width, float height)
{
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .id = id, .style = style, .position = CA_POSITION_ABSOLUTE,
        .pos_x = x, .pos_y = (FL_STYLE_TL_ROW_H - height) * 0.5f, .width = width, .height = height,
        .on_drag_start = it ? item_drag_start : NULL, .on_drag = it ? item_drag : NULL, .on_drag_end = it ? item_drag_end : NULL, .drag_data = it,
        .drag_preview = it && (it->kind == ITEM_BEAT || it->kind == ITEM_KEY || it->kind == ITEM_CLIP)});
    (void)p;
}

/* Emit a label-column row. */
static void label_row(const char *text, const char *style)
{
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = style});
    ca_text(&(Ca_TextDesc){.text = text, .style = FL_CLASS_TL_LABEL_TEXT});
    ca_div_end();
}

void fl_timeline_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_TimelinePanel *p = data; p->dock = dock; p->window = window;
    ca_effect_destroy(p->scroll_effect); p->scroll_effect = NULL; p->label_content = NULL; p->header_content = NULL; p->header_playhead = NULL; p->beat_drop_marker = NULL;
    const Fl_Scene *s = scene_of(p);
    Fl_Project *project = project_of(p);
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TIMELINE});
    if (!s) {
        ca_text(&(Ca_TextDesc){.text = "No scene", .style = FL_CLASS_MUTED});
        ca_div_end(); p->playhead = NULL; p->ruler = NULL; p->scroll = NULL; p->lanes = NULL; p->clock = NULL; return;
    }
    size_t keys = 0, tracks = 0;
    Fl_TrackIterator scan = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&scan));) if (fl_scene_edit_contains(p->edit, track->entity)) {
        keys += track->key_count; ++tracks;
    }
    size_t needed = 1 + s->beat_count * 2 + tracks + keys + s->clip_count;
    if (needed > p->item_capacity) {
        Fl_TimelineItem *grown = realloc(p->items, needed * sizeof(*grown));
        if (grown) { p->items = grown; p->item_capacity = needed; }
    }
    p->item_count = 0;
    bool playing = fl_playback_playing(p->playback);
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TOOLBAR});
    ca_btn_begin(&(Ca_BtnDesc){.text = playing ? CA_ICON_NF_FA_PAUSE : CA_ICON_NF_FA_PLAY, .on_click = transport_toggle, .click_data = p,
        .style = FL_CLASS_ICON_ACTION}); ca_btn_end();
    ca_tooltip(&(Ca_TooltipDesc){.text = playing ? "Pause" : "Play"});
    ca_btn_begin(&(Ca_BtnDesc){.text = CA_ICON_NF_FA_STOP, .on_click = transport_stop, .click_data = p, .style = FL_CLASS_ICON_ACTION}); ca_btn_end();
    ca_tooltip(&(Ca_TooltipDesc){.text = "Stop"});
    p->clock = ca_text(&(Ca_TextDesc){.text = "", .style = FL_CLASS_TL_CLOCK});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_SPACER}); ca_div_end();
    ca_btn_begin(&(Ca_BtnDesc){.text = CA_ICON_NF_FA_PLUS, .on_click = add_beat, .click_data = p, .style = FL_CLASS_ICON_ACTION}); ca_btn_end();
    ca_tooltip(&(Ca_TooltipDesc){.text = "Add beat"});
    ca_btn_begin(&(Ca_BtnDesc){.text = CA_ICON_NF_FA_MAGNIFYING_GLASS_MINUS, .on_click = zoom_out, .click_data = p, .style = FL_CLASS_ICON_ACTION}); ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){.text = CA_ICON_NF_FA_MAGNIFYING_GLASS_PLUS, .on_click = zoom_in, .click_data = p, .style = FL_CLASS_ICON_ACTION}); ca_btn_end();
    ca_div_end();

    size_t selected = fl_scene_edit_count(p->edit);
    const Fl_Entity *entity = selected == 1 ? fl_scene_entity(s, fl_scene_edit_primary(p->edit)) : NULL;
    char heading[192];
    if (entity) snprintf(heading, sizeof(heading), "%s", entity->name);
    else if (selected) snprintf(heading, sizeof(heading), "%zu objects selected", selected);
    else heading[0] = 0;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_SELECTION});
    ca_text(&(Ca_TextDesc){.text = heading, .style = FL_CLASS_TL_LABEL_TEXT});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_SPACER}); ca_div_end();
    const char *options[FL_PROP_COUNT];
    p->property_count = 0;
    int property_index = 0;
    for (int prop = 0; prop < FL_PROP_COUNT; ++prop) if (property_supported(entity, (Fl_AnimProperty)prop)) {
        if (p->property == (Fl_AnimProperty)prop) property_index = (int)p->property_count;
        options[p->property_count] = PROPERTY_NAMES[prop];
        p->properties[p->property_count++] = (Fl_AnimProperty)prop;
    }
    if (p->property_count) p->property = p->properties[property_index];
    ca_select(&(Ca_SelectDesc){.options = options, .option_count = (int)p->property_count, .selected = property_index,
        .id = "timeline-property", .style = FL_CLASS_TL_SELECT, .on_change = property_chosen, .change_data = p,
        .disabled = !entity || playing});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Add keyframe", .id = "timeline-add-key", .on_click = key_selection, .click_data = p,
        .style = FL_CLASS_ACTION, .disabled = !p->property_count || playing}); ca_btn_end();
    ca_tooltip(&(Ca_TooltipDesc){.text = "Key this property at the playhead. An existing key at this time is updated."});
    ca_div_end();
    const Fl_Track *selected_track;
    const Fl_Keyframe *selected_key = fl_scene_key(s, p->selected_key, &selected_track);
    if (selected_key && fl_scene_edit_contains(p->edit, selected_track->entity)) {
        char time[32], detail[128];
        format_time(fl_anchor_resolve(s, &selected_key->at), time, sizeof(time));
        snprintf(detail, sizeof(detail), "%s key at %s", PROPERTY_NAMES[selected_track->property], time);
        static const char *eases[] = {"Linear", "Ease in", "Ease out", "Ease in-out", "Hold"};
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_SELECTION});
        ca_text(&(Ca_TextDesc){.text = detail, .style = FL_CLASS_TL_LABEL_TEXT});
        ca_select(&(Ca_SelectDesc){.options = eases, .option_count = FL_EASE_COUNT, .selected = selected_key->ease,
            .id = "timeline-ease", .style = FL_CLASS_TL_SELECT, .on_change = ease_chosen, .change_data = p, .disabled = playing});
        ca_btn_begin(&(Ca_BtnDesc){.text = "Delete key", .id = "timeline-delete-key", .style = FL_CLASS_ACTION,
            .on_click = delete_key, .click_data = p, .disabled = playing}); ca_btn_end();
        ca_div_end();
    }

    Fl_Tick length = fl_scene_length(s);
    float content = x_of(p, length) + FL_STYLE_TL_PAD;
    p->content_width = content;
    p->label_height = (float)(tracks + s->clip_count) * FL_STYLE_TL_ROW_H;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_BODY});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_LABELS});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_RULER_LABEL}); ca_div_end();
    label_row("Beats", FL_CLASS_TL_LABEL);
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_LABEL_VIEWPORT,
        .on_scroll = labels_scrolled, .scroll_data = p});
    p->label_content = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_LABEL_CONTENT,
        .position = CA_POSITION_ABSOLUTE, .width = FL_STYLE_TL_LABEL_W, .height = p->label_height});
    Fl_TrackIterator label_tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&label_tracks));) {
        const Fl_Track *t = track;
        if (!fl_scene_edit_contains(p->edit, t->entity)) continue;
        const Fl_Entity *e = fl_scene_entity(s, t->entity);
        char text[192]; snprintf(text, sizeof(text), selected > 1 ? "%s / %s" : "%s%s",
            selected > 1 && e ? e->name : "", PROPERTY_NAMES[t->property]);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_LABEL});
        ca_text(&(Ca_TextDesc){.text = text, .style = FL_CLASS_TL_TRACK_NAME});
        Fl_TimelineItem *it = item(p, ITEM_TRACK, t->id);
        ca_btn_begin(&(Ca_BtnDesc){.text = "<", .style = FL_CLASS_TL_TRACK_ACTION, .on_click = previous_key, .click_data = it, .disabled = !it || playing}); ca_btn_end();
        ca_tooltip(&(Ca_TooltipDesc){.text = "Previous keyframe"});
        ca_btn_begin(&(Ca_BtnDesc){.text = CA_ICON_NF_FA_DIAMOND, .style = FL_CLASS_TL_TRACK_ACTION, .on_click = key_track, .click_data = it, .disabled = !it || playing}); ca_btn_end();
        ca_tooltip(&(Ca_TooltipDesc){.text = "Add or update this property at the playhead"});
        ca_btn_begin(&(Ca_BtnDesc){.text = ">", .style = FL_CLASS_TL_TRACK_ACTION, .on_click = next_key, .click_data = it, .disabled = !it || playing}); ca_btn_end();
        ca_tooltip(&(Ca_TooltipDesc){.text = "Next keyframe"});
        ca_div_end();
        static const char *track_items[] = {"Remove animation"};
        if (it) ca_context_menu(&(Ca_CtxMenuDesc){.items = track_items, .item_count = 1, .on_select = track_menu, .select_data = it});
    }
    for (size_t i = 0; i < s->clip_count; ++i) {
        const Fl_Asset *a = fl_project_asset(project, s->clips[i].asset);
        char text[192]; snprintf(text, sizeof(text), "%s  %s", CA_ICON_NF_FA_MUSIC, a ? a->name : "?");
        label_row(text, FL_CLASS_TL_LABEL);
    }
    ca_div_end();
    ca_div_end();
    ca_div_end();

    p->lanes = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "timeline-lanes", .style = FL_CLASS_TL_LANES});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_HEADER,
        .on_scroll = header_scrolled, .scroll_data = p});
    p->header_content = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_CONTENT,
        .position = CA_POSITION_ABSOLUTE, .width = content, .height = FL_STYLE_TL_RULER_H + FL_STYLE_TL_ROW_H});
    Fl_TimelineItem *ruler = item(p, ITEM_RULER, 0);
    p->ruler = ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .id = "timeline-ruler", .style = FL_CLASS_TL_RULER, .width = content,
        .on_drag_start = ruler ? ruler_drag : NULL, .on_drag = ruler ? ruler_drag : NULL, .drag_data = ruler});
    static const double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60, 300};
    double step = steps[9];
    for (size_t i = 0; i < 10; ++i) if (steps[i] * p->zoom >= FL_STYLE_TL_LABEL_SPACING) { step = steps[i]; break; }
    for (double t = 0; t * FL_TICKS_PER_SECOND <= (double)length + 1; t += step) {
        char label[32]; format_time((Fl_Tick)llround(t * FL_TICKS_PER_SECOND), label, sizeof(label));
        char id[32]; snprintf(id, sizeof(id), "tick-%.2f", t);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .id = id, .style = FL_CLASS_TL_TICK, .position = CA_POSITION_ABSOLUTE,
            .pos_x = x_of(p, (Fl_Tick)llround(t * FL_TICKS_PER_SECOND)), .pos_y = 0});
        ca_text(&(Ca_TextDesc){.text = label, .style = FL_CLASS_TL_TICK_TEXT});
        ca_div_end();
    }
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_ROW, .width = content});
    Fl_Tick at = 0;
    static const char *beat_items[] = {"Rename", "Split at playhead", "Delete beat and its keys"};
    for (size_t i = 0; i < s->beat_count; ++i) {
        const Fl_Beat *b = &s->beats[i];
        float x = x_of(p, at), w = x_of(p, at + (Fl_Tick)b->duration) - x;
        char id[48]; snprintf(id, sizeof(id), "beat-%llu", (unsigned long long)b->id);
        Fl_TimelineItem *it = item(p, ITEM_BEAT, b->id);
        element(p, it, id, p->selected_beat == b->id ? FL_CLASS_TL_BEAT_SELECTED : FL_CLASS_TL_BEAT, x, w, FL_STYLE_TL_ROW_H - 4);
        ca_text(&(Ca_TextDesc){.text = b->name, .style = FL_CLASS_TL_BEAT_TEXT});
        ca_div_end();
        if (it) ca_context_menu(&(Ca_CtxMenuDesc){.items = beat_items, .item_count = 3, .on_select = beat_menu, .select_data = it});
        at += (Fl_Tick)b->duration;
    }
    at = 0;
    for (size_t i = 0; i < s->beat_count; ++i) {
        const Fl_Beat *b = &s->beats[i];
        at += (Fl_Tick)b->duration;
        char id[48]; snprintf(id, sizeof(id), "beat-edge-%llu", (unsigned long long)b->id);
        element(p, item(p, ITEM_BEAT_EDGE, b->id), id, FL_CLASS_TL_EDGE,
            x_of(p, at) - FL_STYLE_TL_EDGE_W * 0.5f, FL_STYLE_TL_EDGE_W, FL_STYLE_TL_ROW_H);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_EDGE_MARK, .no_hover = true});
        ca_div_end(); ca_div_end();
    }
    ca_div_end();

    p->beat_drop_marker = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_BEAT_DROP,
        .position = CA_POSITION_ABSOLUTE, .width = 2, .height = FL_STYLE_TL_ROW_H, .hidden = true, .no_hover = true});
    ca_div_end();
    p->header_playhead = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_PLAYHEAD,
        .position = CA_POSITION_ABSOLUTE, .width = 2, .height = FL_STYLE_TL_RULER_H + FL_STYLE_TL_ROW_H, .no_hover = true});
    ca_div_end(); ca_div_end(); ca_div_end();
    p->scroll = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "timeline-scroll", .style = FL_CLASS_TL_SCROLL});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_KEY_CONTENT, .width = content, .height = p->label_height});
    static const char *key_items[] = {"Linear", "Ease in", "Ease out", "Ease in-out", "Hold", "Delete"};
    Fl_TrackIterator lane_tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&lane_tracks));) {
        const Fl_Track *t = track;
        if (!fl_scene_edit_contains(p->edit, t->entity)) continue;
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_ROW, .width = content});
        Fl_Tick first = INT64_MAX, last = INT64_MIN;
        for (size_t k = 0; k < t->key_count; ++k) {
            Fl_Tick when = fl_anchor_resolve(s, &t->keys[k].at);
            if (when < first) first = when;
            if (when > last) last = when;
        }
        if (t->key_count > 1) {
            element(p, NULL, NULL, FL_CLASS_TL_SPAN, x_of(p, first), x_of(p, last) - x_of(p, first), 2);
            ca_div_end();
        }
        for (size_t k = 0; k < t->key_count; ++k) {
            const Fl_Keyframe *key = &t->keys[k];
            char id[48]; snprintf(id, sizeof(id), "key-%llu", (unsigned long long)key->id);
            Fl_TimelineItem *it = item(p, ITEM_KEY, key->id);
            element(p, it, id, p->selected_key == key->id ? FL_CLASS_TL_KEY_SELECTED : FL_CLASS_TL_KEY,
                    x_of(p, fl_anchor_resolve(s, &key->at)) - FL_STYLE_TL_KEY * 0.5f, FL_STYLE_TL_KEY, FL_STYLE_TL_KEY);
            ca_text(&(Ca_TextDesc){.text = CA_ICON_NF_FA_DIAMOND,
                .style = p->selected_key == key->id ? FL_CLASS_TL_KEY_TEXT_SELECTED : FL_CLASS_TL_KEY_TEXT});
            ca_div_end();
            if (it) ca_context_menu(&(Ca_CtxMenuDesc){.items = key_items, .item_count = 6, .on_select = key_menu, .select_data = it});
        }
        ca_div_end();
    }

    static const char *clip_items[] = {"Gain +3 dB", "Gain -3 dB", "Reset gain", "Toggle fades", "Delete"};
    for (size_t i = 0; i < s->clip_count; ++i) {
        const Fl_AudioClip *c = &s->clips[i];
        const Fl_Asset *a = fl_project_asset(project, c->asset);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_ROW, .width = content});
        Fl_Tick start = fl_anchor_resolve(s, &c->start);
        char id[48], text[192];
        snprintf(id, sizeof(id), "clip-%llu", (unsigned long long)c->id);
        if (c->gain < 0.001) snprintf(text, sizeof(text), "%s  muted", a ? a->name : "?");
        else if (fabs(c->gain - 1) > 0.01) snprintf(text, sizeof(text), "%s  %+.1f dB", a ? a->name : "?", 20 * log10(c->gain));
        else snprintf(text, sizeof(text), "%s", a ? a->name : "?");
        Fl_TimelineItem *it = item(p, ITEM_CLIP, c->id);
        element(p, it, id, p->selected_clip == c->id ? FL_CLASS_TL_CLIP_SELECTED : FL_CLASS_TL_CLIP, x_of(p, start),
                fmaxf(4, x_of(p, start + clip_length(project, c)) - x_of(p, start)), FL_STYLE_TL_ROW_H - 4);
        ca_text(&(Ca_TextDesc){.text = text, .style = FL_CLASS_TL_BEAT_TEXT});
        ca_div_end();
        if (it) ca_context_menu(&(Ca_CtxMenuDesc){.items = clip_items, .item_count = 5, .on_select = clip_menu, .select_data = it});
        ca_div_end();
    }
    p->playhead = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "timeline-playhead", .style = FL_CLASS_TL_PLAYHEAD,
        .position = CA_POSITION_ABSOLUTE, .pos_x = x_of(p, fl_scene_edit_time(p->edit)) - 1, .pos_y = 0, .width = 2,
        .height = p->label_height, .no_hover = true});
    ca_div_end();
    ca_div_end();
    ca_div_end();
    ca_div_end();
    ca_div_end();
    ca_div_end();
    if (p->drag_beat && p->drag_moved) preview_beat_drop(p, p->drag_applied);
    p->scroll_effect = ca_effect(ca_window_instance(window), sync_scroll, p);
    sync_playhead(p);
}
