#include "schema/fl_properties.h"
#include "fl_timeline_panel.h"
#include "ui/fl_button.h"
#include "components/fl_script.h"
#include "style.h"
#include "core/fl_log.h"
#include "core/fl_clock.h"
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
    Ca_Div *playhead, *playhead_viewport, *header_playhead, *header_content, *ruler, *scroll, *label_content, *lanes;
    Ca_Effect *scroll_effect;
    Ca_Div *beat_drop_marker;
    bool snap_enabled;
    bool key_handler;
    Fl_Id drag_beat, drag_scene;
    uint64_t drag_revision;
    size_t beat_drop;
    float drag_scroll;
    float label_height, content_width;
    Ca_Label *clock;
    Ca_Button *split_button;
    double zoom;                /* pixels per second */
    Fl_TimelineItem *items;
    size_t item_count, item_capacity;
    Fl_Id selected_key, selected_beat, selected_clip;
    Fl_Id selection_entity, timeline_scene;
    uint64_t timeline_revision;
    Fl_AnimProperty property;
    Fl_AnimProperty properties[FL_PROP_COUNT];
    size_t property_count;
    Fl_Tick drag_origin, drag_applied, drag_time;
    bool drag_moved;
    Ca_Window *rename_window;
    Ca_TextInput *rename_input;
    Fl_Id rename_beat;
    Fl_Id click_beat, click_scene;
    Ca_Window *click_window;
    uint64_t click_ns;
    float click_x, click_y;
};

static void sync_playhead(Fl_TimelinePanel *p);

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
    ca_div_set_absolute_rect(p->header_content, -x, 0, p->content_width * scale, FL_STYLE_TL_HEADER_H * scale);
    sync_playhead(p);
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
static const Fl_Scene *scene_of(const Fl_TimelinePanel *p) { return fl_scene_edit_timeline(p->edit); }

/* Timeline lanes use the focused scope clock; transport still advances the root scene. */
static void seek_local(Fl_TimelinePanel *p, Fl_Tick tick)
{
    Fl_Tick root;
    if (fl_scene_edit_root_time(p->edit,tick,&root)) fl_playback_seek(p->playback,root);
}

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
    if (fl_scene_edit_timeline_apply(p->edit, &op, id)) return true;
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

/* Keep cursor inside a viewport margin; inputs and result are resolved pixels. */
static float playback_scroll(float cursor, float width, float offset, float content, float scale)
{
    if (width <= 0) return offset;
    float margin = fminf(48 * scale, width * 0.2f);
    if (cursor < offset + margin) offset = cursor - margin;
    else if (cursor > offset + width - margin) offset = cursor - width + margin;
    return fminf(fmaxf(0, offset), fmaxf(0, content - width));
}

/* A beat can split only strictly inside its time range. */
static bool can_split_beat(const Fl_TimelinePanel *p, Fl_Id beat)
{
    Fl_Tick start, end, now = fl_scene_edit_local_time(p->edit);
    return fl_scene_beat_range(scene_of(p), beat, &start, &end) && now > start && now < end;
}

