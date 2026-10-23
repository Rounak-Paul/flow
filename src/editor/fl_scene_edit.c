#include "time/fl_composition.h"
#include "fl_scene_edit.h"
#include "project/fl_scope_authoring.h"
#include "time/fl_evaluation.h"
#include "components/fl_component.h"
#include "components/fl_transform.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_GestureTarget {
    Fl_TransformEdit initial;
    Fl_Tick local_tick;
    Fl_TransformEdit applied;   /* last value written by this gesture */
    Fl_Affine inverse, parent_inverse;
    Fl_Mat4 parent_inverse_model;
    bool spin_center;           /* keep the box center fixed while a ring spins */
} Fl_GestureTarget;
struct Fl_SceneEdit {
    Fl_Project *project;
    Fl_JobSystem *jobs;
    Fl_Evaluation *evaluation;
    uint64_t evaluation_generation;
    Fl_Id behavior_scene;
    uint64_t behavior_revision;
    bool scene_scripted;
    Fl_EventBus *bus;
    Fl_EventType selection_event, time_event, snap_event;
    Fl_Id scene, focus;
    Fl_Tick time, gesture_tick;
    Fl_SceneState state;
    Fl_Id *selection;
    size_t count;
    Fl_SceneGeometry geometry;
    Fl_GestureTarget *targets;
    Fl_TransformEdit *updates;
    size_t target_count;
    Fl_EditGesture gesture;
    double start_x, start_y;
    bool applying, snap, gesture_snap, gesture_edit;
    double snap_x, snap_y;
};

/* Publish position changes or evaluation invalidations with their distinct clock semantics. */
static void notify_time(Fl_SceneEdit *edit, Fl_TimeChangeKind kind)
{
    Fl_TimeChange change = {edit, edit->time, kind};
    fl_event_publish(edit->bus, edit->time_event, &change, sizeof(change));
}

/* Publish selection changes after ids have been fully updated. */
static void notify(Fl_SceneEdit *edit)
{
    Fl_SelectionChange change = {edit};
    fl_event_publish(edit->bus, edit->selection_event, &change, sizeof(change));
}

/* Finish an abandoned gesture before releasing its local geometry snapshots. */
static void clear_gesture(Fl_SceneEdit *edit)
{
    if (edit->gesture_edit) {edit->gesture_edit=false; fl_project_edit_commit(edit->project);}
    free(edit->targets); free(edit->updates);
    edit->targets = NULL; edit->updates = NULL; edit->target_count = 0;
}

/* Destroy only after cancellation has fenced the worker's isolated document reads. */
static void clear_evaluation(Fl_SceneEdit *edit)
{
    fl_evaluation_destroy(edit->evaluation); edit->evaluation=NULL;
}

/* A removed generated key clears only the local focus; its persistent owner remains selected. */
static void reconcile_focus(Fl_SceneEdit *edit, const Fl_SceneState *state)
{
    if (!edit->focus || !state || !state->valid) return;
    const Fl_Scene *root=fl_project_scene(edit->project,edit->scene);
    if (!root || state->revision!=root->revision) return;
    if (!fl_scene_entity(fl_scene_state_scene(state,root),edit->focus)) {edit->focus=0; notify(edit);}
}

/* Publish at the event boundary so geometry and state remain coherent throughout one rendered frame. */
static bool evaluation_done(const Fl_Event *event, void *data)
{
    Fl_SceneEdit *edit=data;
    if (!fl_evaluation_matches(edit->evaluation,event->payload)) return false;
    fl_evaluation_publish(edit->evaluation); reconcile_focus(edit,fl_evaluation_state(edit->evaluation)); notify_time(edit, FL_TIME_EVALUATION);
    return false;
}

/* Reconcile selection with document deletion; external mutations invalidate active gestures. */
static bool document_changed(const Fl_Event *event, void *data)
{
    Fl_SceneEdit *edit = data; const Fl_ProjectChange *change = event->payload;
    if (change->project != edit->project) return false;
    clear_evaluation(edit);
    if (!edit->applying) clear_gesture(edit);
    const Fl_Scene *scene = fl_project_scene(edit->project, edit->scene);
    if (!scene) {
        scene = fl_project_scene_at(edit->project, 0); edit->scene = scene ? scene->id : 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < edit->count; ++i)
        if (fl_scene_entity(scene, edit->selection[i])) edit->selection[count++] = edit->selection[i];
    if (count != edit->count) { edit->count = count; edit->focus=0; notify(edit); }
    Fl_Tick length = fl_scene_length(scene);
    if (edit->time > length) { edit->time = length; notify_time(edit, FL_TIME_POSITION); }
    return false;
}

