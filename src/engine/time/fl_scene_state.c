#include "fl_scene_state.h"
#include "components/fl_path.h"
#include "components/fl_effect.h"
#include "fl_composition.h"
#include <stdio.h>
#include "components/fl_animation.h"
#include "components/fl_style.h"
#include "components/fl_spatial.h"
#include "schema/fl_properties.h"
#include "components/fl_sampled_animation.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


static double bezier_component(double p1, double p2, double u)
{
    double v = 1 - u;
    return 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u;
}

static double cubic_ease(const double *param, double t)
{
    double x1 = param && (param[0] != 0 || param[2] != 0) ? param[0] : 0.42;
    double y1 = param && (param[0] != 0 || param[2] != 0) ? param[1] : 0;
    double x2 = param && (param[0] != 0 || param[2] != 0) ? param[2] : 0.58;
    double y2 = param && (param[0] != 0 || param[2] != 0) ? param[3] : 1;
    double u = t;
    for (int i = 0; i < 6; ++i) {
        double x = bezier_component(x1, x2, u) - t;
        double dx = bezier_component(x1, x2, u + 1e-4) - bezier_component(x1, x2, u - 1e-4);
        if (fabs(dx) < 1e-8) break;
        u = fmin(1, fmax(0, u - x / (dx / 2e-4)));
    }
    return bezier_component(y1, y2, u);
}

static double spring_ease(const double *param, double t)
{
    double stiffness = param && param[0] > 0 ? param[0] : 80;
    double damping = param && param[1] > 0 ? param[1] : 12;
    double omega = sqrt(stiffness);
    double zeta = damping / (2 * omega);
    double time = t * 1.2;
    if (zeta < 1) {
        double wd = omega * sqrt(1 - zeta * zeta);
        return 1 - exp(-zeta * omega * time) * (cos(wd * time) + zeta * omega / wd * sin(wd * time));
    }
    return 1 - exp(-omega * time) * (1 + omega * time);
}

double fl_ease_apply(Fl_Ease ease, double t)
{
    return fl_ease_apply_param(ease, NULL, t);
}

double fl_ease_apply_param(Fl_Ease ease, const double param[4], double t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    switch (ease) {
    case FL_EASE_IN: return t * t * t;
    case FL_EASE_OUT: { double u = 1 - t; return 1 - u * u * u; }
    case FL_EASE_IN_OUT: return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
    case FL_EASE_HOLD: return 0;
    case FL_EASE_CUBIC: return cubic_ease(param, t);
    case FL_EASE_SPRING: return spring_ease(param, t);
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
        double weight = fl_ease_apply_param(before->ease, before->ease_param, progress);
        bool colour = fl_property_definition(t->property)->channels==4;
        for (int c = 0; c < 4; ++c) {
            if (colour && c < 3) out[c] = to_srgb(to_linear(before->value[c]) + (to_linear(after->value[c]) - to_linear(before->value[c])) * weight);
            else out[c] = before->value[c] + (after->value[c] - before->value[c]) * weight;
        }
    }
    return true;
}

static void write_color(unsigned char rgba[4], const double v[4])
{
    for (int c = 0; c < 4; ++c) rgba[c] = (unsigned char)lround(fmin(255, fmax(0, v[c])));
}

