#include "flow_test.h"
#include "fl_scene_edit.h"
#include "components/fl_transform.h"
#include "time/fl_composition.h"
#include "project/fl_prototype_authoring.h"
#include "components/fl_prototype_instance.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <string.h>
#include "core/fl_clock.h"
#include "core/fl_thread.h"

/* Borrow and decode an entity's current local pose and bounds. */
static Fl_TransformEdit read_transform(Fl_Project *p,Fl_Id scene,Fl_Id entity)
{
    const Fl_Entity *e=fl_scene_entity(fl_project_scene(p,scene),entity);
    Fl_TransformEdit t={.entity=entity};
    CHECK(fl_pose_decode(fl_entity_component(e,FL_COMPONENT_POSE),&t.pose));
    t.bounds=fl_entity_bounds(e);
    return t;
}
/* Compare transform results with a tolerance appropriate to affine composition. */
static bool near(double a,double b) {return fabs(a-b)<1e-8;}

/* A moving playhead cannot turn one drag into keys on every frame/channel. */
static void check_keyed_gesture(Fl_Project *p, Fl_SceneEdit *edit, Fl_Id scene)
{
    Fl_Id entity;
    CHECK(fl_widget_create(p, scene, 0, FL_WIDGET_RECTANGLE, 0, 0, &entity));
    CHECK(fl_scene_edit_select(edit, scene, entity, false));
    const Fl_AnimProperty properties[] = {FL_PROP_X, FL_PROP_Y, FL_PROP_WIDTH, FL_PROP_HEIGHT};
    const double initial[] = {0, 0, 320, 180};
    for (int property = 0; property < 4; ++property)
        CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = entity,
            .property = properties[property], .anchor = fl_anchor_at(fl_project_scene(p, scene), 0), .value = {initial[property]}}, NULL));
    fl_scene_edit_set_time(edit, FL_TICKS_PER_SECOND);
    CHECK(fl_scene_edit_begin(edit, FL_GESTURE_MOVE, 0, 0));
    CHECK(fl_scene_edit_update(edit, 10, 0));
    fl_scene_edit_set_time(edit, 2 * FL_TICKS_PER_SECOND);
    CHECK(fl_scene_edit_update(edit, 20, 0));
    fl_scene_edit_end(edit, false);
    const Fl_Scene *s = fl_project_scene(p, scene);
    const Fl_Track *x = fl_scene_track(s, entity, FL_PROP_X);
    CHECK(x->key_count == 2 && fl_anchor_resolve(s, &x->keys[1].at) == FL_TICKS_PER_SECOND && x->keys[1].value[0] == 20);
    CHECK(fl_scene_track(s, entity, FL_PROP_Y)->key_count == 1);
    CHECK(fl_scene_track(s, entity, FL_PROP_WIDTH)->key_count == 1);
    CHECK(fl_scene_track(s, entity, FL_PROP_HEIGHT)->key_count == 1);
}

