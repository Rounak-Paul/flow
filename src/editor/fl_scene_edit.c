#include "fl_scene_edit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_GestureTarget {
    Fl_TransformEdit initial;
    Fl_Transform applied;       /* last value written by this gesture */
    Fl_Affine inverse, parent_inverse;
} Fl_GestureTarget;
struct Fl_SceneEdit {
    Fl_Project *project;
    Fl_EventBus *bus;
    Fl_EventType selection_event, time_event;
    Fl_Id scene;
    Fl_Tick time;
    Fl_SceneState state;
    Fl_Id *selection;
    size_t count;
    Fl_SceneGeometry geometry;
    Fl_GestureTarget *targets;
    Fl_TransformEdit *updates;
    size_t target_count;
    Fl_EditGesture gesture;
    double start_x, start_y;
    bool applying;
};

/* Publish a time change. */
static void notify_time(Fl_SceneEdit *edit)
{
    Fl_TimeChange change = {edit, edit->time};
    fl_event_publish(edit->bus, edit->time_event, &change, sizeof(change));
}

/* Publish selection changes after ids have been fully updated. */
static void notify(Fl_SceneEdit *edit)
{
    Fl_SelectionChange change = {edit};
    fl_event_publish(edit->bus, edit->selection_event, &change, sizeof(change));
}

/* Discard gesture snapshots without changing document state. */
static void clear_gesture(Fl_SceneEdit *edit)
{
    free(edit->targets); free(edit->updates);
    edit->targets = NULL; edit->updates = NULL; edit->target_count = 0;
}

/* Reconcile selection with document deletion; external mutations invalidate active gestures. */
static bool document_changed(const Fl_Event *event, void *data)
{
    Fl_SceneEdit *edit = data; const Fl_ProjectChange *change = event->payload;
    if (change->project != edit->project) return false;
    if (!edit->applying) clear_gesture(edit);
    const Fl_Scene *scene = fl_project_scene(edit->project, edit->scene);
    if (!scene) {
        scene = fl_project_scene_at(edit->project, 0); edit->scene = scene ? scene->id : 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < edit->count; ++i)
        if (fl_scene_entity(scene, edit->selection[i])) edit->selection[count++] = edit->selection[i];
    if (count != edit->count) { edit->count = count; notify(edit); }
    Fl_Tick length = fl_scene_length(scene);
    if (edit->time > length) { edit->time = length; notify_time(edit); }
    return false;
}

Fl_SceneEdit *fl_scene_edit_create(Fl_Project *project, Fl_EventBus *bus)
{
    if (!project || !bus) return NULL;
    Fl_SceneEdit *edit = calloc(1, sizeof(*edit)); if (!edit) return NULL;
    edit->project = project; edit->bus = bus;
    const Fl_Scene *scene = fl_project_scene_at(project, 0); edit->scene = scene ? scene->id : 0;
    edit->selection_event = fl_event_type_register(bus, FL_EVENT_SELECTION_CHANGED, sizeof(Fl_SelectionChange));
    edit->time_event = fl_event_type_register(bus, FL_EVENT_TIME_CHANGED, sizeof(Fl_TimeChange));
    Fl_EventType changed = fl_event_type_register(bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange));
    if (edit->selection_event == FL_EVENT_TYPE_INVALID || edit->time_event == FL_EVENT_TYPE_INVALID || changed == FL_EVENT_TYPE_INVALID ||
        !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = changed, .fn = document_changed, .user_data = edit, .owner = edit}).generation) {
        free(edit); return NULL;
    }
    return edit;
}
void fl_scene_edit_destroy(Fl_SceneEdit *edit)
{
    if (!edit) return;
    fl_event_unsubscribe_owner(edit->bus, edit); clear_gesture(edit);
    fl_scene_geometry_clear(&edit->geometry); fl_scene_state_clear(&edit->state); free(edit->selection); free(edit);
}
Fl_Project *fl_scene_edit_project(const Fl_SceneEdit *edit) { return edit ? edit->project : NULL; }
Fl_Id fl_scene_edit_scene(const Fl_SceneEdit *edit) { return edit ? edit->scene : 0; }
Fl_Id fl_scene_edit_primary(const Fl_SceneEdit *edit) { return edit && edit->count ? edit->selection[edit->count-1] : 0; }
size_t fl_scene_edit_count(const Fl_SceneEdit *edit) { return edit ? edit->count : 0; }
bool fl_scene_edit_contains(const Fl_SceneEdit *edit, Fl_Id id)
{
    if (edit) for (size_t i = 0; i < edit->count; ++i) if (edit->selection[i] == id) return true;
    return false;
}