Fl_SceneEdit *fl_scene_edit_create(Fl_Project *project, Fl_EventBus *bus, Fl_JobSystem *jobs)
{
    if (!project || !bus || (jobs && !fl_jobs_done_event(jobs))) return NULL;
    Fl_SceneEdit *edit = calloc(1, sizeof(*edit)); if (!edit) return NULL;
    edit->project = project; edit->bus = bus; edit->jobs=jobs;
    const Fl_Scene *scene = fl_project_scene_at(project, 0); edit->scene = scene ? scene->id : 0;
    edit->selection_event = fl_event_type_register(bus, FL_EVENT_SELECTION_CHANGED, sizeof(Fl_SelectionChange));
    edit->time_event = fl_event_type_register(bus, FL_EVENT_TIME_CHANGED, sizeof(Fl_TimeChange));
    edit->snap_event = fl_event_type_register(bus, FL_EVENT_SNAP_CHANGED, sizeof(Fl_SnapChange));
    Fl_EventType changed = fl_event_type_register(bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange));
    if (edit->snap_event == FL_EVENT_TYPE_INVALID || edit->selection_event == FL_EVENT_TYPE_INVALID || edit->time_event == FL_EVENT_TYPE_INVALID || changed == FL_EVENT_TYPE_INVALID ||
        !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = changed, .fn = document_changed, .user_data = edit, .owner = edit}).generation) {
        free(edit); return NULL;
    }
    if (jobs && !fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_jobs_done_event(jobs),.fn=evaluation_done,.user_data=edit,.owner=edit}).generation) {
        fl_scene_edit_destroy(edit); return NULL;
    }
    return edit;
}
void fl_scene_edit_destroy(Fl_SceneEdit *edit)
{
    if (!edit) return;
    fl_event_unsubscribe_owner(edit->bus, edit); clear_gesture(edit); clear_evaluation(edit);
    fl_scene_geometry_clear(&edit->geometry); fl_scene_state_clear(&edit->state); free(edit->selection); free(edit);
}
Fl_Project *fl_scene_edit_project(const Fl_SceneEdit *edit) { return edit ? edit->project : NULL; }
Fl_Id fl_scene_edit_scene(const Fl_SceneEdit *edit) { return edit ? edit->scene : 0; }
Fl_Id fl_scene_edit_primary(const Fl_SceneEdit *edit) { return edit && edit->count ? edit->selection[edit->count-1] : 0; }
size_t fl_scene_edit_count(const Fl_SceneEdit *edit) { return edit ? edit->count : 0; }
bool fl_scene_edit_contains(const Fl_SceneEdit *edit, Fl_Id id)
{
    if (edit && edit->focus) return edit->focus==id;
    if (edit) for (size_t i = 0; i < edit->count; ++i) if (edit->selection[i] == id) return true;
    return false;
}