/* Exercise parent-aware gestures, cancellation, multi-selection and atomic failure. */
/* Grid editing preserves group transforms, relative multi-selection offsets and aspect-independent selection. */
static void check_canvas_authoring(Fl_Project *p, Fl_SceneEdit *edit, Fl_Id scene)
{
    Fl_Id a,b;
    CHECK(fl_widget_create(p,scene,0,FL_WIDGET_RECTANGLE,13,27,&a));
    CHECK(fl_widget_create(p,scene,0,FL_WIDGET_RECTANGLE,33,57,&b));
    CHECK(fl_scene_edit_select(edit,scene,a,false));
    CHECK(fl_scene_edit_select(edit,scene,b,true));
    uint64_t revision=fl_project_scene(p,scene)->revision;
    fl_scene_edit_set_snap(edit,true);
    CHECK(fl_scene_edit_snap(edit));
    CHECK(fl_project_scene(p,scene)->revision==revision);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));
    CHECK(fl_scene_edit_update(edit,60,60));
    Fl_TransformEdit first=read_transform(p,scene,a),second=read_transform(p,scene,b);
    CHECK(near(first.pose.x,64)&&near(first.pose.y,64));
    CHECK(near(second.pose.x-first.pose.x,20)&&near(second.pose.y-first.pose.y,30));
    fl_scene_edit_end(edit,true);
    first=read_transform(p,scene,a);CHECK(near(first.pose.x,13)&&near(first.pose.y,27));
    CHECK(fl_scene_edit_insert(edit,FL_WIDGET_RECTANGLE,0,-35,98));
    Fl_Id inserted=fl_scene_edit_primary(edit);
    first=read_transform(p,scene,inserted);CHECK(near(first.pose.x,-64)&&near(first.pose.y,128));
    const Fl_Scene *s=fl_project_scene(p,scene);size_t count=s->entity_count;
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_ASPECT,.scene=scene,.aspect_width=9,.aspect_height=16},NULL));
    CHECK(fl_scene_edit_primary(edit)==inserted&&fl_scene_edit_count(edit)==1);
    CHECK(fl_project_scene(p,scene)->entity_count==count);
    second=read_transform(p,scene,inserted);
    CHECK(near(first.pose.x,second.pose.x)&&near(first.pose.y,second.pose.y));
    Fl_Id group;
    CHECK(fl_widget_create(p,scene,0,FL_WIDGET_ENTITY,0,0,&group));
    Fl_TransformEdit transform={.entity=group,.pose={.x=100,.y=50,.rz=.3,.sx=2,.sy=2,.sz=1},.bounds={320,180,0}};
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_TRANSFORMS,.scene=scene,.transforms=&transform,.transform_count=1},NULL));
    CHECK(fl_scene_edit_insert(edit,FL_WIDGET_RECTANGLE,group,19,31));
    const Fl_SceneGeometry *g=fl_scene_edit_geometry(edit);
    s=fl_project_scene(p,scene);
    const Fl_Entity *e=fl_scene_entity(s,fl_scene_edit_primary(edit));
    const Fl_ResolvedEntity *v=&g->entities[e-s->entities];
    CHECK(near(v->world.v[2]/64,round(v->world.v[2]/64)));
    CHECK(near(v->world.v[5]/64,round(v->world.v[5]/64)));
    fl_scene_edit_set_snap(edit,false);
}

/* Pump owner-thread completion events until the requested complete frame is published. */
static const Fl_SceneState *await_preview(Fl_SceneEdit *edit, Fl_EventBus *bus, Fl_Tick tick)
{
    uint64_t deadline=fl_clock_monotonic_ns()+UINT64_C(3000000000);
    const Fl_SceneState *state=NULL;
    Fl_Mutex mutex; Fl_CondVar condition;
    if (!fl_mutex_init(&mutex)) return NULL;
    if (!fl_cond_init(&condition)) {fl_mutex_destroy(&mutex); return NULL;}
    do {
        fl_event_bus_dispatch(bus);
        state=fl_scene_edit_state(edit);
        if (state && state->valid && state->tick==tick && !fl_scene_edit_evaluating(edit)) break;
        state=NULL;
        fl_mutex_lock(&mutex); fl_cond_wait_ms(&condition,&mutex,1); fl_mutex_unlock(&mutex);
    } while (fl_clock_monotonic_ns()<deadline);
    fl_cond_destroy(&condition); fl_mutex_destroy(&mutex);
    return state;
}