bool fl_scene_edit_select(Fl_SceneEdit *edit, Fl_Id scene_id, Fl_Id id, bool additive)
{
    if (!edit) return false;
    const Fl_Scene *scene = fl_project_scene(edit->project, scene_id);
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
    clear_gesture(edit); edit->scene = scene_id; edit->count = count; notify(edit);
    if (!same_scene && edit->time) { edit->time = 0; notify_time(edit); }
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
    notify_time(edit);
}
const Fl_SceneState *fl_scene_edit_state(Fl_SceneEdit *edit)
{
    if (!edit || !fl_scene_state_update(&edit->state, fl_project_scene(edit->project, edit->scene), edit->time)) return NULL;
    return &edit->state;
}
const Fl_SceneGeometry *fl_scene_edit_geometry(Fl_SceneEdit *edit)
{
    const Fl_SceneState *state = fl_scene_edit_state(edit);
    if (!edit || !fl_scene_geometry_update(&edit->geometry, fl_project_scene(edit->project, edit->scene), state)) return NULL;
    return &edit->geometry;
}

bool fl_scene_edit_begin(Fl_SceneEdit *edit, Fl_EditGesture mode, double x, double y)
{
    if (!edit || !edit->count || mode < FL_GESTURE_MOVE || mode > FL_GESTURE_ROTATE ||
        (mode != FL_GESTURE_MOVE && edit->count != 1) || !isfinite(x) || !isfinite(y)) return false;
    clear_gesture(edit);
    const Fl_Scene *scene = fl_project_scene(edit->project, edit->scene);
    const Fl_SceneGeometry *geometry = fl_scene_edit_geometry(edit);
    if (!scene || !geometry || edit->count > SIZE_MAX / sizeof(Fl_GestureTarget)) return false;
    edit->targets = calloc(edit->count, sizeof(*edit->targets));
    edit->updates = calloc(edit->count, sizeof(*edit->updates));
    if (!edit->targets || !edit->updates) { clear_gesture(edit); return false; }
    for (size_t i = 0; i < edit->count; ++i) {
        const Fl_Entity *entity = fl_scene_entity(scene, edit->selection[i]);
        const Fl_Entity *parent = fl_scene_entity(scene, entity->parent);
        const Fl_Entity *ancestor = parent; bool inherited = false;
        while (ancestor) { inherited |= fl_scene_edit_contains(edit, ancestor->id); ancestor = fl_scene_entity(scene, ancestor->parent); }
        if (inherited) continue;
        const Fl_ResolvedEntity *v = &geometry->entities[entity-scene->entities];
        Fl_GestureTarget *target = &edit->targets[edit->target_count];
        target->initial = (Fl_TransformEdit){entity->id,v->local}; target->applied = v->local; target->inverse = v->inverse;
        if (!v->invertible || !fl_affine_inverse(parent ? geometry->entities[parent-scene->entities].world : fl_affine_identity(), &target->parent_inverse)) {
            clear_gesture(edit); return false;
        }
        ++edit->target_count;
    }
    edit->gesture = mode; edit->start_x = x; edit->start_y = y;
    return edit->target_count != 0;
}