bool fl_scene_edit_select(Fl_SceneEdit *edit, Fl_Id scene_id, Fl_Id id, bool additive)
{
    if (!edit) return false;
    const Fl_Scene *scene = fl_project_scene(edit->project, scene_id);
    Fl_Id focus=0;
    if (scene && id && !fl_scene_entity(scene,id) && edit->scene==scene_id) {
        const Fl_SceneState *state=fl_scene_edit_state(edit);
        if (edit->evaluation && (!state || !state->valid || state->revision!=scene->revision)) {
            if (!fl_evaluation_wait(edit->evaluation,edit->time)) return false;
            state=fl_scene_edit_state(edit);
        }
        const Fl_Scene *flat=fl_scene_state_scene(state,scene); const Fl_Entity *entity=fl_scene_entity(flat,id);
        const Fl_Origin *origin=entity && state ? fl_composition_origin(state->composition,(size_t)(entity-flat->entities)) : NULL;
        if (!origin || !origin->generated || !state || state->revision!=scene->revision) return false;
        focus=id; id=origin->owner; additive=false;
    }
    if (!scene || (id && !fl_scene_entity(scene, id))) return false;
    bool same_scene = edit->scene == scene_id;
    size_t found = 0;
    while (found < edit->count && edit->selection[found] != id) ++found;
    bool remove = additive && same_scene && found < edit->count;
    size_t count = additive && same_scene ? edit->count : 0;
    if (id && !remove) {
        if (count >= SIZE_MAX / sizeof(Fl_Id)-1) return false;
        Fl_Id *selection = realloc(edit->selection, (count+1)*sizeof(*selection));
        if (!selection) return false;
        edit->selection = selection; selection[count++] = id;
    } else if (remove) {
        memmove(edit->selection+found, edit->selection+found+1, (count-found-1)*sizeof(*edit->selection)); --count;
    } else count = 0;
    clear_gesture(edit); edit->scene = scene_id; edit->count = count; edit->focus=focus; notify(edit);
    if (!same_scene && edit->time) { edit->time = 0; notify_time(edit, FL_TIME_POSITION); }
    return true;
}
Fl_Tick fl_scene_edit_time(const Fl_SceneEdit *edit) { return edit ? edit->time : 0; }
void fl_scene_edit_set_time(Fl_SceneEdit *edit, Fl_Tick tick)
{
    if (!edit) return;
    Fl_Tick length = fl_scene_length(fl_project_scene(edit->project, edit->scene));
    tick = tick < 0 ? 0 : tick > length ? length : tick;
    if (tick == edit->time) return;
    edit->time = tick;
    notify_time(edit, FL_TIME_POSITION);
}
const Fl_SceneState *fl_scene_edit_state(Fl_SceneEdit *edit)
{
    if (!edit) return NULL;
    const Fl_Scene *scene=fl_project_scene(edit->project,edit->scene);
    if (scene && (edit->behavior_scene!=scene->id || edit->behavior_revision!=scene->revision)) {
        edit->scene_scripted=false; edit->behavior_scene=scene->id; edit->behavior_revision=scene->revision;
        for (size_t i=0; !edit->scene_scripted && i<scene->entity_count; ++i) {
            const Fl_Entity *entity=&scene->entities[i];
            for (size_t j=0;j<entity->component_count;++j) {
                Fl_Script script;
                if ((entity->components[j].type==FL_COMPONENT_PROTOTYPE || entity->components[j].type==FL_COMPONENT_COMPOSITION) || (fl_script_decode(&entity->components[j],&script) && script.enabled)) {edit->scene_scripted=true; break;}
            }
        }
    }
    if (edit->jobs && scene && edit->scene_scripted && !fl_project_edit_active(edit->project)) {
        uint64_t generation=fl_project_generation(edit->project);
        const Fl_Scene *evaluated=fl_evaluation_scene(edit->evaluation);
        if (evaluated && (evaluated->id!=edit->scene || edit->evaluation_generation!=generation)) clear_evaluation(edit);
        if (!edit->evaluation) {
            edit->evaluation=fl_evaluation_create(edit->jobs,edit->project,edit->scene);
            edit->evaluation_generation=generation;
        }
        if (!edit->evaluation) return NULL;
        bool pending=fl_evaluation_pending(edit->evaluation);
        fl_evaluation_request(edit->evaluation,edit->time);
        if (!pending && fl_evaluation_pending(edit->evaluation)) notify_time(edit, FL_TIME_EVALUATION);
        return fl_evaluation_state(edit->evaluation);
    }
    if (!fl_scene_state_update(&edit->state,scene,edit->time)) return NULL;
    reconcile_focus(edit,&edit->state); return &edit->state;
}

bool fl_scene_edit_evaluating(const Fl_SceneEdit *edit)
{
    return edit && fl_evaluation_pending(edit->evaluation);
}

const Fl_SceneGeometry *fl_scene_edit_geometry(Fl_SceneEdit *edit)
{
    const Fl_SceneState *state = fl_scene_edit_state(edit);
    if (!edit || !fl_scene_geometry_update(&edit->geometry, fl_project_scene(edit->project, edit->scene), state)) return NULL;
    return &edit->geometry;
}

bool fl_scene_edit_snap(const Fl_SceneEdit *edit) { return edit && edit->snap; }
void fl_scene_edit_set_snap(Fl_SceneEdit *edit, bool enabled)
{
    if (!edit || edit->snap == enabled) return;
    edit->snap = enabled;
    Fl_SnapChange change = {edit};
    fl_event_publish(edit->bus, edit->snap_event, &change, sizeof(change));
}