/* Write a sampled track value into the entity state. */
void fl_entity_state_apply(Fl_EntityState *e, Fl_AnimProperty property, const double v[4])
{
    double *pose[12] = {&e->pose.x, &e->pose.y, &e->pose.z, &e->pose.rx, &e->pose.ry, &e->pose.rz,
                        &e->pose.sx, &e->pose.sy, &e->pose.sz, &e->pose.px, &e->pose.py, &e->pose.pz};
    switch (property) {
    case FL_PROP_X: case FL_PROP_Y: case FL_PROP_Z: case FL_PROP_RX: case FL_PROP_RY: case FL_PROP_RZ:
    case FL_PROP_SCALE_X: case FL_PROP_SCALE_Y: case FL_PROP_SCALE_Z:
    case FL_PROP_PIVOT_X: case FL_PROP_PIVOT_Y: case FL_PROP_PIVOT_Z:
        *pose[property - FL_PROP_X] = v[0]; break;
    case FL_PROP_WIDTH: e->bounds.width = fmax(0, v[0]); break;
    case FL_PROP_HEIGHT: e->bounds.height = fmax(0, v[0]); break;
    case FL_PROP_DEPTH: e->bounds.depth = fmax(0, v[0]); break;
    case FL_PROP_OPACITY: e->opacity = fmin(1, fmax(0, v[0])); break;
    case FL_PROP_FILL: write_color(e->appearance.fill, v); break;
    case FL_PROP_STROKE: write_color(e->appearance.stroke, v); break;
    case FL_PROP_STROKE_WIDTH: e->appearance.stroke_width = fmax(0, v[0]); break;
    case FL_PROP_FONT_SIZE: e->appearance.font_size = fmin(2048, fmax(1, v[0])); break;
    case FL_PROP_CROP_X: case FL_PROP_CROP_Y: e->crop[property - FL_PROP_CROP_X] = fmin(1, fmax(0, v[0])); break;
    case FL_PROP_CROP_W: case FL_PROP_CROP_H: e->crop[property - FL_PROP_CROP_X] = fmin(1, fmax(0.001, v[0])); break;
    case FL_PROP_RADIUS: e->style.radius=v[0]; break;
    case FL_PROP_BLUR: e->style.blur=v[0]; break;
    case FL_PROP_GRADIENT_ANGLE: e->style.gradient_angle=v[0]; break;
    case FL_PROP_SHADOW_BLUR: e->style.shadow_blur=v[0]; break;
    case FL_PROP_SHADOW_X: e->style.shadow_x=v[0]; break;
    case FL_PROP_SHADOW_Y: e->style.shadow_y=v[0]; break;
    case FL_PROP_GRADIENT_TO: write_color(e->style.gradient_to, v); break;
    case FL_PROP_SHADOW_COLOR: write_color(e->style.shadow_color, v); break;
    case FL_PROP_MEDIA_TIME: e->media_seconds=v[0]; break;
    case FL_PROP_FOV: e->camera.fov_y=v[0]; break;
    case FL_PROP_NEAR: e->camera.near_z=v[0]; break;
    case FL_PROP_FAR: e->camera.far_z=v[0]; break;
    case FL_PROP_EXPOSURE: e->camera.exposure=v[0]; break;
    case FL_PROP_ORTHO: e->camera.ortho_height=v[0]; break;
    case FL_PROP_ASPECT_W: e->camera.aspect_w=(unsigned)fmin(255, fmax(0, v[0])); break;
    case FL_PROP_ASPECT_H: e->camera.aspect_h=(unsigned)fmin(255, fmax(0, v[0])); break;
    case FL_PROP_ROUGHNESS: e->material.roughness=v[0]; break;
    case FL_PROP_METALLIC: e->material.metallic=v[0]; break;
    case FL_PROP_EMISSIVE: e->material.emissive=v[0]; break;
    case FL_PROP_INTENSITY: e->light.intensity=v[0]; break;
    case FL_PROP_RANGE: e->light.range=v[0]; break;
    case FL_PROP_SPOT_ANGLE: e->light.spot_angle=v[0]; break;
    case FL_PROP_LIGHT_COLOR: write_color(e->light.color, v); break;
    case FL_PROP_EMIT_RATE: e->particles.rate=v[0]; break;
    case FL_PROP_LIFE: e->particles.life=v[0]; break;
    case FL_PROP_PARTICLE_SIZE: e->particles.size=v[0]; break;
    case FL_PROP_SPEED: e->particles.speed=v[0]; break;
    case FL_PROP_PARTICLE_COLOR: write_color(e->particles.color, v); break;
    case FL_PROP_BLOOM: e->post.bloom=v[0]; break;
    case FL_PROP_TRIM_START: e->trim_start=fmin(1, fmax(0, v[0])); break;
    case FL_PROP_TRIM_END: e->trim_end=fmin(1, fmax(0, v[0])); break;
    case FL_PROP_TRACKING: e->motion.tracking=v[0]; break;
    case FL_PROP_STAGGER: e->motion.stagger=fmax(0, v[0]); break;
    case FL_PROP_RISE: e->motion.rise=v[0]; break;
    case FL_PROP_TEXT_FADE: e->motion.fade=fmin(1, fmax(0, v[0])); break;
    case FL_PROP_TEXT_DURATION: e->motion.duration=fmax(0, v[0]); break;
    case FL_PROP_TEXT_DELAY: e->motion.delay=fmax(0, v[0]); break;
    case FL_PROP_BLEND: e->style.blend=(unsigned char)fmin(3, fmax(0, v[0])); break;
    case FL_PROP_RADIAL: e->style.radial=v[0] >= 0.5; break;
    case FL_PROP_GLOW: e->style.glow=fmax(0, v[0]); break;
    case FL_PROP_GRADIENT_MID: write_color(e->style.gradient_mid, v); break;
    case FL_PROP_GRADIENT_MID_POS: e->style.gradient_mid_pos=fmin(1, fmax(0, v[0])); break;
    case FL_PROP_VIGNETTE: e->post.vignette=v[0]; break;
    case FL_PROP_GRAIN: e->post.grain=v[0]; break;
    case FL_PROP_LIFT: e->post.lift=v[0]; break;
    case FL_PROP_GAMMA: e->post.gamma=v[0]; break;
    case FL_PROP_GAIN: e->post.gain=v[0]; break;
    case FL_PROP_PARTICLE_SIZE_END: e->particles.size_end=v[0]; break;
    case FL_PROP_GRAVITY: e->particles.gravity=v[0]; break;
    case FL_PROP_SPREAD: e->particles.spread=v[0]; break;
    case FL_PROP_BURST: e->particles.burst=v[0]; break;
    case FL_PROP_PARTICLE_BLEND: e->particles.blend=v[0]; break;
    case FL_PROP_FORCE_X: e->particles.force_x=v[0]; break;
    case FL_PROP_FORCE_Y: e->particles.force_y=v[0]; break;
    case FL_PROP_FORCE_Z: e->particles.force_z=v[0]; break;
    case FL_PROP_PARTICLE_COLOR_END: write_color(e->particles.color_end, v); break;
    case FL_PROP_EFFECT_KIND: e->effect_kind=(uint32_t)fmin(6, fmax(1, v[0])); break;
    case FL_PROP_U0: case FL_PROP_U1: case FL_PROP_U2: case FL_PROP_U3:
    case FL_PROP_U4: case FL_PROP_U5: case FL_PROP_U6: case FL_PROP_U7:
        e->effect_u[property - FL_PROP_U0]=v[0]; break;
    default: break;
    }
}

