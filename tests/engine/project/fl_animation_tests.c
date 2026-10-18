#include "flow_test.h"
#include "components/fl_animation.h"
#include "components/fl_script.h"
#include "time/fl_scene_state.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define SECOND FL_TICKS_PER_SECOND

/* Create a named motion state through a validated document operation. */
static Fl_Id add_state(Fl_Project *p,Fl_Id scene,Fl_Id entity,const char *name,bool loop)
{
    Fl_Id id=0;Fl_AnimationState state={.name=name,.duration=SECOND,.loop=loop};
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_STATE_SET,.scene=scene,.entity=entity,.animation_state_record=&state},&id));return id;
}

/* Author one local-time key without changing scene beat timing. */
static Fl_Id key(Fl_Project *p,Fl_Id scene,Fl_Id entity,Fl_Id state,Fl_AnimProperty property,Fl_Tick at,double value)
{
    Fl_Id id=0;CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene,.entity=entity,.animation_state=state,
        .property=property,.anchor={.offset=at},.value={value},.ease=FL_EASE_LINEAR},&id));return id;
}

/* Attach the behavior source independently of the animation graph's keyframed motion. */
static void source(Fl_Project *p,Fl_Id scene,Fl_Id entity,const char *text)
{
    Fl_Component script={0};CHECK(fl_script_encode(&(Fl_Script){text,strlen(text),true},&script));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=script},NULL));free((void *)script.data);
}

/* Check active state and combined base movement/animation pose at an exact document time. */
static void frame(Fl_SceneState *state,const Fl_Scene *scene,Fl_Tick tick,Fl_Id active,double x,double y)
{
    CHECK(fl_scene_state_update(state,scene,tick));
    const Fl_AnimationPlayer *player=fl_script_runtime_player(state->scripts,0);
    CHECK(player&&player->state==active);
    CHECK(fabs(state->entities[0].transform.x-x)<1e-8);
    CHECK(fabs(state->entities[0].transform.y-y)<1e-8);
}

/* Migrate scene-owned tracks and missing transforms without losing stable IDs or key timing. */
static void legacy_project(const char *root)
{
    char directory[FL_PATH_MAX],folder[FL_PATH_MAX],path[FL_PATH_MAX];
    CHECK(fl_fs_join(directory,sizeof(directory),root,"legacy"));CHECK(fl_fs_join(folder,sizeof(folder),directory,"scenes"));CHECK(fl_fs_mkdir_p(folder));
    Fl_Binary b={0};fl_binary_string(&b,"Legacy");fl_binary_u64(&b,6);fl_binary_u64(&b,1);fl_binary_u64(&b,1);fl_binary_u64(&b,1);
    CHECK(fl_fs_join(path,sizeof(path),directory,"project.flow"));CHECK(fl_binary_save(&b,path,"FLPROJ\0\0",1));free(b.data);b=(Fl_Binary){0};
    fl_binary_u64(&b,1);fl_binary_u64(&b,1);fl_binary_string(&b,"Scene");fl_binary_u64(&b,1920);fl_binary_u64(&b,1080);fl_binary_u64(&b,SECOND);
    fl_binary_u64(&b,1);fl_binary_u64(&b,3);fl_binary_u64(&b,0);fl_binary_string(&b,"Box");fl_binary_u64(&b,1);fl_binary_u64(&b,0);
    fl_binary_u64(&b,1);fl_binary_u64(&b,2);fl_binary_string(&b,"Beat");fl_binary_u64(&b,SECOND);fl_binary_u64(&b,0);
    fl_binary_u64(&b,1);fl_binary_u64(&b,4);fl_binary_u64(&b,3);fl_binary_u64(&b,FL_PROP_X);fl_binary_u64(&b,1);
    fl_binary_u64(&b,5);fl_binary_u64(&b,FL_ANCHOR_BEAT);fl_binary_u64(&b,2);fl_binary_u64(&b,0);fl_binary_u64(&b,SECOND/2);
    double value=75;uint64_t bits;memcpy(&bits,&value,8);fl_binary_u64(&b,bits);for (int i=0;i<3;++i) fl_binary_u64(&b,0);
    fl_binary_u64(&b,FL_EASE_LINEAR);fl_binary_u64(&b,0);
    CHECK(fl_fs_join(path,sizeof(path),folder,"0000000000000001-0000000000000001.flscene"));CHECK(fl_binary_save(&b,path,"FLSCENE\0",2));free(b.data);
    Fl_Project *p=fl_project_open(directory,NULL);CHECK(p);if (!p) return;
    const Fl_Scene *scene=fl_project_scene(p,1);const Fl_Entity *entity=fl_scene_entity(scene,3);
    CHECK(entity&&entity->components[0].type==FL_COMPONENT_TRANSFORM&&fl_project_dirty(p));
    const Fl_Animation *animation=fl_entity_animation(entity);CHECK(animation&&animation->timeline.track_count==1);
    CHECK(animation&&animation->timeline.tracks[0].id==4&&animation->timeline.tracks[0].keys[0].id==5);
    CHECK(fl_project_save(p));fl_project_destroy(p);p=fl_project_open(directory,NULL);CHECK(p);
    if (p) {CHECK(!fl_project_dirty(p));Fl_SceneState state={0};CHECK(fl_scene_state_update(&state,fl_project_scene(p,1),SECOND/2));CHECK(state.entities[0].transform.x==75);fl_scene_state_clear(&state);fl_project_destroy(p);}
}