/* Snap parent-local insertion coordinates to the world grid, preserving transformed parents. */
static bool snap_insertion(Fl_SceneEdit *edit, Fl_Id parent, double *x, double *y)
{
    if (!edit || !isfinite(*x) || !isfinite(*y)) return false;
    if (!edit->snap) return true;
    Fl_Affine world = fl_affine_identity(), inverse;
    if (parent) {
        const Fl_Scene *scene = fl_project_scene(edit->project, edit->scene);
        const Fl_Entity *entity = fl_scene_entity(scene, parent);
        const Fl_SceneGeometry *geometry = fl_scene_edit_geometry(edit);
        if (!entity || !geometry) return false;
        world = geometry->entities[entity - scene->entities].world;
    }
    if (!fl_affine_inverse(world, &inverse)) return false;
    double wx, wy;
    fl_affine_point(world, *x, *y, &wx, &wy);
    wx = round(wx / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING;
    wy = round(wy / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING;
    fl_affine_point(inverse, wx, wy, x, y);
    return isfinite(*x) && isfinite(*y);
}

static bool axis_gesture(Fl_EditGesture mode) { return mode >= FL_GESTURE_AXIS_X && mode <= FL_GESTURE_AXIS_Z; }
static bool ring_gesture(Fl_EditGesture mode) { return mode >= FL_GESTURE_RING_X && mode <= FL_GESTURE_RING_Z; }
static bool scale_gesture(Fl_EditGesture mode) { return mode >= FL_GESTURE_SCALE_X && mode <= FL_GESTURE_SCALE_Z; }
static bool slide_gesture(Fl_EditGesture mode) { return mode == FL_GESTURE_MOVE || mode == FL_GESTURE_PLANE; }

bool fl_scene_edit_begin(Fl_SceneEdit *edit, Fl_EditGesture mode, double x, double y)
{
    bool axis = axis_gesture(mode), ring = ring_gesture(mode), scale = scale_gesture(mode);
    if (!edit || !edit->count || mode < FL_GESTURE_MOVE || mode > FL_GESTURE_PLANE || (ring && edit->count != 1) ||
        (!axis && !ring && !scale && !slide_gesture(mode) && edit->count != 1) || !isfinite(x) || !isfinite(y)) return false;
    clear_gesture(edit);
    fl_scene_edit_state(edit);
    if (edit->evaluation && !fl_evaluation_wait(edit->evaluation,edit->time)) return false;
    const Fl_SceneState *evaluated=fl_scene_edit_state(edit);
    const Fl_Scene *scene = fl_scene_state_scene(evaluated,fl_project_scene(edit->project,edit->scene));
    const Fl_SceneGeometry *geometry = fl_scene_edit_geometry(edit);
    if (!scene || !geometry || edit->count > SIZE_MAX / sizeof(Fl_GestureTarget)) return false;
    edit->targets = calloc(edit->count, sizeof(*edit->targets));
    edit->updates = calloc(edit->count, sizeof(*edit->updates));
    if (!edit->targets || !edit->updates) { clear_gesture(edit); return false; }
    for (size_t i = 0; i < edit->count; ++i) {
        const Fl_Entity *entity = fl_scene_entity(scene, edit->focus ? edit->focus : edit->selection[i]);
        if (!entity) {clear_gesture(edit); return false;}
        const Fl_Entity *parent = fl_scene_entity(scene, entity->parent);
        const Fl_Entity *ancestor = parent; bool inherited = false;
        while (ancestor) { inherited |= fl_scene_edit_contains(edit, ancestor->id); ancestor = fl_scene_entity(scene, ancestor->parent); }
        if (inherited) continue;
        const Fl_SceneState *state=fl_scene_edit_state(edit);
        size_t index=(size_t)(entity-scene->entities);
        int properties[4]={FL_PROP_X,FL_PROP_Y,FL_PROP_WIDTH,FL_PROP_HEIGHT};
        size_t property_count = 4;
        if (mode==FL_GESTURE_ROTATE) { properties[0]=FL_PROP_ROTATION; property_count=1; }
        else if (mode==FL_GESTURE_RING_X) { properties[0]=FL_PROP_RX; property_count=1; }
        else if (mode==FL_GESTURE_RING_Y) { properties[0]=FL_PROP_RY; property_count=1; }
        else if (mode==FL_GESTURE_RING_Z) { properties[0]=FL_PROP_RZ; property_count=1; }
        else if (mode==FL_GESTURE_SCALE_X) { properties[0]=FL_PROP_SCALE_X; property_count=1; }
        else if (mode==FL_GESTURE_SCALE_Y) { properties[0]=FL_PROP_SCALE_Y; property_count=1; }
        else if (mode==FL_GESTURE_SCALE_Z) { properties[0]=FL_PROP_SCALE_Z; property_count=1; }
        else if (axis) { properties[0]=FL_PROP_X; properties[1]=FL_PROP_Y; properties[2]=FL_PROP_Z; property_count=3; }
        else if (slide_gesture(mode)) property_count=2;
        for (size_t p=0;p<property_count;++p) if (fl_entity_component(entity,FL_COMPONENT_SAMPLED) || fl_scene_state_writer(state,index,properties[p],NULL,NULL)) {clear_gesture(edit); return false;}
        const Fl_ResolvedEntity *v = &geometry->entities[entity-scene->entities];
        Fl_GestureTarget *target = &edit->targets[edit->target_count];
        target->local_tick=state->entities[index].local_tick;
        target->initial = (Fl_TransformEdit){entity->id, v->pose, v->bounds}; target->applied = target->initial; target->inverse = v->inverse;
        target->spin_center = !v->mesh && !fl_entity_component(entity, FL_COMPONENT_CAMERA);
        if (parent) {
            const Fl_ResolvedEntity *above = &geometry->entities[parent-scene->entities];
            if (!above->invertible) { clear_gesture(edit); return false; }
            target->parent_inverse_model = above->inverse_model;
        } else target->parent_inverse_model = fl_mat4_identity();
        if (!v->invertible || !fl_affine_inverse(parent ? geometry->entities[parent-scene->entities].world : fl_affine_identity(), &target->parent_inverse)) {
            clear_gesture(edit); return false;
        }
        if (!edit->target_count) { edit->snap_x = v->world.v[2]; edit->snap_y = v->world.v[5]; }
        ++edit->target_count;
    }
    edit->gesture_snap = edit->snap;
    edit->gesture_tick = edit->time;
    edit->gesture = mode; edit->start_x = x; edit->start_y = y;
    if (!edit->target_count || !fl_project_edit_begin(edit->project,"Transform selection",!edit->focus)) {clear_gesture(edit); return false;}
    edit->gesture_edit=true; return true;
}

static bool write_updates(Fl_SceneEdit *edit)
{
    bool changed = false;
    for (size_t i = 0; i < edit->target_count; ++i)
        changed |= memcmp(&edit->targets[i].applied, &edit->updates[i], sizeof(Fl_TransformEdit)) != 0;
    if (!changed) return true;
    edit->applying = true;
    bool ok = false;
    if (edit->focus && edit->target_count == 1) {
        const Fl_SceneState *state = fl_scene_edit_state(edit);
        const Fl_Scene *flat = state ? fl_scene_state_scene(state, NULL) : NULL;
        const Fl_Entity *entity = fl_scene_entity(flat, edit->focus);
        if (entity) {
            size_t index = (size_t)(entity - flat->entities);
            unsigned char pose_bytes[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES];
            fl_pose_encode(&edit->updates[0].pose, pose_bytes);
            fl_bounds_encode(&edit->updates[0].bounds, bounds_bytes);
            Fl_Component components[2] = {{.type = FL_COMPONENT_POSE, .version = 1, .data = pose_bytes, .size = FL_POSE_BYTES},
                                          {.type = FL_COMPONENT_BOUNDS, .version = 1, .data = bounds_bytes, .size = FL_BOUNDS_BYTES}};
            ok = fl_scope_configure(edit->project, edit->scene, state->composition, index, NULL, entity->visible, components, 2, 0, edit->targets[0].local_tick);
        }
    } else ok = fl_project_apply(edit->project, &(Fl_Op){.kind = FL_OP_TRANSFORMS, .scene = edit->scene,
        .transforms = edit->updates, .transform_count = edit->target_count, .keyed = true, .tick = edit->gesture_tick}, NULL);
    edit->applying = false;
    if (ok) for (size_t i = 0; i < edit->target_count; ++i) edit->targets[i].applied = edit->updates[i];
    return ok;
}

bool fl_scene_edit_update(Fl_SceneEdit *edit, double x, double y)
{
    if (!edit || !edit->target_count || !isfinite(x) || !isfinite(y)) return false;
    if (edit->gesture_snap && (x != edit->start_x || y != edit->start_y) && edit->gesture != FL_GESTURE_ROTATE) {
        if (slide_gesture(edit->gesture)) {
            x = edit->start_x + round((edit->snap_x + x - edit->start_x) / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING - edit->snap_x;
            y = edit->start_y + round((edit->snap_y + y - edit->start_y) / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING - edit->snap_y;
        } else {
            x = round(x / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING;
            y = round(y / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING;
        }
    }
    for (size_t i = 0; i < edit->target_count; ++i) {
        const Fl_GestureTarget *target = &edit->targets[i];
        Fl_Pose t = target->initial.pose; Fl_Bounds box = target->initial.bounds;
        if (x==edit->start_x&&y==edit->start_y) {
            edit->updates[i]=target->initial;continue;
        }
        if (slide_gesture(edit->gesture)) {
            double dx = x-edit->start_x, dy = y-edit->start_y;
            t.x += target->parent_inverse.v[0]*dx+target->parent_inverse.v[1]*dy;
            t.y += target->parent_inverse.v[3]*dx+target->parent_inverse.v[4]*dy;
        } else if (edit->gesture == FL_GESTURE_ROTATE) {
            double px,py,sx,sy,cx,cy;
            fl_affine_point(target->parent_inverse,x,y,&px,&py);
            fl_affine_point(target->parent_inverse,edit->start_x,edit->start_y,&sx,&sy);
            Fl_Affine local = fl_affine_pose(&t); fl_affine_point(local,box.width*.5,box.height*.5,&cx,&cy);
            t.rz += atan2(py-cy,px-cx)-atan2(sy-cy,sx-cx);
            double c=cos(t.rz),s=sin(t.rz);
            t.x = cx-c*t.sx*box.width*.5+s*t.sy*box.height*.5;
            t.y = cy-s*t.sx*box.width*.5-c*t.sy*box.height*.5;
        } else {
            double lx,ly,sx,sy;
            fl_affine_point(target->inverse,x,y,&lx,&ly);
            fl_affine_point(target->inverse,edit->start_x,edit->start_y,&sx,&sy);
            bool west = edit->gesture == FL_GESTURE_NW || edit->gesture == FL_GESTURE_SW;
            bool north = edit->gesture == FL_GESTURE_NW || edit->gesture == FL_GESTURE_NE;
            double w=fmax(1,box.width+(west ? sx-lx : lx-sx));
            double h=fmax(1,box.height+(north ? sy-ly : ly-sy));
            Fl_Affine local=fl_affine_pose(&t);
            double dx=west ? box.width-w : 0,dy=north ? box.height-h : 0;
            t.x += local.v[0]*dx+local.v[1]*dy; t.y += local.v[3]*dx+local.v[4]*dy;
            box.width=w; box.height=h;
        }
        edit->updates[i]=(Fl_TransformEdit){target->initial.entity,t,box};
    }
    return write_updates(edit);
}

static Fl_Mat4 euler_matrix(const Fl_Pose *pose)
{
    return fl_mat4_mul(fl_mat4_rotation_z(pose->rz), fl_mat4_mul(fl_mat4_rotation_y(pose->ry), fl_mat4_rotation_x(pose->rx)));
}

static Fl_Mat4 rotation_about(Fl_Vec3 axis, double radians)
{
    double n = hypot(hypot(axis.x, axis.y), axis.z);
    if (!(n > 1e-12)) return fl_mat4_identity();
    double x = axis.x / n, y = axis.y / n, z = axis.z / n, c = cos(radians), s = sin(radians), t = 1 - c;
    Fl_Mat4 m = fl_mat4_identity();
    m.m[0] = t * x * x + c; m.m[4] = t * x * y - s * z; m.m[8] = t * x * z + s * y;
    m.m[1] = t * y * x + s * z; m.m[5] = t * y * y + c; m.m[9] = t * y * z - s * x;
    m.m[2] = t * z * x - s * y; m.m[6] = t * z * y + s * x; m.m[10] = t * z * z + c;
    return m;
}

static void euler_from_matrix(Fl_Mat4 m, double *rx, double *ry, double *rz)
{
    double cy = hypot(m.m[0], m.m[1]);
    *ry = atan2(-m.m[2], cy);
    if (cy > 1e-6) { *rx = atan2(m.m[6], m.m[10]); *rz = atan2(m.m[1], m.m[0]); }
    else { *rx = atan2(-m.m[4], m.m[5]); *rz = 0; }
}

/* Keep the box center where it was after the orientation changes. */
static void hold_center(const Fl_Pose *initial, Fl_Pose *pose, const Fl_Bounds *box)
{
    Fl_Vec3 center = {box->width * 0.5, box->height * 0.5, 0};
    Fl_Mat4 placed = fl_mat4_mul(fl_mat4_scale(initial->sx, initial->sy, initial->sz), fl_mat4_translation(-initial->px, -initial->py, -initial->pz));
    Fl_Vec3 local = fl_mat4_point(placed, center);
    Fl_Vec3 held = fl_mat4_point(fl_mat4_mul(fl_mat4_translation(initial->x, initial->y, initial->z), euler_matrix(initial)), local);
    Fl_Vec3 spun = fl_mat4_vector(euler_matrix(pose), local);
    pose->x = held.x - spun.x; pose->y = held.y - spun.y; pose->z = held.z - spun.z;
}

bool fl_scene_edit_update_spatial(Fl_SceneEdit *edit, Fl_Vec3 world_delta, Fl_Vec3 world_axis, double radians)
{
    if (!edit || !edit->target_count || !isfinite(world_delta.x) || !isfinite(world_delta.y) || !isfinite(world_delta.z) || !isfinite(radians)) return false;
    bool axis = axis_gesture(edit->gesture), ring = ring_gesture(edit->gesture), scale = scale_gesture(edit->gesture);
    if (!axis && !ring && !scale) return false;
    if (scale) {
        if (!isfinite(radians) || fabs(radians) > 1e6) return false;
    } else if (axis && edit->gesture_snap) {
        world_delta.x = round(world_delta.x / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING;
        world_delta.y = round(world_delta.y / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING;
        world_delta.z = round(world_delta.z / FL_SCENE_GRID_SPACING) * FL_SCENE_GRID_SPACING;
    }
    for (size_t i = 0; i < edit->target_count; ++i) {
        const Fl_GestureTarget *target = &edit->targets[i];
        Fl_Pose t = target->initial.pose;
        if (axis) {
            Fl_Vec3 local = fl_mat4_vector(target->parent_inverse_model, world_delta);
            t.x += local.x; t.y += local.y; t.z += local.z;
        } else if (scale) {
            if (edit->gesture == FL_GESTURE_SCALE_X) t.sx = target->initial.pose.sx * radians;
            else if (edit->gesture == FL_GESTURE_SCALE_Y) t.sy = target->initial.pose.sy * radians;
            else t.sz = target->initial.pose.sz * radians;
        } else {
            Fl_Mat4 spun = fl_mat4_mul(rotation_about(world_axis, radians), euler_matrix(&t));
            euler_from_matrix(spun, &t.rx, &t.ry, &t.rz);
            if (target->spin_center) hold_center(&target->initial.pose, &t, &target->initial.bounds);
        }
        edit->updates[i] = (Fl_TransformEdit){target->initial.entity, t, target->initial.bounds};
    }
    return write_updates(edit);
}
void fl_scene_edit_end(Fl_SceneEdit *edit, bool cancel)
{
    if (!edit) return;
    bool active=edit->gesture_edit; edit->gesture_edit=false;
    if (active) {if (cancel) fl_project_edit_cancel(edit->project); else fl_project_edit_commit(edit->project);}
    clear_gesture(edit);
}
bool fl_scene_edit_insert(Fl_SceneEdit *edit, Fl_WidgetKind kind, Fl_Id parent, double x, double y)
{
    Fl_Id id;
    return snap_insertion(edit,parent,&x,&y) && fl_widget_create(edit->project,edit->scene,parent,kind,x,y,&id) &&
        fl_scene_edit_select(edit,edit->scene,id,false);
}

bool fl_scene_edit_insert_media(Fl_SceneEdit *edit, Fl_Id asset, Fl_Id parent, double x, double y, bool centred)
{
    const Fl_Scene *scene=edit ? fl_project_scene(edit->project,edit->scene) : NULL;
    const Fl_Asset *record=edit ? fl_project_asset(edit->project,asset) : NULL;
    if (!scene||!record||(record->kind!=FL_ASSET_IMAGE&&record->kind!=FL_ASSET_VIDEO)||!isfinite(x)||!isfinite(y)) return false;
    double w=record->width ? (double)record->width : 320,h=record->height ? (double)record->height : 180;
    double fit=fmin(1.0,fmin(scene->width/w,scene->height/h));
    w*=fit;h*=fit;
    Fl_Id id;
    if (centred) {x-=w*.5;y-=h*.5;}
    if (!snap_insertion(edit,parent,&x,&y)) return false;
    bool created=record->kind==FL_ASSET_VIDEO ? fl_widget_create_video(edit->project,edit->scene,parent,asset,x,y,w,h,&id)
                                               : fl_widget_create_image(edit->project,edit->scene,parent,asset,x,y,w,h,&id);
    return created&&fl_scene_edit_select(edit,edit->scene,id,false);
}

static Fl_Id pick_owner(Fl_SceneEdit *edit, Fl_Id id)
{
    const Fl_SceneState *state=fl_scene_edit_state(edit);
    const Fl_Scene *scene=fl_scene_state_scene(state,edit ? fl_project_scene(edit->project,edit->scene) : NULL);
    const Fl_Entity *entity=fl_scene_entity(scene,id);
    const Fl_Origin *origin=entity && state ? fl_composition_origin(state->composition,(size_t)(entity-scene->entities)) : NULL;
    (void)origin; return id;
}

Fl_Id fl_scene_edit_pick(Fl_SceneEdit *edit, double x, double y, double tolerance)
{
    const Fl_SceneGeometry *geometry=fl_scene_edit_geometry(edit);
    const Fl_SceneState *state=fl_scene_edit_state(edit);
    const Fl_Scene *scene=fl_scene_state_scene(state,edit ? fl_project_scene(edit->project,edit->scene) : NULL);
    return pick_owner(edit, fl_scene_geometry_pick(geometry,scene,x,y,tolerance));
}

Fl_Id fl_scene_edit_pick_ray(Fl_SceneEdit *edit, Fl_Vec3 origin, Fl_Vec3 direction)
{
    const Fl_SceneGeometry *geometry=fl_scene_edit_geometry(edit);
    const Fl_SceneState *state=fl_scene_edit_state(edit);
    const Fl_Scene *scene=fl_scene_state_scene(state,edit ? fl_project_scene(edit->project,edit->scene) : NULL);
    return pick_owner(edit, fl_scene_geometry_pick_ray(geometry,scene,origin,direction));
}

Fl_JobSystem *fl_scene_edit_jobs(const Fl_SceneEdit *edit) {return edit ? edit->jobs : NULL;}

Fl_Id fl_scene_edit_focus(const Fl_SceneEdit *edit) {return edit && edit->focus ? edit->focus : fl_scene_edit_primary(edit);}

/* Resolve the focused generated object only against a complete current document revision. */
static const Fl_Entity *focused_entity(Fl_SceneEdit *edit, const Fl_SceneState **out)
{
    const Fl_SceneState *state=fl_scene_edit_state(edit); const Fl_Scene *root=fl_project_scene(edit->project,edit->scene);
    const Fl_Scene *flat=fl_scene_state_scene(state,root);
    if (out) *out=state;
    return state && root && state->valid && state->revision==root->revision ? fl_scene_entity(flat,edit->focus) : NULL;
}

const Fl_Scene *fl_scene_edit_timeline(Fl_SceneEdit *edit)
{
    if (!edit) return NULL;
    if (edit->focus) {
        const Fl_SceneState *state=NULL; const Fl_Entity *entity=focused_entity(edit,&state);
        if (!entity) return NULL;
        const Fl_Scene *flat=fl_composition_scene(state->composition);
        return fl_composition_entity_scope(state->composition,(size_t)(entity-flat->entities));
    }
    return fl_project_scene(edit->project,edit->scene);
}

Fl_Tick fl_scene_edit_local_time(Fl_SceneEdit *edit)
{
    if (!edit) return 0;
    if (edit->focus) {
        const Fl_SceneState *state=NULL; const Fl_Entity *entity=focused_entity(edit,&state);
        const Fl_Scene *flat=state ? fl_composition_scene(state->composition) : NULL;
        return entity ? state->entities[entity-flat->entities].local_tick : 0;
    }
    return edit->time;
}

bool fl_scene_edit_root_time(Fl_SceneEdit *edit, Fl_Tick local_tick, Fl_Tick *root_tick)
{
    if (!edit || !root_tick) return false;
    if (!edit->focus) {*root_tick=local_tick; return true;}
    const Fl_SceneState *state=NULL; const Fl_Entity *entity=focused_entity(edit,&state);
    const Fl_Scene *flat=state ? fl_composition_scene(state->composition) : NULL;
    return entity && fl_composition_entity_root_tick(state->composition,(size_t)(entity-flat->entities),local_tick,edit->time,root_tick);
}

bool fl_scene_edit_timeline_apply(Fl_SceneEdit *edit, const Fl_Op *operation, Fl_Id *created)
{
    if (!edit || !operation) return false;
    if (!edit->focus) return fl_project_apply(edit->project,operation,created);
    const Fl_SceneState *state=NULL; const Fl_Entity *entity=focused_entity(edit,&state);
    const Fl_Scene *flat=state ? fl_composition_scene(state->composition) : NULL;
    return entity && fl_scope_timeline(edit->project,edit->scene,state->composition,(size_t)(entity-flat->entities),operation,created);
}