/* Native previews keep a complete front frame and cancel obsolete document generations before teardown. */
static void check_async_preview(Fl_Project *project, Fl_EventBus *bus, Fl_Id scene)
{
    Fl_JobSystem *jobs=fl_jobs_create(&(Fl_JobSystemDesc){.worker_count=1,.bus=bus}); CHECK(jobs);
    Fl_SceneEdit *edit=fl_scene_edit_create(project,bus,jobs); CHECK(edit);
    if (!edit) {fl_jobs_destroy(jobs); return;}
    Fl_Id entity=0, asset=0;
    CHECK(fl_widget_create(project,scene,0,FL_WIDGET_RECTANGLE,0,0,&entity));
    const char *source="return {step_hz=60,initialize=function(ctx) ctx.state={x=0} end,step=function(ctx) ctx.state.x=ctx.state.x+100*ctx.time.dt end,sample=function(ctx) ctx.entity.pose.x=ctx.state.x end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Async Preview",.source=source,.source_size=strlen(source)},&asset));
    Fl_Component component={0}; CHECK(fl_script_encode(&(Fl_Script){.script=asset,.enabled=true},&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component},NULL));
    fl_component_clear(&component);
    CHECK(fl_scene_edit_select(edit,scene,entity,false)); fl_scene_edit_set_time(edit,FL_TICKS_PER_SECOND);
    CHECK(!fl_scene_edit_state(edit) && fl_scene_edit_evaluating(edit));
    const Fl_SceneState *state=await_preview(edit,bus,FL_TICKS_PER_SECOND);
    const Fl_Scene *document=fl_project_scene(project,scene);
    size_t index=(size_t)(fl_scene_entity(document,entity)-document->entities);
    CHECK(state && near(state->entities[index].pose.x,100));
    fl_scene_edit_set_snap(edit,false);
    CHECK(!fl_scene_edit_begin(edit,FL_GESTURE_MOVE,100,0));
    state=await_preview(edit,bus,FL_TICKS_PER_SECOND); CHECK(state);
    fl_scene_edit_set_time(edit,2*FL_TICKS_PER_SECOND);
    state=fl_scene_edit_state(edit);
    CHECK(state && state->tick==FL_TICKS_PER_SECOND && fl_scene_edit_evaluating(edit));
    source="return {sample=function(ctx) ctx.entity.pose.x=ctx.time.seconds*1000 end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.asset =asset,.source=source,.source_size=strlen(source)},NULL));
    CHECK(!fl_scene_edit_state(edit));
    state=await_preview(edit,bus,2*FL_TICKS_PER_SECOND);
    CHECK(state && state->entities[index].pose.x==2000);
    fl_scene_edit_set_time(edit,3*FL_TICKS_PER_SECOND); fl_scene_edit_state(edit);
    fl_scene_edit_destroy(edit); CHECK(fl_jobs_destroy(jobs)); fl_event_bus_dispatch(bus);
}

/* Generated children remain independently pickable, draggable and keyable in their local clock. */
static void check_scoped_editing(Fl_Project *project, Fl_SceneEdit *edit, Fl_Id scene)
{
    Fl_Id shape=0,prototype=0,owner=0;
    CHECK(fl_widget_create(project,scene,0,FL_WIDGET_RECTANGLE,10,20,&shape));
    CHECK(fl_prototype_capture(project,scene,shape,"Scoped gesture",&prototype));
    CHECK(fl_prototype_instantiate(project,scene,0,prototype,500,500,&owner));
    CHECK(fl_scene_edit_select(edit,scene,owner,false)); fl_scene_edit_set_time(edit,0);
    const Fl_SceneState *state=fl_scene_edit_state(edit); const Fl_Scene *flat=fl_scene_state_scene(state,fl_project_scene(project,scene));
    Fl_Id generated=0;
    for (size_t i=0;i<flat->entity_count;++i) {const Fl_Origin *origin=fl_composition_origin(state->composition,i); if (origin && origin->generated && origin->owner==owner) generated=flat->entities[i].id;}
    CHECK(generated); CHECK(fl_scene_edit_select(edit,scene,generated,false));
    CHECK(fl_scene_edit_primary(edit)==owner && fl_scene_edit_focus(edit)==generated);
    CHECK(fl_scene_edit_contains(edit,generated) && !fl_scene_edit_contains(edit,owner));
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0)); CHECK(fl_scene_edit_update(edit,15,25)); fl_scene_edit_end(edit,false);
    state=fl_scene_edit_state(edit); flat=fl_scene_state_scene(state,NULL); const Fl_Entity *entity=fl_scene_entity(flat,generated); CHECK(entity);
    if (entity) CHECK(state->entities[entity-flat->entities].pose.x==15 && state->entities[entity-flat->entities].pose.y==25);
    Fl_Id key=0; CHECK(fl_scene_edit_timeline_apply(edit,&(Fl_Op){.kind=FL_OP_KEY_SET,.entity=generated,.property=FL_PROP_Y,.anchor={.kind=FL_ANCHOR_ABSOLUTE,.offset=0},.value={25}},&key)); CHECK(key>>63);
    fl_scene_edit_set_time(edit,12000);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));
    for (unsigned i=0;i<80;++i) CHECK(fl_scene_edit_update(edit,0,(double)(i+1)/8));
    fl_scene_edit_end(edit,false);
    const Fl_Scene *clock=fl_scene_edit_timeline(edit); const Fl_Track *track=fl_scene_track(clock,generated,FL_PROP_Y);
    CHECK(track && track->key_count==2 && track->keys[1].value[0]==35);
    Fl_PrototypeInstance instance={0}; CHECK(fl_prototype_instance_decode(fl_entity_component(fl_scene_entity(fl_project_scene(project,scene),owner),FL_COMPONENT_PROTOTYPE),&instance));
    Fl_Json context={0}; cJSON *operations=fl_json_parse(&context,instance.overrides,instance.overrides_size);
    CHECK(operations && cJSON_GetArraySize(operations)<12); cJSON_Delete(operations);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0)); CHECK(fl_scene_edit_update(edit,100,100)); fl_scene_edit_end(edit,true);
    state=fl_scene_edit_state(edit); flat=fl_scene_state_scene(state,NULL); entity=fl_scene_entity(flat,generated);
    CHECK(entity && state->entities[entity-flat->entities].pose.y==35);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene,.entity=owner},NULL));
    CHECK(fl_scene_edit_count(edit)==0);
}