/* Validate graph ownership, script-controlled transitions, blending, replay and binary persistence. */
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    char path[FL_PATH_MAX];CHECK(fl_fs_mkdir_p(argv[1]));CHECK(fl_fs_join(path,sizeof(path),argv[1],"project.flow"));fl_fs_remove(path);
    Fl_Project *p=fl_project_create(argv[1],"Animation states",NULL);CHECK(p);if (!p) return 1;
    Fl_Id scene_id=fl_project_scene_at(p,0)->id,entity;
    CHECK(fl_widget_create(p,scene_id,0,FL_WIDGET_RECTANGLE,100,20,&entity));
    Fl_Id idle=add_state(p,scene_id,entity,"Idle",true),run=add_state(p,scene_id,entity,"Run",true),jump=add_state(p,scene_id,entity,"Jump",false);
    key(p,scene_id,entity,idle,FL_PROP_Y,0,20);
    key(p,scene_id,entity,run,FL_PROP_Y,0,20);key(p,scene_id,entity,run,FL_PROP_Y,SECOND,40);
    key(p,scene_id,entity,run,FL_PROP_X,0,100);key(p,scene_id,entity,run,FL_PROP_X,SECOND,110);
    key(p,scene_id,entity,jump,FL_PROP_Y,0,20);key(p,scene_id,entity,jump,FL_PROP_Y,SECOND/2,-80);key(p,scene_id,entity,jump,FL_PROP_Y,SECOND,20);
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_ENTRY,.scene=scene_id,.entity=entity,.animation_state=idle},NULL));
    Fl_AnimationParameter speed={.name="speed",.kind=FL_ANIM_NUMBER},jump_parameter={.name="jump",.kind=FL_ANIM_TRIGGER};Fl_Id speed_id,jump_id;
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_PARAMETER_SET,.scene=scene_id,.entity=entity,.animation_parameter_record=&speed},&speed_id));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_PARAMETER_SET,.scene=scene_id,.entity=entity,.animation_parameter_record=&jump_parameter},&jump_id));
    Fl_AnimationTransition edges[]={
        {.from=idle,.to=run,.parameter=speed_id,.condition=FL_ANIM_GREATER,.threshold=.1,.blend_duration=SECOND/4},
        {.from=run,.to=idle,.parameter=speed_id,.condition=FL_ANIM_LESS,.threshold=.1,.blend_duration=SECOND/4},
        {.from=run,.to=jump,.parameter=jump_id,.condition=FL_ANIM_FIRED},
        {.from=jump,.to=run,.condition=FL_ANIM_ALWAYS,.at_end=true}};
    for (size_t i=0;i<4;++i) CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_TRANSITION_SET,.scene=scene_id,.entity=entity,.animation_transition_record=&edges[i]},NULL));
    const Fl_Scene *scene=fl_project_scene(p,scene_id);uint64_t revision=scene->revision;
    Fl_AnimationTransition invalid={.from=run,.to=999999};
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_TRANSITION_SET,.scene=scene_id,.entity=entity,.animation_transition_record=&invalid},NULL));
    Fl_AnimationState duplicate={.name="Run",.duration=SECOND};
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_STATE_SET,.scene=scene_id,.entity=entity,.animation_state_record=&duplicate},NULL));
    CHECK(scene->revision==revision);
    source(p,scene_id,entity,"function on_init(self) self.jumped=false end function on_update(self,dt,time) "
        "self.transform.x=self.transform.x+100*dt; self.animation:parameter('speed',time>=1 and 1 or 0); "
        "if time>=2 and not self.jumped then self.animation:trigger('jump'); self.jumped=true end end");
    Fl_SceneState state={0};
    frame(&state,scene,0,idle,100,20);
    frame(&state,scene,SECOND/2,idle,150,20);
    frame(&state,scene,SECOND,run,200,20);
    frame(&state,scene,SECOND+SECOND/10,run,210.4,20.8);
    frame(&state,scene,SECOND+SECOND/2,run,255,30);
    frame(&state,scene,2*SECOND,jump,300,20);
    frame(&state,scene,2*SECOND+SECOND/2,jump,350,-80);
    frame(&state,scene,3*SECOND,run,400,20);
    frame(&state,scene,SECOND/2,idle,150,20);
    frame(&state,scene,SECOND+SECOND/2,run,255,30);
    CHECK(!*fl_script_runtime_error(state.scripts,0));
    CHECK(fl_project_save(p));fl_scene_state_clear(&state);fl_project_destroy(p);
    p=fl_project_open(argv[1],NULL);CHECK(p);if (!p) return 1;
    scene=fl_project_scene(p,scene_id);CHECK(!fl_project_dirty(p));
    const Fl_Animation *a=fl_entity_animation(fl_scene_entity(scene,entity));
    CHECK(a&&a->state_count==3&&a->parameter_count==2&&a->transition_count==4);
    frame(&state,scene,SECOND+SECOND/2,run,255,30);
    /* Named state playback is controlled by the component API, without key writes from Lua. */
    source(p,scene_id,entity,"function on_init(self) self.animation:play('Run',0.25); self.animation:parameter('speed',1) end function on_update(self,dt,time) self.transform.x=self.transform.x+100*dt end");
    frame(&state,scene,SECOND/2,run,155,30);
    source(p,scene_id,entity,"function on_init(self) self.animation:play('Run'); self.animation:parameter('speed',1); self.animation:pause() end function on_update(self,dt,time) if time>=1 then self.animation:resume() end end");
    frame(&state,scene,SECOND/2,run,100,20);
    frame(&state,scene,SECOND,run,100+1.0/6,20+1.0/3);
    /* A graph runs without a script, using entry state and default parameters. */
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=entity,.component={.type=FL_COMPONENT_SCRIPT}},NULL));
    frame(&state,scene,SECOND/2,idle,100,20);
    /* Removing a state also removes its edges; removing a parameter prunes dependent conditions. */
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_STATE_REMOVE,.scene=scene_id,.entity=entity,.animation_state=jump},NULL));
    a=fl_entity_animation(fl_scene_entity(scene,entity));CHECK(a->state_count==2&&a->transition_count==2);
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ANIMATION_PARAMETER_REMOVE,.scene=scene_id,.entity=entity,.animation_parameter=speed_id},NULL));
    a=fl_entity_animation(fl_scene_entity(scene,entity));CHECK(a->parameter_count==1&&a->transition_count==0);
    CHECK(fl_project_save(p));fl_scene_state_clear(&state);fl_project_destroy(p);
    legacy_project(argv[1]);
    return FLOW_TEST_RESULT("fl_animation_tests");
}
