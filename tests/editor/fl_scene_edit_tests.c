#include "flow_test.h"
#include "fl_scene_edit.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>

/* Borrow and decode an entity's current local transform. */
static Fl_Transform read_transform(Fl_Project *p,Fl_Id scene,Fl_Id entity)
{
    Fl_Transform t={0};CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(fl_project_scene(p,scene),entity),FL_COMPONENT_TRANSFORM),&t));return t;
}
/* Compare transform results with a tolerance appropriate to affine composition. */
static bool near(double a,double b) {return fabs(a-b)<1e-8;}

/* A moving playhead cannot turn one drag into keys on every frame/channel. */
static void check_keyed_gesture(Fl_Project *p, Fl_SceneEdit *edit, Fl_Id scene)
{
    Fl_Id entity;
    CHECK(fl_widget_create(p, scene, 0, FL_WIDGET_RECTANGLE, 0, 0, &entity));
    CHECK(fl_scene_edit_select(edit, scene, entity, false));
    const double initial[] = {0, 0, 320, 180};
    for (int property = 0; property < 4; ++property)
        CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = entity,
            .property = (Fl_AnimProperty)property, .anchor = fl_anchor_at(fl_project_scene(p, scene), 0), .value = {initial[property]}}, NULL));
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
    Fl_Transform first=read_transform(p,scene,a),second=read_transform(p,scene,b);
    CHECK(near(first.x,64)&&near(first.y,64));
    CHECK(near(second.x-first.x,20)&&near(second.y-first.y,30));
    fl_scene_edit_end(edit,true);
    first=read_transform(p,scene,a);CHECK(near(first.x,13)&&near(first.y,27));
    CHECK(fl_scene_edit_insert(edit,FL_WIDGET_RECTANGLE,0,-35,98));
    Fl_Id inserted=fl_scene_edit_primary(edit);
    first=read_transform(p,scene,inserted);CHECK(near(first.x,-64)&&near(first.y,128));
    const Fl_Scene *s=fl_project_scene(p,scene);size_t count=s->entity_count;
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_ASPECT,.scene=scene,.aspect_width=9,.aspect_height=16},NULL));
    CHECK(fl_scene_edit_primary(edit)==inserted&&fl_scene_edit_count(edit)==1);
    CHECK(fl_project_scene(p,scene)->entity_count==count);
    second=read_transform(p,scene,inserted);
    CHECK(near(first.x,second.x)&&near(first.y,second.y));
    Fl_Id group;
    CHECK(fl_widget_create(p,scene,0,FL_WIDGET_GROUP,0,0,&group));
    Fl_TransformEdit transform={group,{100,50,320,180,.3,2,2}};
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

int main(int argc,char **argv)
{
    CHECK(argc==2);if (argc!=2) return 1;
    char path[FL_PATH_MAX];CHECK(fl_fs_mkdir_p(argv[1]));CHECK(fl_fs_join(path,sizeof(path),argv[1],"project.flow"));fl_fs_remove(path);
    Fl_EventBus *bus=fl_event_bus_create(NULL);Fl_Project *p=fl_project_create(argv[1],"Editing",bus);CHECK(p);if (!p) return 1;
    Fl_Id scene=fl_project_scene_at(p,0)->id,parent,child;
    CHECK(fl_widget_create(p,scene,0,FL_WIDGET_GROUP,100,100,&parent));
    CHECK(fl_widget_create(p,scene,parent,FL_WIDGET_RECTANGLE,10,20,&child));
    Fl_TransformEdit parent_transform={parent,{100,100,320,180,1.5707963267948966,2,2}};
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_TRANSFORMS,.scene=scene,.transforms=&parent_transform,.transform_count=1},NULL));
    Fl_SceneEdit *edit=fl_scene_edit_create(p,bus);CHECK(edit);
    CHECK(fl_scene_edit_select(edit,scene,child,false));
    uint64_t revision=fl_project_scene(p,scene)->revision;
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));CHECK(fl_scene_edit_update(edit,0,0));fl_scene_edit_end(edit,true);
    CHECK(fl_project_scene(p,scene)->revision==revision);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));CHECK(fl_scene_edit_update(edit,20,40));
    Fl_Transform t=read_transform(p,scene,child);CHECK(near(t.x,30)&&near(t.y,10));
    fl_scene_edit_end(edit,true);t=read_transform(p,scene,child);CHECK(near(t.x,10)&&near(t.y,20));
    CHECK(fl_scene_edit_select(edit,scene,parent,true));CHECK(fl_scene_edit_count(edit)==2);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_MOVE,0,0));CHECK(fl_scene_edit_update(edit,50,60));fl_scene_edit_end(edit,false);
    t=read_transform(p,scene,parent);CHECK(near(t.x,150)&&near(t.y,160));
    t=read_transform(p,scene,child);CHECK(near(t.x,10)&&near(t.y,20));
    CHECK(!fl_scene_edit_begin(edit,FL_GESTURE_ROTATE,0,0));
    CHECK(fl_scene_edit_select(edit,scene,parent,false));
    Fl_Transform before=read_transform(p,scene,parent);Fl_Affine old=fl_affine_transform(&before);
    double right_x,right_y,start_x,start_y;fl_affine_point(old,before.width,before.height,&right_x,&right_y);fl_affine_point(old,0,0,&start_x,&start_y);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_NW,start_x,start_y));CHECK(fl_scene_edit_update(edit,start_x+20,start_y+40));fl_scene_edit_end(edit,false);
    t=read_transform(p,scene,parent);double x,y;fl_affine_point(fl_affine_transform(&t),t.width,t.height,&x,&y);CHECK(near(x,right_x)&&near(y,right_y));
    double cx,cy;fl_affine_point(fl_affine_transform(&t),t.width*.5,t.height*.5,&cx,&cy);
    CHECK(fl_scene_edit_begin(edit,FL_GESTURE_ROTATE,cx+100,cy));CHECK(fl_scene_edit_update(edit,cx,cy+100));fl_scene_edit_end(edit,false);
    t=read_transform(p,scene,parent);fl_affine_point(fl_affine_transform(&t),t.width*.5,t.height*.5,&x,&y);CHECK(near(x,cx)&&near(y,cy));
    before=t;
    Fl_TransformEdit batch[2]={{parent,{0,0,50,50,0,1,1}},{999999,{0,0,10,10,0,1,1}}};
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_TRANSFORMS,.scene=scene,.transforms=batch,.transform_count=2},NULL));
    t=read_transform(p,scene,parent);CHECK(t.x==before.x&&t.width==before.width);
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene,.entity=parent},NULL));CHECK(fl_scene_edit_count(edit)==0);
    CHECK(fl_scene_edit_insert(edit,FL_WIDGET_TEXT,0,100,200));CHECK(fl_scene_edit_count(edit)==1);
    check_keyed_gesture(p,edit,scene);
    check_canvas_authoring(p,edit,scene);
    CHECK(fl_project_save(p));fl_scene_edit_destroy(edit);fl_project_destroy(p);CHECK(fl_event_bus_destroy(bus));fl_fs_remove(path);
    return FLOW_TEST_RESULT("fl_scene_edit_tests");
}