int main(int argc,char **argv)
{
    CHECK(argc==2);if (argc!=2) return 1;
    char path[FL_PATH_MAX];CHECK(fl_fs_mkdir_p(argv[1]));CHECK(fl_fs_join(path,sizeof(path),argv[1],"project.flow.json"));fl_fs_remove(path);
    Fl_EventBus *bus=fl_event_bus_create(NULL);Fl_Project *p=fl_project_create(argv[1],"Editing",bus);CHECK(p);if (!p) return 1;
    Fl_Id scene=fl_project_scene_at(p,0)->id,parent,child;
    CHECK(fl_widget_create(p,scene,0,FL_WIDGET_ENTITY,100,100,&parent));
    CHECK(fl_widget_create(p,scene,parent,FL_WIDGET_RECTANGLE,10,20,&child));
    Fl_TransformEdit parent_transform={.entity=parent,.pose={.x=100,.y=100,.rz=1.5707963267948966,.sx=2,.sy=2,.sz=1},.bounds={320,180,0}};
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_TRANSFORMS,.scene=scene,.transforms=&parent_transform,.transform_count=1},NULL));
    Fl_SceneEdit *edit=fl_scene_edit_create(p,bus,NULL);CHECK(edit);
    CHECK(fl_scene_edit_select(edit,scene,child,false));
    uint64_t revision=fl_project_scene(p,scene)->revision;
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));CHECK(fl_scene_edit_update(edit,0,0));fl_scene_edit_end(edit,true);
    CHECK(fl_project_scene(p,scene)->revision==revision);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));CHECK(fl_scene_edit_update(edit,20,40));
    Fl_TransformEdit t=read_transform(p,scene,child);CHECK(near(t.pose.x,30)&&near(t.pose.y,10));
    fl_scene_edit_end(edit,true);t=read_transform(p,scene,child);CHECK(near(t.pose.x,10)&&near(t.pose.y,20));
    CHECK(fl_scene_edit_select(edit,scene,parent,true));CHECK(fl_scene_edit_count(edit)==2);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));CHECK(fl_scene_edit_update(edit,50,60));fl_scene_edit_end(edit,false);
    t=read_transform(p,scene,parent);CHECK(near(t.pose.x,150)&&near(t.pose.y,160));
    t=read_transform(p,scene,child);CHECK(near(t.pose.x,10)&&near(t.pose.y,20));
    CHECK(!fl_scene_edit_begin(edit,FL_GESTURE_ROTATE,0,0));
    CHECK(fl_scene_edit_select(edit,scene,parent,false));
    Fl_TransformEdit before=read_transform(p,scene,parent);Fl_Affine old=fl_affine_pose(&before.pose);
    double right_x,right_y,start_x,start_y;fl_affine_point(old,before.bounds.width,before.bounds.height,&right_x,&right_y);fl_affine_point(old,0,0,&start_x,&start_y);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_NW,start_x,start_y));CHECK(fl_scene_edit_update(edit,start_x+20,start_y+40));fl_scene_edit_end(edit,false);
    t=read_transform(p,scene,parent);double x,y;fl_affine_point(fl_affine_pose(&t.pose),t.bounds.width,t.bounds.height,&x,&y);CHECK(near(x,right_x)&&near(y,right_y));
    double cx,cy;fl_affine_point(fl_affine_pose(&t.pose),t.bounds.width*.5,t.bounds.height*.5,&cx,&cy);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_ROTATE,cx+100,cy));CHECK(fl_scene_edit_update(edit,cx,cy+100));fl_scene_edit_end(edit,false);
    t=read_transform(p,scene,parent);fl_affine_point(fl_affine_pose(&t.pose),t.bounds.width*.5,t.bounds.height*.5,&x,&y);CHECK(near(x,cx)&&near(y,cy));
    before=t;
    Fl_TransformEdit batch[2]={{.entity=parent,.pose={.sx=1,.sy=1,.sz=1},.bounds={50,50,0}},{.entity=999999,.pose={.sx=1,.sy=1,.sz=1},.bounds={10,10,0}}};
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_TRANSFORMS,.scene=scene,.transforms=batch,.transform_count=2},NULL));
    t=read_transform(p,scene,parent);CHECK(t.pose.x==before.pose.x&&t.bounds.width==before.bounds.width);
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene,.entity=parent},NULL));CHECK(fl_scene_edit_count(edit)==0);
    CHECK(fl_scene_edit_insert(edit,FL_WIDGET_TEXT,0,100,200));CHECK(fl_scene_edit_count(edit)==1);
    check_keyed_gesture(p,edit,scene);
    check_canvas_authoring(p,edit,scene);
    Fl_Id box;
    CHECK(fl_widget_create(p,scene,0,FL_WIDGET_RECTANGLE,10,20,&box));
    CHECK(fl_scene_edit_select(edit,scene,box,false));
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_AXIS_X,0,0));
    CHECK(fl_scene_edit_update_spatial(edit,(Fl_Vec3){40,0,0},(Fl_Vec3){1,0,0},0));
    t=read_transform(p,scene,box);CHECK(near(t.pose.x,50)&&near(t.pose.y,20)&&near(t.pose.z,0));
    fl_scene_edit_end(edit,false);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_AXIS_Z,0,0));
    CHECK(fl_scene_edit_update_spatial(edit,(Fl_Vec3){0,0,30},(Fl_Vec3){0,0,1},0));
    t=read_transform(p,scene,box);CHECK(near(t.pose.z,30));
    fl_scene_edit_end(edit,false);
    before=read_transform(p,scene,box);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_RING_Z,0,0));
    CHECK(fl_scene_edit_update_spatial(edit,(Fl_Vec3){0,0,0},(Fl_Vec3){0,0,1},1.5707963267948966));
    t=read_transform(p,scene,box);
    Fl_Vec3 center=fl_mat4_point(fl_pose_model(&t.pose),(Fl_Vec3){t.bounds.width*.5,t.bounds.height*.5,0});
    Fl_Vec3 held=fl_mat4_point(fl_pose_model(&before.pose),(Fl_Vec3){before.bounds.width*.5,before.bounds.height*.5,0});
    CHECK(near(center.x,held.x)&&near(center.y,held.y)&&near(center.z,held.z));
    fl_scene_edit_end(edit,true);
    check_async_preview(p,bus,scene);
    check_scoped_editing(p,edit,scene);
    CHECK(fl_project_save(p));fl_scene_edit_destroy(edit);fl_project_destroy(p);CHECK(fl_event_bus_destroy(bus));fl_fs_remove(path);
    return FLOW_TEST_RESULT("fl_scene_edit_tests");
}
