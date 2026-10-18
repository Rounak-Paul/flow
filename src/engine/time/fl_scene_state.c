#include "fl_scene_state.h"
#include "components/fl_animation.h"
#include "components/fl_style.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define INLINE_KEYS 32

double fl_ease_apply(Fl_Ease ease, double t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    switch (ease) {
    case FL_EASE_IN: return t * t * t;
    case FL_EASE_OUT: { double u = 1 - t; return 1 - u * u * u; }
    case FL_EASE_IN_OUT: return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
    case FL_EASE_HOLD: return 0;
    default: return t;
    }
}

/* sRGB channel (0..255) to linear light and back, for perceptually correct colour blends. */
static double to_linear(double v) { v /= 255; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double to_srgb(double v) { v = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055; return fmin(255, fmax(0, v * 255)); }

bool fl_track_sample(const Fl_Scene *s, const Fl_Track *t, Fl_Tick tick, double out[4])
{
    if (!t || !t->key_count || !out) return false;
    size_t inline_order[INLINE_KEYS]; Fl_Tick inline_times[INLINE_KEYS];
    size_t *order = t->key_count <= INLINE_KEYS ? inline_order : malloc(t->key_count * sizeof(*order));
    Fl_Tick *times = t->key_count <= INLINE_KEYS ? inline_times : malloc(t->key_count * sizeof(*times));
    if (!order || !times) {
        if (order != inline_order) free(order);
        if (times != inline_times) free(times);
        memcpy(out, t->keys[0].value, 4 * sizeof(double)); return true;
    }
    for (size_t i = 0; i < t->key_count; ++i) {
        times[i] = fl_anchor_resolve(s, &t->keys[i].at);
        size_t j = i;
        while (j && times[order[j - 1]] > times[i]) { order[j] = order[j - 1]; --j; }
        order[j] = i;
    }
    size_t last = t->key_count - 1, segment = 0;
    while (segment < last && times[order[segment + 1]] <= tick) ++segment;
    const Fl_Keyframe *a = &t->keys[order[segment]];
    if (segment == last || tick <= times[order[0]]) {
        memcpy(out, (tick <= times[order[0]] ? &t->keys[order[0]] : a)->value, 4 * sizeof(double));
    } else {
        const Fl_Keyframe *b = &t->keys[order[segment + 1]];
        Fl_Tick span = times[order[segment + 1]] - times[order[segment]];
        double w = span > 0 ? fl_ease_apply(a->ease, (double)(tick - times[order[segment]]) / (double)span) : 1;
        bool colour = t->property == FL_PROP_FILL || t->property == FL_PROP_STROKE;
        for (int c = 0; c < 4; ++c) {
            if (colour && c < 3) out[c] = to_srgb(to_linear(a->value[c]) + (to_linear(b->value[c]) - to_linear(a->value[c])) * w);
            else out[c] = a->value[c] + (b->value[c] - a->value[c]) * w;
        }
    }
    if (order != inline_order) free(order);
    if (times != inline_times) free(times);
    return true;
}

/* Write a sampled track value into the entity state. */
void fl_entity_state_apply(Fl_EntityState *e, Fl_AnimProperty property, const double v[4])
{
    double *channels[7] = {&e->transform.x, &e->transform.y, &e->transform.width, &e->transform.height,
                           &e->transform.rotation, &e->transform.scale_x, &e->transform.scale_y};
    switch (property) {
    case FL_PROP_X: case FL_PROP_Y: case FL_PROP_ROTATION: case FL_PROP_SCALE_X: case FL_PROP_SCALE_Y:
        *channels[property - FL_PROP_X] = v[0]; break;
    case FL_PROP_WIDTH: case FL_PROP_HEIGHT: *channels[property - FL_PROP_X] = fmax(0, v[0]); break;
    case FL_PROP_OPACITY: e->opacity = fmin(1, fmax(0, v[0])); break;
    case FL_PROP_FILL: case FL_PROP_STROKE: {
        unsigned char *rgba = property == FL_PROP_FILL ? e->appearance.fill : e->appearance.stroke;
        for (int c = 0; c < 4; ++c) rgba[c] = (unsigned char)lround(fmin(255, fmax(0, v[c])));
        break;
    }
    case FL_PROP_STROKE_WIDTH: e->appearance.stroke_width = fmax(0, v[0]); break;
    case FL_PROP_FONT_SIZE: e->appearance.font_size = fmin(2048, fmax(1, v[0])); break;
    case FL_PROP_CROP_X: case FL_PROP_CROP_Y: e->crop[property - FL_PROP_CROP_X] = fmin(1, fmax(0, v[0])); break;
    case FL_PROP_CROP_W: case FL_PROP_CROP_H: e->crop[property - FL_PROP_CROP_X] = fmin(1, fmax(0.001, v[0])); break;
    default: break;
    }
}

bool fl_scene_state_update(Fl_SceneState *st, const Fl_Scene *s, Fl_Tick tick)
{
    if (!st) return false;
    if (!s) { fl_scene_state_clear(st); return true; }
    if (st->valid && st->scene == s->id && st->revision == s->revision && st->tick == tick) return true;
    bool replace_scripts = st->scene != s->id || st->revision != s->revision;
    Fl_ScriptRuntime *scripts = st->scripts;
    if (replace_scripts) {
        scripts = NULL;
        for (size_t i = 0; i < s->entity_count; ++i) {
            Fl_Script script;
            if (fl_script_decode(fl_entity_component(&s->entities[i], FL_COMPONENT_SCRIPT), &script) && script.enabled) {
                scripts = fl_script_runtime_create(s);
                if (!scripts) return false;
                break;
            }
        }
    }
    if (s->entity_count > st->capacity) {
        if (s->entity_count > SIZE_MAX / sizeof(Fl_EntityState)) {
            if (replace_scripts) fl_script_runtime_destroy(scripts);
            return false;
        }
        Fl_EntityState *grown = realloc(st->entities, s->entity_count * sizeof(*grown));
        if (!grown) {if (replace_scripts) fl_script_runtime_destroy(scripts); return false;}
        st->entities = grown; st->capacity = s->entity_count;
    }
    if (replace_scripts) {fl_script_runtime_destroy(st->scripts); st->scripts = scripts;}
    for (size_t i = 0; i < s->entity_count; ++i) {
        const Fl_Entity *e = &s->entities[i];
        Fl_EntityState *v = &st->entities[i];
        v->transform = (Fl_Transform){.scale_x = 1, .scale_y = 1};
        fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &v->transform);
        v->appearance = fl_widget_appearance(e);
        v->opacity = fl_widget_opacity(e);
        v->text_override = false;
        const Fl_Style style = fl_entity_style(e);
        memcpy(v->crop, style.crop, sizeof(v->crop));
    }
    double value[4];
    for (size_t i = 0; i < s->entity_count; ++i) {
        const Fl_Animation *animation = fl_entity_animation(&s->entities[i]);
        for (size_t t = 0; animation && t < animation->track_count; ++t)
            if (fl_track_sample(s, &animation->tracks[t], tick, value)) fl_entity_state_apply(&st->entities[i], animation->tracks[t].property, value);
        if (st->scripts) fl_script_runtime_evaluate(st->scripts, s, i, tick, &st->entities[i]);
    }
    st->scene = s->id; st->revision = s->revision; st->tick = tick; st->count = s->entity_count; st->valid = true;
    return true;
}

void fl_scene_state_clear(Fl_SceneState *st)
{
    if (!st) return;
    fl_script_runtime_destroy(st->scripts);
    free(st->entities);
    *st = (Fl_SceneState){0};
}