bool fl_entity_state_sample(const Fl_Scene *scene, size_t index, Fl_Tick tick, Fl_EntityState *out)
{
    if (!scene || !out || index>=scene->entity_count) return false;
    const Fl_Entity *entity=&scene->entities[index];
    *out=(Fl_EntityState){.local_tick=tick,.media_seconds=-1,.pose=fl_pose_identity(),.camera=fl_camera_default(),.material=fl_material_default(),.light=fl_light_default(),.particles=fl_particles_default(),.post=fl_post_default(),.motion=fl_motion_default(),.trim_end=1,.effect_kind=1};
    fl_pose_decode(fl_entity_component(entity,FL_COMPONENT_POSE),&out->pose);
    out->bounds=fl_entity_bounds(entity);
    out->has_camera=fl_camera_decode(fl_entity_component(entity,FL_COMPONENT_CAMERA),&out->camera);
    out->has_mesh=fl_mesh_decode(fl_entity_component(entity,FL_COMPONENT_MESH),&out->mesh);
    out->has_material=fl_material_decode(fl_entity_component(entity,FL_COMPONENT_MATERIAL),&out->material);
    out->has_light=fl_light_decode(fl_entity_component(entity,FL_COMPONENT_LIGHT),&out->light);
    out->has_particles=fl_particles_decode(fl_entity_component(entity,FL_COMPONENT_PARTICLES),&out->particles);
    out->has_post=fl_post_decode(fl_entity_component(entity,FL_COMPONENT_POST),&out->post);
    Fl_Path path;
    out->has_path=fl_path_decode(fl_entity_component(entity,FL_COMPONENT_PATH),&path);
    if (out->has_path) {out->trim_start=path.trim_start; out->trim_end=path.trim_end;}
    out->has_motion=fl_motion_decode(fl_entity_component(entity,FL_COMPONENT_TEXT_MOTION),&out->motion);
    Fl_Effect effect;
    out->has_effect=fl_effect_decode(fl_entity_component(entity,FL_COMPONENT_EFFECT),&effect);
    if (out->has_effect) {out->effect_kind=effect.kind; memcpy(out->effect_u,effect.u,sizeof(out->effect_u)); out->effect_source=effect.source; out->effect_source_size=effect.source_size;}
    out->appearance=fl_widget_appearance(entity); out->opacity=fl_widget_opacity(entity);
    out->style=fl_entity_style(entity); memcpy(out->crop,out->style.crop,sizeof(out->crop));
    const Fl_Animation *animation=fl_entity_animation(entity); double value[4];
    for (size_t i=0; animation && i<animation->track_count; ++i)
        if (fl_track_sample(scene,&animation->tracks[i],tick,value)) fl_entity_state_apply(out,animation->tracks[i].property,value);
    const Fl_Component *sampled=fl_entity_component(entity,FL_COMPONENT_SAMPLED);
    if (sampled) {
        Fl_SampledKey key; bool text;
        if (!fl_sampled_animation_sample(sampled,tick,&key,&text)) return false;
        for (int p=0;p<FL_PROP_COUNT;++p) fl_entity_state_apply(out,(Fl_AnimProperty)p,key.values[p]);
        if (text) {out->text_override=true; out->text=key.text; out->text_size=key.text_size;}
    }
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
    if (st->scripts) fl_script_runtime_evaluate_scene(st->scripts,s,tick,st->entities);
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

bool fl_scene_state_writer(const Fl_SceneState *state, size_t entity, int property, Fl_Id *component, int32_t *layer)
{
    if (!state || !state->valid || entity>=state->count) return false;
    size_t local=entity;
    const Fl_ScriptRuntime *runtime=state->composition ? fl_composition_entity_scripts(state->composition,entity,&local) : state->scripts;
    return fl_script_runtime_writer(runtime,local,property,component,layer);
}

/* Read an entity's numeric channels without exposing native pointers to Lua. */
void fl_entity_state_values(const Fl_EntityState *s, double values[FL_PROP_COUNT][4])
{
    memset(values,0,sizeof(double)*FL_PROP_COUNT*4);
    values[FL_PROP_X][0]=s->pose.x; values[FL_PROP_Y][0]=s->pose.y; values[FL_PROP_Z][0]=s->pose.z;
    values[FL_PROP_RX][0]=s->pose.rx; values[FL_PROP_RY][0]=s->pose.ry; values[FL_PROP_RZ][0]=s->pose.rz;
    values[FL_PROP_SCALE_X][0]=s->pose.sx; values[FL_PROP_SCALE_Y][0]=s->pose.sy; values[FL_PROP_SCALE_Z][0]=s->pose.sz;
    values[FL_PROP_PIVOT_X][0]=s->pose.px; values[FL_PROP_PIVOT_Y][0]=s->pose.py; values[FL_PROP_PIVOT_Z][0]=s->pose.pz;
    values[FL_PROP_WIDTH][0]=s->bounds.width; values[FL_PROP_HEIGHT][0]=s->bounds.height; values[FL_PROP_DEPTH][0]=s->bounds.depth;
    values[FL_PROP_OPACITY][0]=s->opacity; values[FL_PROP_STROKE_WIDTH][0]=s->appearance.stroke_width; values[FL_PROP_FONT_SIZE][0]=s->appearance.font_size;
    values[FL_PROP_CROP_X][0]=s->crop[0]; values[FL_PROP_CROP_Y][0]=s->crop[1]; values[FL_PROP_CROP_W][0]=s->crop[2]; values[FL_PROP_CROP_H][0]=s->crop[3];
    values[FL_PROP_RADIUS][0]=s->style.radius; values[FL_PROP_BLUR][0]=s->style.blur; values[FL_PROP_GRADIENT_ANGLE][0]=s->style.gradient_angle;
    values[FL_PROP_SHADOW_BLUR][0]=s->style.shadow_blur; values[FL_PROP_SHADOW_X][0]=s->style.shadow_x; values[FL_PROP_SHADOW_Y][0]=s->style.shadow_y;
    values[FL_PROP_MEDIA_TIME][0]=s->media_seconds;
    values[FL_PROP_FOV][0]=s->camera.fov_y; values[FL_PROP_NEAR][0]=s->camera.near_z; values[FL_PROP_FAR][0]=s->camera.far_z;
    values[FL_PROP_EXPOSURE][0]=s->camera.exposure; values[FL_PROP_ORTHO][0]=s->camera.ortho_height;
    values[FL_PROP_ASPECT_W][0]=s->camera.aspect_w; values[FL_PROP_ASPECT_H][0]=s->camera.aspect_h;
    values[FL_PROP_ROUGHNESS][0]=s->material.roughness; values[FL_PROP_METALLIC][0]=s->material.metallic; values[FL_PROP_EMISSIVE][0]=s->material.emissive;
    values[FL_PROP_INTENSITY][0]=s->light.intensity; values[FL_PROP_RANGE][0]=s->light.range; values[FL_PROP_SPOT_ANGLE][0]=s->light.spot_angle;
    values[FL_PROP_EMIT_RATE][0]=s->particles.rate; values[FL_PROP_LIFE][0]=s->particles.life; values[FL_PROP_PARTICLE_SIZE][0]=s->particles.size; values[FL_PROP_SPEED][0]=s->particles.speed;
    values[FL_PROP_BLOOM][0]=s->post.bloom;
    values[FL_PROP_TRIM_START][0]=s->trim_start; values[FL_PROP_TRIM_END][0]=s->trim_end;
    values[FL_PROP_TRACKING][0]=s->motion.tracking; values[FL_PROP_STAGGER][0]=s->motion.stagger; values[FL_PROP_RISE][0]=s->motion.rise;
    values[FL_PROP_TEXT_FADE][0]=s->motion.fade; values[FL_PROP_TEXT_DURATION][0]=s->motion.duration;
    values[FL_PROP_TEXT_DELAY][0]=s->motion.delay;
    values[FL_PROP_BLEND][0]=s->style.blend; values[FL_PROP_RADIAL][0]=s->style.radial ? 1 : 0; values[FL_PROP_GLOW][0]=s->style.glow;
    values[FL_PROP_GRADIENT_MID_POS][0]=s->style.gradient_mid_pos;
    values[FL_PROP_VIGNETTE][0]=s->post.vignette; values[FL_PROP_GRAIN][0]=s->post.grain; values[FL_PROP_LIFT][0]=s->post.lift;
    values[FL_PROP_GAMMA][0]=s->post.gamma; values[FL_PROP_GAIN][0]=s->post.gain;
    values[FL_PROP_PARTICLE_SIZE_END][0]=s->particles.size_end; values[FL_PROP_GRAVITY][0]=s->particles.gravity; values[FL_PROP_SPREAD][0]=s->particles.spread;
    values[FL_PROP_BURST][0]=s->particles.burst; values[FL_PROP_PARTICLE_BLEND][0]=s->particles.blend;
    values[FL_PROP_FORCE_X][0]=s->particles.force_x; values[FL_PROP_FORCE_Y][0]=s->particles.force_y; values[FL_PROP_FORCE_Z][0]=s->particles.force_z;
    values[FL_PROP_EFFECT_KIND][0]=s->effect_kind;
    for (int i = 0; i < 8; ++i) values[FL_PROP_U0 + i][0] = s->effect_u[i];
    for (int c = 0; c < 4; ++c) {
        values[FL_PROP_FILL][c] = s->appearance.fill[c]; values[FL_PROP_STROKE][c] = s->appearance.stroke[c];
        values[FL_PROP_GRADIENT_TO][c]=s->style.gradient_to[c]; values[FL_PROP_SHADOW_COLOR][c]=s->style.shadow_color[c];
        values[FL_PROP_GRADIENT_MID][c]=s->style.gradient_mid[c];
        values[FL_PROP_LIGHT_COLOR][c]=s->light.color[c]; values[FL_PROP_PARTICLE_COLOR][c]=s->particles.color[c];
        values[FL_PROP_PARTICLE_COLOR_END][c]=s->particles.color_end[c];
    }
}