/* Move the two scroll-locked playhead segments and clock without rebuilding. */
static void sync_playhead(Fl_TimelinePanel *p)
{
    const Fl_Scene *scene = scene_of(p);
    Fl_Tick now = fl_scene_edit_local_time(p->edit);
    float scale = p->window ? ca_instance_get_scale(ca_window_instance(p->window)) : 1;
    float x = (x_of(p, now) - FL_STYLE_TL_PLAYHEAD_W * 0.5f) * scale;
    float w = FL_STYLE_TL_PLAYHEAD_W * scale;
    if (p->header_playhead) ca_div_set_absolute_rect(p->header_playhead,
        (x_of(p, now) - FL_STYLE_TL_PLAYHEAD_W * 0.5f) * scale, 0,
        FL_STYLE_TL_PLAYHEAD_W * scale, FL_STYLE_TL_HEADER_H * scale);
    if (p->playhead && p->playhead_viewport && p->scroll && p->lanes) {
        float sx = 0, sy = 0, width = 0, height = 0, lx = 0, ly = 0;
        ca_div_content_screen_rect(p->scroll, &sx, &sy, &width, &height);
        ca_div_screen_rect(p->lanes, &lx, &ly, NULL, NULL);
        Ca_Signal *horizontal = ca_get_scroll_x_signal(p->window, "timeline-scroll");
        const float *offset = ca_signal_peek(horizontal);
        ca_div_set_absolute_rect(p->playhead_viewport, sx - lx, sy - ly, width, height);
        ca_div_set_absolute_rect(p->playhead, x - (offset ? *offset : 0), 0, w, height);
    }
    if (p->window && p->scroll && p->header_content && fl_playback_playing(p->playback)) {
        float width = 0;
        ca_div_content_screen_rect(p->scroll, NULL, NULL, &width, NULL);
        Ca_Signal *horizontal = ca_get_scroll_x_signal(p->window, "timeline-scroll");
        if (horizontal && width > 0) {
            const float *value = ca_signal_peek(horizontal);
            float offset = value ? *value : 0;
            float next = playback_scroll(x_of(p, now) * scale, width, offset, p->content_width * scale, scale);
            if (next != offset) ca_set_scroll_x(p->window, "timeline-scroll", next);
        }
    }
    if (p->split_button) ca_set_disabled(p->split_button,
        fl_playback_playing(p->playback) || !can_split_beat(p, p->selected_beat));
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
    if (!p->snap_enabled) return tick;
    Fl_Tick limit = (Fl_Tick)(FL_STYLE_TL_SNAP / (p->zoom * panel_scale(p)) * FL_TICKS_PER_SECOND), best = tick, distance = limit + 1;
    Fl_Tick edge = 0, candidates[2] = {0, fl_scene_edit_local_time(p->edit)};
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
    fl_playback_pause(p->playback); seek_local(p, 0);
}
static void zoom_in(Ca_Button *b, void *data) { (void)b; Fl_TimelinePanel *p = data; p->zoom = fmin(FL_STYLE_TL_ZOOM_MAX, p->zoom * 1.5); invalidate(p); }
static void zoom_out(Ca_Button *b, void *data) { (void)b; Fl_TimelinePanel *p = data; p->zoom = fmax(FL_STYLE_TL_ZOOM_MIN, p->zoom / 1.5); invalidate(p); }
static void zoom_reset(Fl_TimelinePanel *p) { p->zoom = FL_STYLE_TL_ZOOM; invalidate(p); }
/* Fit the scene duration to the visible time viewport. */
static void zoom_fit(Ca_Button *button, void *data)
{
    (void)button;
    Fl_TimelinePanel *p = data;
    const Fl_Scene *scene = scene_of(p);
    if (!scene || !p->scroll) return;
    float width = 0;
    ca_div_content_screen_rect(p->scroll, NULL, NULL, &width, NULL);
    double seconds = (double)fl_scene_length(scene) / FL_TICKS_PER_SECOND;
    double available = width / panel_scale(p) - 2 * FL_STYLE_TL_PAD;
    if (seconds <= 0 || available <= 0) return;
    p->zoom = fmax(FL_STYLE_TL_ZOOM_MIN, fmin(FL_STYLE_TL_ZOOM_MAX, available / seconds));
    invalidate(p);
}

static void toggle_snap(Ca_Button *b, void *data)
{
    (void)b; Fl_TimelinePanel *p = data;
    p->snap_enabled = !p->snap_enabled; invalidate(p);
}

/* Global shortcuts while the editor window owns the keyboard: space plays or pauses, +/- zoom, 0 resets. */
static void on_key(const Ca_Event *event, void *data)
{
    Fl_TimelinePanel *p = data;
    if (!p->window || event->window != p->window) return;
    if (event->key.action != CA_PRESS) return;
    bool pointer = false, keyboard = false;
    ca_window_input_capture(event->window, &pointer, &keyboard);
    if (keyboard) return;
    switch (event->key.key) {
        case CA_KEY_SPACE: fl_playback_toggle(p->playback); return;
        case CA_KEY_EQUAL:
        case CA_KEY_KP_ADD: zoom_in(NULL, p); return;
        case CA_KEY_MINUS:
        case CA_KEY_KP_SUBTRACT: zoom_out(NULL, p); return;
        case CA_KEY_0: zoom_reset(p); return;
        default: return;
    }
}

/* Add a beat after the one under the playhead. */
static void add_beat(Ca_Button *b, void *data)
{
    (void)b; Fl_TimelinePanel *p = data;
    const Fl_Scene *s = scene_of(p);
    if (!s) return;
    Fl_TimeAnchor at = fl_anchor_at(s, fl_scene_edit_local_time(p->edit));
    size_t position = s->beat_count;
    for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == at.beat) position = i + 1;
    char name[32]; snprintf(name, sizeof(name), "Beat %zu", (s->beat_count ? s->beat_count : 1) + 1);
    Fl_Id id;
    if (apply(p, (Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = s->id, .name = name, .position = position,
                         .beat_duration = FL_STYLE_TL_BEAT_DEFAULT}, &id)) {
        p->selected_beat = id; p->selected_key = p->selected_clip = 0;
        Fl_Tick start;
        if (fl_scene_beat_range(scene_of(p), id, &start, NULL)) seek_local(p, start);
        invalidate(p);
    }
}

/* Behavior-owned and baked properties cannot accept ineffective native keys. */
static bool key_editable(Fl_TimelinePanel *p, Fl_Id id, Fl_AnimProperty property)
{
    const Fl_SceneState *state=fl_scene_edit_state(p->edit); const Fl_Scene *flat=fl_scene_state_scene(state,scene_of(p));
    const Fl_Entity *entity=fl_scene_entity(flat,id);
    return entity && fl_property_supported(entity,property) && !fl_entity_component(entity,FL_COMPONENT_SAMPLED) && !fl_scene_state_writer(state,(size_t)(entity-flat->entities),property,NULL,NULL);
}

