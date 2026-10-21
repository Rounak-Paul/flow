#include "fl_scene_state.h"
#include "fl_composition.h"
#include <stdio.h>
#include "components/fl_animation.h"
#include "components/fl_style.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


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
    const Fl_Keyframe *first = NULL, *before = NULL, *after = NULL;
    Fl_Tick first_tick = 0, before_tick = 0, after_tick = 0;
    for (size_t i = 0; i < t->key_count; ++i) {
        Fl_Tick time;
        if (!fl_anchor_sample(s, &t->keys[i].at, &time)) return false;
        if (!first || time < first_tick) {first = &t->keys[i]; first_tick = time;}
        if (time <= tick && (!before || time >= before_tick)) {before = &t->keys[i]; before_tick = time;}
        if (time > tick && (!after || time < after_tick)) {after = &t->keys[i]; after_tick = time;}
    }
    if (tick <= first_tick || !before) memcpy(out, first->value, 4 * sizeof(double));
    else if (!after) memcpy(out, before->value, 4 * sizeof(double));
    else {
        double progress = (double)(((long double)tick - before_tick) / ((long double)after_tick - before_tick));
        double weight = fl_ease_apply(before->ease, progress);
        bool colour = t->property == FL_PROP_FILL || t->property == FL_PROP_STROKE;
        for (int c = 0; c < 4; ++c) {
            if (colour && c < 3) out[c] = to_srgb(to_linear(before->value[c]) + (to_linear(after->value[c]) - to_linear(before->value[c])) * weight);
            else out[c] = before->value[c] + (after->value[c] - before->value[c]) * weight;
        }
    }
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

bool fl_entity_state_sample(const Fl_Scene *scene, size_t index, Fl_Tick tick, Fl_EntityState *out)
{
    if (!scene || !out || index>=scene->entity_count) return false;
    const Fl_Entity *entity=&scene->entities[index];
    *out=(Fl_EntityState){.local_tick=tick,.transform={.scale_x=1,.scale_y=1}};
    fl_transform_decode(fl_entity_component(entity,FL_COMPONENT_TRANSFORM),&out->transform);
    out->appearance=fl_widget_appearance(entity); out->opacity=fl_widget_opacity(entity);
    Fl_Style style=fl_entity_style(entity); memcpy(out->crop,style.crop,sizeof(out->crop));
    const Fl_Animation *animation=fl_entity_animation(entity); double value[4];
    for (size_t i=0; animation && i<animation->track_count; ++i)
        if (fl_track_sample(scene,&animation->tracks[i],tick,value)) fl_entity_state_apply(out,animation->tracks[i].property,value);
    return true;
}

bool fl_scene_state_update_native(Fl_SceneState *st, const Fl_Scene *s, Fl_Tick tick, const atomic_bool *cancel)
{
    if (!st) return false;
    if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {st->valid=false; return false;}
    if (!s) { fl_scene_state_clear(st); return true; }
    if (st->valid && st->scene == s->id && st->revision == s->revision && st->tick == tick) return true;
    bool replace_scripts = st->scene != s->id || st->revision != s->revision;
    Fl_ScriptRuntime *scripts = st->scripts;
    if (replace_scripts) {
        scripts = NULL;
        bool attached=false;
        for (size_t i=0;i<s->entity_count && !attached;++i) for (size_t j=0;j<s->entities[i].component_count;++j) {
            Fl_Script script;
            if (fl_script_decode(&s->entities[i].components[j],&script) && script.enabled) {attached=true; break;}
        }
        if (attached) {scripts=fl_script_runtime_create(s); if (!scripts) return false;}
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
    fl_script_runtime_begin_frame(st->scripts);
    fl_script_runtime_set_cancel(st->scripts,cancel);
    for (size_t i=0;i<s->entity_count;++i) {
        if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {
            fl_script_runtime_set_cancel(st->scripts,NULL); st->valid=false; return false;
        }
        fl_entity_state_sample(s,i,tick,&st->entities[i]);
    }
    for (size_t i=0;i<s->entity_count;++i) {
        if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {
            fl_script_runtime_set_cancel(st->scripts,NULL); st->valid=false; return false;
        }
        if (st->scripts) fl_script_runtime_evaluate(st->scripts,s,i,tick,&st->entities[i]);
    }
    fl_script_runtime_set_cancel(st->scripts,NULL);
    if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {st->valid=false; return false;}
    st->scene = s->id; st->revision = s->revision; st->tick = tick; st->count = s->entity_count; st->valid = true;
    return true;
}

const Fl_Scene *fl_scene_state_scene(const Fl_SceneState *state, const Fl_Scene *authored)
{
    return state && state->valid && state->composition ? fl_composition_scene(state->composition) : authored;
}

bool fl_scene_state_update_cancel(Fl_SceneState *state, const Fl_Scene *scene, Fl_Tick tick, const atomic_bool *cancel)
{
    if (!state) return false;
    if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {state->valid=false; return false;}
    if (!scene) {fl_scene_state_clear(state); return true;}
    if (!fl_composition_required(scene)) {
        if (state->composition) fl_scene_state_clear(state);
        return fl_scene_state_update_native(state,scene,tick,cancel);
    }
    if (state->valid && state->scene==scene->id && state->revision==scene->revision && state->tick==tick) return true;
    if (!state->composition || state->scene!=scene->id || state->revision!=scene->revision) {
        char error[256]; Fl_Composition *next=fl_composition_create(scene,cancel,error,sizeof(error));
        if (!next) {state->valid=false; snprintf(state->error,sizeof(state->error),"%s",error); return false;}
        fl_scene_state_clear(state); state->composition=next; state->scene=scene->id; state->revision=scene->revision;
    }
    const Fl_Scene *flat=fl_composition_scene(state->composition);
    if (flat->entity_count>state->capacity) {
        Fl_EntityState *entities=realloc(state->entities,flat->entity_count*sizeof(*entities));
        if (!entities) {state->valid=false; return false;}
        state->entities=entities; state->capacity=flat->entity_count;
    }
    state->error[0]=0;
    state->valid=false;
    if (!fl_composition_sample(state->composition,tick,state,cancel)) return false;
    state->count=flat->entity_count; state->tick=tick; state->valid=true; return true;
}

bool fl_scene_state_update(Fl_SceneState *state, const Fl_Scene *scene, Fl_Tick tick)
{
    return fl_scene_state_update_cancel(state,scene,tick,NULL);
}

void fl_scene_state_clear(Fl_SceneState *st)
{
    if (!st) return;
    fl_script_runtime_destroy(st->scripts);
    fl_composition_destroy(st->composition);
    free(st->entities);
    *st = (Fl_SceneState){0};
}

const Fl_ScriptRuntime *fl_scene_state_scripts(const Fl_SceneState *state)
{
    return state ? state->composition ? fl_composition_scripts(state->composition) : state->scripts : NULL;
}

const char *fl_scene_state_entity_error(const Fl_SceneState *state, size_t entity)
{
    return !state ? "" : state->composition ? fl_composition_entity_error(state->composition,entity) : fl_script_runtime_error(state->scripts,entity);
}