bool fl_scene_edit_update(Fl_SceneEdit *edit, double x, double y)
{
    if (!edit || !edit->target_count || !isfinite(x) || !isfinite(y)) return false;
    for (size_t i = 0; i < edit->target_count; ++i) {
        const Fl_GestureTarget *target = &edit->targets[i];
        Fl_Transform t = target->initial.transform;
        if (x==edit->start_x&&y==edit->start_y) {
            edit->updates[i]=target->initial;continue;
        }
        if (edit->gesture == FL_GESTURE_MOVE) {
            double dx = x-edit->start_x, dy = y-edit->start_y;
            t.x += target->parent_inverse.v[0]*dx+target->parent_inverse.v[1]*dy;
            t.y += target->parent_inverse.v[3]*dx+target->parent_inverse.v[4]*dy;
        } else if (edit->gesture == FL_GESTURE_ROTATE) {
            double px,py,sx,sy,cx,cy;
            fl_affine_point(target->parent_inverse,x,y,&px,&py);
            fl_affine_point(target->parent_inverse,edit->start_x,edit->start_y,&sx,&sy);
            Fl_Affine local = fl_affine_transform(&t); fl_affine_point(local,t.width*.5,t.height*.5,&cx,&cy);
            t.rotation += atan2(py-cy,px-cx)-atan2(sy-cy,sx-cx);
            double c=cos(t.rotation),s=sin(t.rotation);
            t.x = cx-c*t.scale_x*t.width*.5+s*t.scale_y*t.height*.5;
            t.y = cy-s*t.scale_x*t.width*.5-c*t.scale_y*t.height*.5;
        } else {
            double lx,ly,sx,sy;
            fl_affine_point(target->inverse,x,y,&lx,&ly);
            fl_affine_point(target->inverse,edit->start_x,edit->start_y,&sx,&sy);
            bool west = edit->gesture == FL_GESTURE_NW || edit->gesture == FL_GESTURE_SW;
            bool north = edit->gesture == FL_GESTURE_NW || edit->gesture == FL_GESTURE_NE;
            double w=fmax(1,t.width+(west ? sx-lx : lx-sx));
            double h=fmax(1,t.height+(north ? sy-ly : ly-sy));
            Fl_Affine local=fl_affine_transform(&t);
            double dx=west ? t.width-w : 0,dy=north ? t.height-h : 0;
            t.x += local.v[0]*dx+local.v[1]*dy; t.y += local.v[3]*dx+local.v[4]*dy;
            t.width=w; t.height=h;
        }
        edit->updates[i]=(Fl_TransformEdit){target->initial.entity,t};
    }
    bool changed=false;
    for (size_t i=0;i<edit->target_count;++i)
        changed|=memcmp(&edit->targets[i].applied,&edit->updates[i].transform,sizeof(Fl_Transform))!=0;
    if (!changed) return true;
    edit->applying = true;
    bool ok = fl_project_apply(edit->project, &(Fl_Op){.kind=FL_OP_TRANSFORMS,.scene=edit->scene,
        .transforms=edit->updates,.transform_count=edit->target_count,.keyed=true,.tick=edit->time},NULL);
    edit->applying = false;
    if (ok) for (size_t i=0;i<edit->target_count;++i) edit->targets[i].applied=edit->updates[i].transform;
    return ok;
}
void fl_scene_edit_end(Fl_SceneEdit *edit, bool cancel)
{
    if (!edit) return;
    if (cancel&&edit->target_count) fl_scene_edit_update(edit,edit->start_x,edit->start_y);
    clear_gesture(edit);
}
bool fl_scene_edit_insert(Fl_SceneEdit *edit, Fl_WidgetKind kind, Fl_Id parent, double x, double y)
{
    Fl_Id id;
    return edit && fl_widget_create(edit->project,edit->scene,parent,kind,x,y,&id) &&
        fl_scene_edit_select(edit,edit->scene,id,false);
}

bool fl_scene_edit_insert_image(Fl_SceneEdit *edit, Fl_Id asset, Fl_Id parent, double x, double y, bool centred)
{
    const Fl_Scene *scene=edit ? fl_project_scene(edit->project,edit->scene) : NULL;
    const Fl_Asset *record=edit ? fl_project_asset(edit->project,asset) : NULL;
    if (!scene||!record||record->kind!=FL_ASSET_IMAGE||!isfinite(x)||!isfinite(y)) return false;
    double w=record->width ? (double)record->width : 320,h=record->height ? (double)record->height : 180;
    double fit=fmin(1.0,fmin(scene->width/w,scene->height/h));
    w*=fit;h*=fit;
    Fl_Id id;
    if (centred) {x-=w*.5;y-=h*.5;}
    return fl_widget_create_image(edit->project,edit->scene,parent,asset,x,y,w,h,&id)&&
        fl_scene_edit_select(edit,edit->scene,id,false);
}