static void key_property(Fl_TimelinePanel *p, Fl_Id entity, Fl_AnimProperty property)
{
    const Fl_Scene *s = scene_of(p);
    const Fl_Entity *e = fl_scene_entity(s, entity);
    if (fl_playback_playing(p->playback) || !fl_property_supported(e, property)) return;
    const Fl_SceneState *state = fl_scene_edit_state(p->edit);
    if (!state) return;
    const Fl_Scene *flat=fl_scene_state_scene(state,s); const Fl_Entity *displayed=fl_scene_entity(flat,e->id);
    if (!displayed || fl_entity_component(displayed,FL_COMPONENT_SAMPLED) || fl_scene_state_writer(state,(size_t)(displayed-flat->entities),property,NULL,NULL)) return;
    const Fl_EntityState *v = &state->entities[displayed-flat->entities];
    Fl_Tick now = fl_scene_edit_local_time(p->edit);
    Fl_Op op = {.kind = FL_OP_KEY_SET, .scene = s->id, .entity = entity, .property = property,
        .anchor = fl_anchor_at(s, now), .ease = FL_EASE_IN_OUT};
    double values[FL_PROP_COUNT][4]={{0}}; fl_entity_state_values(v,values);
    memcpy(op.value,values[property],sizeof(op.value));
    const Fl_Track *track = fl_scene_track(s, entity, property);
    Fl_Tick previous = INT64_MIN;
    for (size_t i = 0; track && i < track->key_count; ++i) {
        Fl_Tick tick;
        if (!fl_anchor_sample(s, &track->keys[i].at, &tick)) continue;
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
    if (fl_scene_edit_count(p->edit) == 1) key_property(p, fl_scene_edit_focus(p->edit), p->property);
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
    Fl_Tick now = fl_scene_edit_local_time(p->edit), best = forward ? INT64_MAX : INT64_MIN;
    Fl_Id id = 0;
    Fl_TrackIterator tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&tracks));) if (track->id == it->id)
        for (size_t k = 0; k < track->key_count; ++k) {
            const Fl_Keyframe *key = &track->keys[k];
            Fl_Tick tick;
            if (!fl_anchor_sample(s, &key->at, &tick)) continue;
            if (forward ? tick > now && tick < best : tick < now && tick > best) {best = tick; id = key->id;}
        }
    if (id) {p->selected_key = id; seek_local(p, best); invalidate(p);}
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

/* Scrub by dragging the playhead handle. The handle's own origin moves while it seeks, so the
 * lane coordinate is read back from the lane content's window rect instead. */
static void handle_drag(const Ca_DragEvent *event, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    float origin = 0;
    if (p->header_content) ca_div_screen_rect(p->header_content, &origin, NULL, NULL, NULL);
    seek_local(p, tick_of(p, (float)event->x - origin));
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
        ca_div_set_absolute_rect(p->beat_drop_marker, (x_of(p, boundary) - 1) * scale, 0,
            2 * scale, FL_STYLE_TL_LANE_H * scale);
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
    p->drag_origin = 0; p->drag_moved = false; p->drag_time = fl_scene_edit_local_time(p->edit);
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
static void rename_beat(Fl_TimelinePanel *p, const Fl_Beat *beat);

/* Recognize consecutive nearby releases on one beat; now is monotonic nanoseconds. */
static bool beat_double_clicked(Fl_TimelinePanel *p, Fl_Id beat, const Ca_DragEvent *event, uint64_t now)
{
    Fl_Id scene = fl_scene_edit_scene(p->edit);
    bool twice = p->click_beat != 0 && p->click_beat == beat && p->click_scene == scene && p->click_window == event->window &&
        now >= p->click_ns && now - p->click_ns <= 400000000ull &&
        hypotf(event->x - p->click_x, event->y - p->click_y) <= 4 * panel_scale(p);
    p->click_beat = twice ? 0 : beat;
    p->click_scene = scene; p->click_window = event->window; p->click_ns = now;
    p->click_x = event->x; p->click_y = event->y;
    return twice;
}

static void item_drag_end(const Ca_DragEvent *event, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    if (it->kind != ITEM_BEAT) p->click_beat = 0;
    if (!s) return;
    if (it->kind == ITEM_BEAT || it->kind == ITEM_KEY || it->kind == ITEM_BEAT_EDGE || it->kind == ITEM_CLIP) item_drag(event, data);
    if (p->drag_moved || hypotf(event->dx, event->dy) / panel_scale(p) >= FL_STYLE_WIDGET_DRAG_THRESHOLD)
        p->click_beat = 0;
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
                if (fl_scene_beat_range(scene_of(p), beat, &start, NULL)) seek_local(p, start);
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
        if (k) {Fl_Tick time; if (fl_anchor_sample(s, &k->at, &time)) seek_local(p, time);}
    } else if ((it->kind == ITEM_BEAT || it->kind == ITEM_BEAT_EDGE) && fl_scene_beat_range(s, it->id, &start, NULL)) {
        p->selected_beat = it->id;
        if (it->kind == ITEM_BEAT) {
            seek_local(p, start);
            if (beat_double_clicked(p, it->id, event, fl_clock_monotonic_ns())) {
                for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == it->id) {
                    rename_beat(p, &s->beats[i]); break;
                }
            }
        }
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
    fl_button(&(Fl_ButtonDesc){.text = "Rename", .on_click = rename_apply, .click_data = p});
    ca_ui_end(); ca_input_focus(p->rename_input);
}

/* Remove beat and its keys, preserving the scene's required final beat. */
static void remove_beat(Fl_TimelinePanel *p, Fl_Id beat)
{
    const Fl_Scene *scene = scene_of(p);
    if (!scene || scene->beat_count < 2 || !fl_scene_beat_range(scene, beat, NULL, NULL)) return;
    fl_playback_pause(p->playback);
    apply(p, (Fl_Op){.kind = FL_OP_BEAT_REMOVE, .scene = scene->id, .beat = beat}, NULL);
}

/* Delete the selected beat through the same path as its context menu. */
static void delete_beat(Ca_Button *button, void *data)
{
    (void)button;
    Fl_TimelinePanel *p = data;
    remove_beat(p, p->selected_beat);
}

/* Split beat at the playhead through the same operation for menu and toolbar. */
static void split_beat(Fl_TimelinePanel *p, Fl_Id beat)
{
    if (!can_split_beat(p, beat)) return;
    const Fl_Scene *scene = scene_of(p);
    for (size_t i = 0; i < scene->beat_count; ++i) if (scene->beats[i].id == beat) {
        char name[160]; snprintf(name, sizeof(name), "%s b", scene->beats[i].name);
        fl_playback_pause(p->playback);
        Fl_Id id;
        if (apply(p, (Fl_Op){.kind = FL_OP_BEAT_SPLIT, .scene = scene->id, .beat = beat,
            .name = name, .tick = fl_scene_edit_local_time(p->edit)}, &id)) {
            p->selected_beat = id; invalidate(p);
        }
        return;
    }
}

/* Split the selected beat when editing is available. */
static void split_selected_beat(Ca_Button *button, void *data)
{
    (void)button;
    Fl_TimelinePanel *p = data;
    if (!fl_playback_playing(p->playback)) split_beat(p, p->selected_beat);
}

/* Beat actions: rename, split at the playhead, delete. */
static void beat_menu(int index, void *data)
{
    Fl_TimelineItem *it = data; Fl_TimelinePanel *p = it->panel;
    const Fl_Scene *s = scene_of(p);
    size_t at = SIZE_MAX;
    for (size_t i = 0; s && i < s->beat_count; ++i) if (s->beats[i].id == it->id) at = i;
    if (at == SIZE_MAX) return;
    if (index == 0) rename_beat(p, &s->beats[at]);
    else if (index == 1) split_beat(p, it->id);
    else if (index == 2) remove_beat(p, it->id);
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
    Fl_Id primary = fl_scene_edit_focus(p->edit);
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
    if (!fl_scene_beat_range(scene_of(p), p->selected_beat, NULL, NULL)) p->selected_beat = 0;
    invalidate(p); return false;
}
static bool time_changed(const Fl_Event *event, void *data)
{
    (void)event; Fl_TimelinePanel *p=data; const Fl_Scene *scene=scene_of(p);
    if (scene && (scene->id!=p->timeline_scene || scene->revision!=p->timeline_revision)) invalidate(p);
    else sync_playhead(p);
    return false;
}

Fl_TimelinePanel *fl_timeline_panel_create(Fl_SceneEdit *edit, Fl_Playback *playback, Fl_EventBus *bus)
{
    if (!edit || !playback || !bus) return NULL;
    Fl_TimelinePanel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = edit; p->playback = playback; p->bus = bus; p->zoom = FL_STYLE_TL_ZOOM; p->snap_enabled = true;
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
    ca_effect_destroy(p->scroll_effect); p->scroll_effect = NULL; p->label_content = NULL; p->header_content = NULL; p->beat_drop_marker = NULL; p->header_playhead = NULL; p->playhead_viewport = NULL; p->split_button = NULL;
    p->dock = NULL; p->window = NULL; p->playhead = NULL; p->ruler = NULL; p->scroll = NULL; p->lanes = NULL; p->clock = NULL;
}

void fl_timeline_panel_destroy(Fl_TimelinePanel *p)
{
    if (!p) return;
    ca_effect_destroy(p->scroll_effect);
    if (p->key_handler && p->window) {
        ca_event_set_handler(ca_window_instance(p->window), CA_EVENT_KEY, NULL, NULL);
        p->key_handler = false;
    }
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

/* Emit an absolutely positioned, draggable element inside a lane of the given height. */
static void element(Fl_TimelinePanel *p, Fl_TimelineItem *it, const char *id, const char *style, float x, float width, float height, float lane_h)
{
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .id = id, .style = style, .position = CA_POSITION_ABSOLUTE,
        .pos_x = x, .pos_y = (lane_h - height) * 0.5f, .width = width, .height = height,
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

/* Draw bounded clock-style subdivisions with labeled major ticks at each zoom level. */
static void build_time_ruler(Fl_TimelinePanel *p, Fl_Tick length, float width)
{
    double seconds = (double)length / FL_TICKS_PER_SECOND;
    double target = fmax(72.0 / p->zoom, seconds / 200.0);
    double unit = pow(10, floor(log10(target)));
    double step = unit * (target <= unit ? 1 : target <= 2 * unit ? 2 : target <= 5 * unit ? 5 : 10);
    Fl_TimelineItem *handle = item(p, ITEM_RULER, 0);
    p->header_playhead = ca_div_begin(&(Ca_DivDesc){.position = CA_POSITION_ABSOLUTE,
        .id = "timeline-needle", .style = FL_CLASS_TL_NEEDLE,
        .pos_x = x_of(p, fl_scene_edit_local_time(p->edit)) - FL_STYLE_TL_PLAYHEAD_W * 0.5f,
        .width = FL_STYLE_TL_PLAYHEAD_W, .height = FL_STYLE_TL_HEADER_H,
        .on_drag_start = handle ? handle_drag : NULL, .on_drag = handle ? handle_drag : NULL, .drag_data = handle});
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = "fl-tl-time-ruler", .width = width,
        .on_drag_start = handle ? handle_drag : NULL, .on_drag = handle ? handle_drag : NULL, .drag_data = handle});
    size_t count = (size_t)fmin(2000, floor(seconds / (step / 10)));
    for (size_t i = 0; i <= count; ++i) {
        bool major = i % 10 == 0;
        float height = major ? 9 : i % 5 == 0 ? 6 : 3;
        double time = (double)i * step / 10;
        float x = FL_STYLE_TL_PAD + (float)(time * p->zoom);
        ca_div_begin(&(Ca_DivDesc){.position = CA_POSITION_ABSOLUTE, .pos_x = x,
            .pos_y = 0, .width = 1, .height = height,
            .style = major ? "fl-tl-tick major" : "fl-tl-tick", .no_hover = true});
        ca_div_end();
        if (major) {
            char text[40];
            snprintf(text, sizeof(text), step < 1 ? "%.2fs" : "%.0fs", time);
            ca_div_begin(&(Ca_DivDesc){.position = CA_POSITION_ABSOLUTE, .pos_x = x + 4,
                .width = 68, .height = FL_STYLE_TL_RULER_H - 6, .no_hover = true});
            ca_text(&(Ca_TextDesc){.text = text, .style = "fl-tl-tick-text"});
            ca_div_end();
        }
    }
    ca_div_end();
}

/* Build transport, contextual editing and synchronized property/time lanes for window. */
void fl_timeline_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_TimelinePanel *p = data; p->dock = dock; p->window = window;
    ca_effect_destroy(p->scroll_effect); p->scroll_effect = NULL; p->label_content = NULL; p->header_content = NULL; p->beat_drop_marker = NULL; p->header_playhead = NULL; p->playhead_viewport = NULL; p->split_button = NULL;
    const Fl_Scene *s = scene_of(p);
    p->timeline_scene=s ? s->id : 0; p->timeline_revision=s ? s->revision : 0;
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
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = "fl-tl-transport"});
    fl_button(&(Fl_ButtonDesc){.text = playing ? CA_ICON_NF_MD_PAUSE : CA_ICON_NF_MD_PLAY, .on_click = transport_toggle, .click_data = p,
        .kind = FL_BUTTON_ICON,
        .tooltip = playing ? "Pause (Space)" : "Play (Space)"});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_STOP, .on_click = transport_stop, .click_data = p, .kind = FL_BUTTON_ICON, .tooltip = "Stop"});
    p->clock = ca_text(&(Ca_TextDesc){.text = "", .style = FL_CLASS_TL_CLOCK});
    /* Key the selected object's property at the playhead, including its first one. */
    size_t selected = fl_scene_edit_count(p->edit);
    const Fl_Entity *entity = selected == 1 ? fl_scene_entity(s, fl_scene_edit_focus(p->edit)) : NULL;
    char selection[192];
    if (entity) snprintf(selection, sizeof(selection), "%s", entity->name);
    else if (selected) snprintf(selection, sizeof(selection), "%zu objects", selected);
    else snprintf(selection, sizeof(selection), "No object selected");
    if (entity) {
        const char *options[FL_PROP_COUNT];
        p->property_count = 0;
        int property_index = 0;
        for (int prop = 0; prop < FL_PROP_COUNT; ++prop) if (fl_property_supported(entity, (Fl_AnimProperty)prop)) {
            if (p->property == (Fl_AnimProperty)prop) property_index = (int)p->property_count;
            options[p->property_count] = fl_property_definition(prop)->label;
            p->properties[p->property_count++] = (Fl_AnimProperty)prop;
        }
        if (p->property_count) p->property = p->properties[property_index];
        ca_select(&(Ca_SelectDesc){.options = options, .option_count = (int)p->property_count, .selected = property_index,
            .id = "timeline-property", .style = FL_CLASS_TL_SELECT, .on_change = property_chosen, .change_data = p,
            .disabled = !entity || playing});
        fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_FA_DIAMOND, .id = "timeline-add-key", .on_click = key_selection, .click_data = p,
             .disabled = !p->property_count || playing || !key_editable(p,fl_scene_edit_focus(p->edit),p->property), .kind = FL_BUTTON_ICON, .tooltip = "Key this property at the playhead. An existing key at this time is updated."});
    }

    ca_div_begin(&(Ca_DivDesc){.style = FL_CLASS_TL_SPACER}); ca_div_end();
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_MAGNET, .on_click = toggle_snap, .click_data = p,
        .kind = FL_BUTTON_ICON, .active = p->snap_enabled,
        .tooltip = p->snap_enabled ? "Snapping on: beat edges and playhead" : "Snapping off"});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_MAGNIFY_MINUS, .on_click = zoom_out, .click_data = p, .kind = FL_BUTTON_ICON, .tooltip = "Zoom out (-)"});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_MAGNIFY_PLUS, .on_click = zoom_in, .click_data = p, .kind = FL_BUTTON_ICON, .tooltip = "Zoom in (+)"});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_FIT_TO_PAGE_OUTLINE, .on_click = zoom_fit, .click_data = p,
        .kind = FL_BUTTON_ICON,
        .tooltip = "Fit the scene in the visible timeline"});
    fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_PLUS, .on_click = add_beat, .click_data = p, .kind = FL_BUTTON_ICON, .tooltip = "Add beat at the playhead"});

    p->split_button = fl_button(&(Fl_ButtonDesc){.kind = FL_BUTTON_ICON, .text = CA_ICON_NF_MD_CONTENT_CUT,
        .id = "timeline-split-beat", .on_click = split_selected_beat, .click_data = p,
        .disabled = playing || !can_split_beat(p, p->selected_beat),
        .tooltip = "Split selected beat at the playhead"});
    fl_button(&(Fl_ButtonDesc){.kind = FL_BUTTON_ICON, .text = CA_ICON_NF_MD_DELETE_OUTLINE,
        .id = "timeline-delete-beat", .on_click = delete_beat, .click_data = p,
        .destructive = true, .disabled = playing || !p->selected_beat || s->beat_count < 2,
        .tooltip = !p->selected_beat ? "Select a beat to delete" : s->beat_count < 2 ? "The scene must keep one beat" : "Delete selected beat and its keyframes"});
    ca_div_end();

    /* Show contextual key editing only while a visible key is selected. */
    const Fl_Track *selected_track;
    const Fl_Keyframe *selected_key = fl_scene_key(s, p->selected_key, &selected_track);
    if (selected_key && fl_scene_edit_contains(p->edit, selected_track->entity)) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = "fl-tl-authoring"});
        char time[32], detail[160];
        Fl_Tick selected_time;
        if (fl_anchor_sample(s, &selected_key->at, &selected_time)) format_time(selected_time, time, sizeof(time));
        else snprintf(time, sizeof(time), "Unbound");
        snprintf(detail, sizeof(detail), "%s at %s", fl_property_definition(selected_track->property)->label, time);
        ca_text(&(Ca_TextDesc){.text = detail, .style = FL_CLASS_TL_KEY_DETAIL});
        static const char *eases[] = {"Linear", "Ease in", "Ease out", "Ease in-out", "Hold"};
        ca_select(&(Ca_SelectDesc){.options = eases, .option_count = FL_EASE_COUNT, .selected = selected_key->ease,
            .id = "timeline-ease", .style = FL_CLASS_TL_SELECT, .on_change = ease_chosen, .change_data = p, .disabled = playing});
        fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_DELETE_OUTLINE, .id = "timeline-delete-key", .destructive = true,
            .on_click = delete_key, .click_data = p, .disabled = playing, .kind = FL_BUTTON_ICON, .tooltip = "Delete keyframe"});
        ca_div_end();
    }

    Fl_Tick length = fl_scene_length(s);
    float content = x_of(p, length) + FL_STYLE_TL_PAD;
    p->content_width = content;
    p->label_height = (float)(tracks + s->clip_count) * FL_STYLE_TL_ROW_H;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_BODY});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_LABELS});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = "fl-tl-label-header"});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .height = FL_STYLE_TL_RULER_H, .style = "fl-tl-object-header"});
    ca_text(&(Ca_TextDesc){.text = selection, .style = "fl-tl-selection"});
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_LANE_LABEL});
    ca_text(&(Ca_TextDesc){.text = "Beats", .style = FL_CLASS_TL_LANE_LABEL_TEXT});
    ca_div_end();
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_LABEL_VIEWPORT,
        .on_scroll = labels_scrolled, .scroll_data = p});
    p->label_content = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_LABEL_CONTENT,
        .position = CA_POSITION_ABSOLUTE, .width = FL_STYLE_TL_LABEL_W, .height = p->label_height});
    Fl_TrackIterator label_tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&label_tracks));) {
        const Fl_Track *t = track;
        if (!fl_scene_edit_contains(p->edit, t->entity)) continue;
        const Fl_Entity *e = fl_scene_entity(s, t->entity);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = selected_key && selected_track == t ? FL_CLASS_TL_LABEL_SELECTED : FL_CLASS_TL_LABEL,
            .height = FL_STYLE_TL_ROW_H});
        char label[192]; snprintf(label, sizeof(label), "%s%s%s",
            selected > 1 && e ? e->name : "",
            selected > 1 && e ? " · " : "",
            fl_property_definition(t->property)->label);
        ca_text(&(Ca_TextDesc){.text = label, .style = FL_CLASS_TL_TRACK_NAME});
        Fl_TimelineItem *it = item(p, ITEM_TRACK, t->id);
        fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_SKIP_PREVIOUS, .on_click = previous_key, .click_data = it, .disabled = !it || playing,
            .kind = FL_BUTTON_ICON,
            .tooltip = "Previous keyframe"});
        fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_FA_DIAMOND, .on_click = key_track, .click_data = it, .disabled = !it || playing || !key_editable(p,t->entity,t->property),
            .kind = FL_BUTTON_ICON,
            .tooltip = "Add or update this property at the playhead"});
        fl_button(&(Fl_ButtonDesc){.text = CA_ICON_NF_MD_SKIP_NEXT, .on_click = next_key, .click_data = it, .disabled = !it || playing,
            .kind = FL_BUTTON_ICON,
            .tooltip = "Next keyframe"});
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
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = "fl-tl-header",
        .on_scroll = header_scrolled, .scroll_data = p});
    p->header_content = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_CONTENT,
        .position = CA_POSITION_ABSOLUTE, .width = content, .height = FL_STYLE_TL_HEADER_H});
    build_time_ruler(p, length, content);
    p->ruler = ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .id = "timeline-ruler", .style = FL_CLASS_TL_BEATLABE_SEGMENTS, .width = content});
    if (!s->beat_count) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_BEAT_EMPTY, .width = content});
        ca_text(&(Ca_TextDesc){.text = "No beats yet — press + to add the first one", .style = FL_CLASS_MUTED});
        ca_div_end();
    }
    Fl_Tick at = 0;
    static const char *beat_items[] = {"Rename", "Split at playhead", "Delete beat and its keys"};
    for (size_t i = 0; i < s->beat_count; ++i) {
        const Fl_Beat *b = &s->beats[i];
        float x = x_of(p, at), w = x_of(p, at + (Fl_Tick)b->duration) - x;
        char id[48]; snprintf(id, sizeof(id), "beat-%llu", (unsigned long long)b->id);
        Fl_TimelineItem *it = item(p, ITEM_BEAT, b->id);
        element(p, it, id, p->selected_beat == b->id ? FL_CLASS_TL_BEAT_SELECTED : FL_CLASS_TL_BEAT,
            x + 2, fmaxf(0, w - 4), FL_STYLE_TL_LANE_H - 4, FL_STYLE_TL_LANE_H);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .position = CA_POSITION_ABSOLUTE, .style = FL_CLASS_TL_BEAT_LABEL, .no_hover = true});
        char title[160]; snprintf(title, sizeof(title), "%zu · %s", i + 1, b->name);
        ca_text(&(Ca_TextDesc){.text = title, .style = FL_CLASS_TL_BEAT_TEXT});
        double secs = (double)b->duration / (double)FL_TICKS_PER_SECOND;
        char duration[32]; snprintf(duration, sizeof(duration), secs >= 9.95 ? "%.0fs" : "%.1fs", secs);
        ca_text(&(Ca_TextDesc){.text = duration, .style = FL_CLASS_TL_BEAT_DURATION});
        ca_div_end();
        ca_div_end();
        if (it) ca_context_menu(&(Ca_CtxMenuDesc){.items = beat_items, .item_count = s->beat_count > 1 ? 3 : 2, .on_select = beat_menu, .select_data = it});
        at += (Fl_Tick)b->duration;
    }
    at = 0;
    for (size_t i = 0; i < s->beat_count; ++i) {
        const Fl_Beat *b = &s->beats[i];
        at += (Fl_Tick)b->duration;
        char id[48]; snprintf(id, sizeof(id), "beat-edge-%llu", (unsigned long long)b->id);
        Fl_TimelineItem *edge = item(p, ITEM_BEAT_EDGE, b->id);
        element(p, edge, id, FL_CLASS_TL_EDGE,
            x_of(p, at) - FL_STYLE_TL_EDGE_W * 0.5f, FL_STYLE_TL_EDGE_W, FL_STYLE_TL_LANE_H, FL_STYLE_TL_LANE_H);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_EDGE_MARK, .no_hover = true});
        ca_div_end(); ca_div_end();
        if (edge) ca_context_menu(&(Ca_CtxMenuDesc){.items = beat_items, .item_count = s->beat_count > 1 ? 3 : 2,
            .on_select = beat_menu, .select_data = edge});
    }
    ca_div_end();

    p->beat_drop_marker = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_BEAT_DROP,
        .position = CA_POSITION_ABSOLUTE, .width = 2, .height = FL_STYLE_TL_LANE_H, .hidden = true, .no_hover = true});
    ca_div_end();
    ca_div_end(); ca_div_end();
    p->scroll = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "timeline-scroll", .style = FL_CLASS_TL_SCROLL});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TL_KEY_CONTENT, .width = content, .height = p->label_height});
    static const char *key_items[] = {"Linear", "Ease in", "Ease out", "Ease in-out", "Hold", "Delete"};
    Fl_TrackIterator lane_tracks = {.scene = s};
    for (const Fl_Track *track; (track = fl_scene_track_next(&lane_tracks));) {
        const Fl_Track *t = track;
        if (!fl_scene_edit_contains(p->edit, t->entity)) continue;
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = selected_key && selected_track == t ? "fl-tl-row selected" : FL_CLASS_TL_ROW,
            .width = content, .height = FL_STYLE_TL_ROW_H});
        Fl_Tick first = INT64_MAX, last = INT64_MIN;
        for (size_t k = 0; k < t->key_count; ++k) {
            Fl_Tick when;
            if (!fl_anchor_sample(s, &t->keys[k].at, &when)) continue;
            if (when < first) first = when;
            if (when > last) last = when;
        }
        if (first != INT64_MAX && last > first) {
            element(p, NULL, NULL, FL_CLASS_TL_SPAN, x_of(p, first), x_of(p, last) - x_of(p, first), 1, FL_STYLE_TL_ROW_H);
            ca_div_end();
        }
        for (size_t k = 0; k < t->key_count; ++k) {
            const Fl_Keyframe *key = &t->keys[k];
            Fl_Tick key_time;
            if (!fl_anchor_sample(s, &key->at, &key_time)) continue;
            char id[48]; snprintf(id, sizeof(id), "key-%llu", (unsigned long long)key->id);
            Fl_TimelineItem *it = item(p, ITEM_KEY, key->id);
            element(p, it, id, p->selected_key == key->id ? FL_CLASS_TL_KEY_SELECTED : FL_CLASS_TL_KEY,
                    x_of(p, key_time) - FL_STYLE_TL_KEY * 0.5f, FL_STYLE_TL_KEY, FL_STYLE_TL_KEY, FL_STYLE_TL_ROW_H);
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
        ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TL_ROW, .width = content, .height = FL_STYLE_TL_ROW_H});
        Fl_Tick start;
        if (!fl_anchor_sample(s, &c->start, &start)) {
            ca_text(&(Ca_TextDesc){.text="Unbound audio start", .style=FL_CLASS_TL_BEAT_TEXT});
            ca_div_end(); continue;
        }
        char id[48], text[192];
        snprintf(id, sizeof(id), "clip-%llu", (unsigned long long)c->id);
        if (c->gain < 0.001) snprintf(text, sizeof(text), "%s  muted", a ? a->name : "?");
        else if (fabs(c->gain - 1) > 0.01) snprintf(text, sizeof(text), "%s  %+.1f dB", a ? a->name : "?", 20 * log10(c->gain));
        else snprintf(text, sizeof(text), "%s", a ? a->name : "?");
        Fl_TimelineItem *it = item(p, ITEM_CLIP, c->id);
        element(p, it, id, p->selected_clip == c->id ? FL_CLASS_TL_CLIP_SELECTED : FL_CLASS_TL_CLIP, x_of(p, start),
                fmaxf(4, x_of(p, start + clip_length(project, c)) - x_of(p, start)), FL_STYLE_TL_ROW_H - 4, FL_STYLE_TL_ROW_H);
        ca_text(&(Ca_TextDesc){.text = text, .style = FL_CLASS_TL_BEAT_TEXT});
        ca_div_end();
        if (it) ca_context_menu(&(Ca_CtxMenuDesc){.items = clip_items, .item_count = 5, .on_select = clip_menu, .select_data = it});
        ca_div_end();
    }
    ca_div_end();
    ca_div_end();
    p->playhead_viewport = ca_div_begin(&(Ca_DivDesc){.position = CA_POSITION_ABSOLUTE,
        .style = "fl-tl-playhead-viewport", .no_hover = true});
    p->playhead = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "timeline-playhead", .style = FL_CLASS_TL_PLAYHEAD,
        .position = CA_POSITION_ABSOLUTE, .width = FL_STYLE_TL_PLAYHEAD_W, .no_hover = true});
    ca_div_end();
    ca_div_end();
    ca_div_end();
    ca_div_end();
    ca_div_end();
    if (p->drag_beat && p->drag_moved) preview_beat_drop(p, p->drag_applied);
    p->scroll_effect = ca_effect(ca_window_instance(window), sync_scroll, p);
    sync_playhead(p);
    if (!p->key_handler) {
        ca_event_set_handler(ca_window_instance(window), CA_EVENT_KEY, on_key, p);
        p->key_handler = true;
    }
}
