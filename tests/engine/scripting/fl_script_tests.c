#include "project/fl_source_asset.h"
#include "project/fl_prototype_authoring.h"
#include "time/fl_composition.h"
#include "flow_test.h"
#include "components/fl_animation.h"
#include "components/fl_component.h"
#include "components/fl_script.h"
#include "time/fl_scene_state.h"
#include "doc/fl_scene_geometry.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SECOND FL_TICKS_PER_SECOND

/* Store source as the entity's own repository script (created on first use) and attach it, through the validated boundary. */
static bool script(Fl_Project *project, Fl_Id scene, Fl_Id entity, const char *source, bool enabled)
{
    char name[48]; snprintf(name, sizeof(name), "entity %llu", (unsigned long long)entity);
    const Fl_Asset *existing = fl_project_asset_named(project, FL_ASSET_LUA, name);
    Fl_Id id = existing ? existing->id : 0, created = 0;
    if (!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene, .asset = id, .name = existing ? NULL : name,
                          .source = source, .source_size = strlen(source)}, &created)) return false;
    if (!id) id = created;
    Fl_Component component = {0};
    bool ok = fl_script_encode(&(Fl_Script){.script=id,.enabled=enabled}, &component);
    const Fl_Component *attachment=fl_entity_script_component(fl_scene_entity(fl_project_scene(project,scene),entity),id);
    if (attachment) component.id=attachment->id;
    if (ok) ok = fl_project_apply(project, &(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component}, NULL);
    fl_component_clear(&component);
    return ok;
}

/* Sample document time directly; evaluation never changes base components or revisions. */
static double sample(Fl_SceneState *state, const Fl_Scene *scene, Fl_Id entity, Fl_Tick tick)
{
    CHECK(fl_scene_state_update(state, scene, tick));
    const Fl_Entity *e = fl_scene_entity(scene, entity);
    CHECK(e != NULL);
    return e ? state->entities[e - scene->entities].pose.x : 0;
}

/* Runtime errors, budgets and invalid outputs retain all evaluated base values. */
static void failure(Fl_Project *project, const Fl_Scene *scene, Fl_Id entity, Fl_SceneState *state, const char *source, const char *message)
{
    CHECK(script(project, scene->id, entity, source, true));
    CHECK(sample(state, scene, entity, SECOND) == 185);
    const char *error = fl_script_runtime_error(fl_scene_state_scripts(state), (size_t)(fl_scene_entity(scene, entity) - scene->entities));
    CHECK(strstr(error, message) != NULL);
}

/* The repository: shared scripts, unique names, atomic validation, cascading removal and persistence. Closes project. */
static void repository(Fl_Project *project, Fl_Id scene_id, const char *directory)
{
    const Fl_Scene *scene = fl_project_scene(project, scene_id);
    Fl_Id one = 0, two = 0, counter = 0, other = 0;
    CHECK(fl_widget_create(project, scene_id, 0, FL_WIDGET_RECTANGLE, 0, 0, &one) && fl_widget_create(project, scene_id, 0, FL_WIDGET_RECTANGLE, 0, 0, &two));
    const char *source = "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x = self.pose.x + 10 * t end }";
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .name = "Counter"}, &counter));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .source = source, .source_size = strlen(source)}, &counter));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .name = "", .source = source, .source_size = strlen(source)}, &counter));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .name = "Bad", .source = "a\0b", .source_size = 3}, &counter));
    const size_t base = fl_project_asset_count(scene->project);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .name = "Counter", .source = source, .source_size = strlen(source)}, &counter) && counter);
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .name = "Counter", .source = source, .source_size = strlen(source)}, &other));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .name = "Other", .source = "x = 1", .source_size = 5}, &other));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .asset = other, .name = "Counter"}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .asset = other}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .asset = 999999, .name = "Ghost"}, NULL));
    CHECK(fl_project_asset_count(scene->project) == base + 2 && fl_project_asset_named(scene->project, FL_ASSET_LUA, "Counter")->id == counter);
    Fl_Component attach = {0};
    CHECK(fl_script_encode(&(Fl_Script){.script=999999,.enabled=true}, &attach));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = one, .component = attach}, NULL));
    fl_component_clear(&attach);
    CHECK(fl_script_encode(&(Fl_Script){.script=counter,.enabled=true}, &attach));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = one, .component = attach}, NULL));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = two, .component = attach}, NULL));
    fl_component_clear(&attach);
    CHECK(fl_scene_script_uses(scene, counter) == 2 && fl_scene_script_uses(scene, other) == 0);
    Fl_SceneState state = {0};
    CHECK(fl_scene_state_update(&state, scene, SECOND));
    const size_t first = (size_t)(fl_scene_entity(scene, one) - scene->entities), second_index = (size_t)(fl_scene_entity(scene, two) - scene->entities);
    CHECK(state.entities[first].pose.x == 170 && state.entities[second_index].pose.x == 170);
    const char *faster = "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x = 100 * t end }";
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .asset = counter, .source = faster, .source_size = strlen(faster)}, NULL));
    CHECK(fl_scene_state_update(&state, scene, SECOND));
    CHECK(state.entities[first].pose.x == 100 && state.entities[second_index].pose.x == 100);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .asset = counter, .name = "Ramp"}, NULL));
    CHECK(fl_project_asset_named(scene->project, FL_ASSET_LUA, "Ramp") && !fl_project_asset_named(scene->project, FL_ASSET_LUA, "Counter") && fl_scene_script_uses(scene, counter) == 2);
    CHECK(fl_project_save(project));
    fl_project_destroy(project);
    project = fl_project_open(directory, NULL);
    CHECK(project);
    if (!project) return;
    scene = fl_project_scene(project, scene_id);
    const Fl_Asset *ramp = fl_project_asset_named(scene->project, FL_ASSET_LUA, "Ramp");
    CHECK(ramp && ramp->id == counter && !strcmp(ramp->source, faster) && ramp->size == strlen(faster) && fl_scene_script_uses(scene, counter) == 2);
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=counter},NULL));
    CHECK(fl_project_edit_begin(project,"Detach and delete behavior",false));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=one,.component={.type=FL_COMPONENT_SCRIPT}},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=two,.component={.type=FL_COMPONENT_SCRIPT}},NULL));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .scene = scene_id, .asset = counter}, NULL));
    CHECK(fl_project_edit_commit(project));
    scene=fl_project_scene(project,scene_id);
    CHECK(!fl_project_asset(scene->project, counter) && fl_scene_script_uses(scene, counter) == 0 && !fl_entity_component(fl_scene_entity(scene, one), FL_COMPONENT_SCRIPT));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .scene = scene_id, .asset = counter}, NULL));
    Fl_Snapshot *snapshot = fl_project_snapshot(project);
    CHECK(snapshot && fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .scene = scene_id, .asset = other}, NULL));
    CHECK(fl_project_restore(project, snapshot));
    scene = fl_project_scene(project, scene_id);
    CHECK(fl_project_asset(scene->project, other) != NULL);
    fl_snapshot_destroy(snapshot);
    fl_scene_state_clear(&state);
    fl_project_destroy(project);
}

/* Multiple independent Script components retain identity across editing and disk round trips. */
static void instances(const char *directory)
{
    char dir[FL_PATH_MAX]; CHECK(fl_fs_join(dir,sizeof(dir),directory,"instances"));
    Fl_Project *project=fl_project_create(dir,"Instances",NULL); CHECK(project); if (!project) return;
    Fl_Id scene_id=fl_project_scene_at(project,0)->id, entity=0, x=0, y=0, first=0, second=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene_id,.name="Object"},&entity));
    const char *sx="return { outputs={self={x='replace'}}, sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x=10*t end }", *sy="return { outputs={self={y='replace'}}, sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.y=20*t end }";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="X",.source=sx,.source_size=strlen(sx)},&x));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Y",.source=sy,.source_size=strlen(sy)},&y));
    Fl_Component component={0};
    CHECK(fl_script_encode(&(Fl_Script){.script=x,.enabled=true},&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=component},&first)); fl_component_clear(&component);
    CHECK(fl_script_encode(&(Fl_Script){.script=y,.enabled=true},&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=component},&second)); fl_component_clear(&component);
    CHECK(first && second && first!=second);
    Fl_SceneState state={0}; const Fl_Scene *scene=fl_project_scene(project,scene_id);
    CHECK(fl_scene_state_update(&state,scene,SECOND)); CHECK(state.entities[0].pose.x==10 && state.entities[0].pose.y==20);
    Fl_Id transform=fl_entity_component(fl_scene_entity(scene,entity),FL_COMPONENT_POSE)->id;
    unsigned char bytes[FL_POSE_BYTES]; fl_pose_encode(&(Fl_Pose){.x=50,.sx=1,.sy=1,.sz=1},bytes);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component={.type=FL_COMPONENT_POSE,.version=1,.size=FL_POSE_BYTES,.data=bytes}},NULL));
    CHECK(fl_entity_component(fl_scene_entity(scene,entity),FL_COMPONENT_POSE)->id==transform);
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(dir,NULL); CHECK(project); if (!project) return;
    scene=fl_project_scene(project,scene_id); const Fl_Entity *owner=fl_scene_entity(scene,entity);
    CHECK(fl_entity_component_instance(owner,first) && fl_entity_component_instance(owner,second));
    CHECK(fl_scene_state_update(&state,scene,SECOND)); CHECK(state.entities[0].pose.x==10 && state.entities[0].pose.y==20);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=entity,.component.id=first},NULL));
    CHECK(!fl_entity_component_instance(fl_scene_entity(scene,entity),first) && fl_entity_component_instance(fl_scene_entity(scene,entity),second));
    CHECK(fl_scene_state_update(&state,scene,SECOND)); CHECK(state.entities[0].pose.x==50 && state.entities[0].pose.y==20);
    fl_scene_state_clear(&state); fl_project_destroy(project);
}

/* Beat bindings, local clocks and typed parameters retime without changing reusable source. */
static void retiming(const char *directory)
{
    char dir[FL_PATH_MAX]; CHECK(fl_fs_join(dir,sizeof(dir),directory,"retiming"));
    Fl_Project *project=fl_project_create(dir,"Retiming",NULL); CHECK(project); if (!project) return;
    Fl_Id scene_id=fl_project_scene_at(project,0)->id, before=fl_project_scene_at(project,0)->beats[0].id, beat=0, entity=0, asset=0, attachment=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_ADD,.scene=scene_id,.name="Effect",.position=1,.beat_duration=3*SECOND},&beat));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene_id,.name="Motion"},&entity));
    const char *source="return {parameters={distance={type='number',default=100,min=0,max=1000}},sample=function(ctx) ctx.entity.pose.x=ctx.params.distance*ctx.time.progress end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Entrance",.source=source,.source_size=strlen(source)},&asset));
    Fl_Script script={.script=asset,.enabled=true,.time=fl_time_mapping_default(),.parameters="{\"distance\":200}",.parameters_size=16};
    script.time.explicit_range=true; script.time.start=(Fl_TimeAnchor){.kind=FL_ANCHOR_BEAT,.beat=beat,.end=false,.offset=0}; script.time.end=(Fl_TimeAnchor){.kind=FL_ANCHOR_BEAT,.beat=beat,.end=true,.offset=0}; script.time.duration=SECOND;
    Fl_Component component={0}; CHECK(fl_script_encode(&script,&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=component},&attachment)); fl_component_clear(&component);
    Fl_SceneState state={0}; const Fl_Scene *scene=fl_project_scene(project,scene_id); Fl_Tick start,end;
    CHECK(fl_scene_beat_range(scene,beat,&start,&end)); CHECK(sample(&state,scene,entity,start+(end-start)/2)==100);
    CHECK(sample(&state,scene,entity,start-1)==0);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_UPDATE,.scene=scene_id,.beat=before,.name="Before",.beat_duration=7*SECOND},NULL));
    CHECK(fl_scene_beat_range(scene,beat,&start,&end)); CHECK(sample(&state,scene,entity,start+(end-start)/2)==100);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_UPDATE,.scene=scene_id,.beat=beat,.name="Effect",.beat_duration=6*SECOND},NULL));
    CHECK(fl_scene_beat_range(scene,beat,&start,&end)); CHECK(sample(&state,scene,entity,start+(end-start)/2)==100);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_MOVE,.scene=scene_id,.beat=beat,.position=0},NULL));
    CHECK(sample(&state,scene,entity,3*SECOND)==100);
    Fl_Id right=0; CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_SPLIT,.scene=scene_id,.beat=beat,.tick=3*SECOND,.name="Effect continuation"},&right));
    CHECK(sample(&state,scene,entity,3*SECOND)==100 && fabs(sample(&state,scene,entity,5*SECOND)-200.0*5.0/6.0)<1e-9);
    Fl_Script decoded; const Fl_Component *bound=fl_entity_component_instance(fl_scene_entity(scene,entity),attachment);
    CHECK(fl_script_decode(bound,&decoded) && decoded.time.start.beat==beat && decoded.time.end.beat==right);
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(dir,NULL); CHECK(project); if (!project) return;
    scene=fl_project_scene(project,scene_id); CHECK(sample(&state,scene,entity,3*SECOND)==100);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_REMOVE,.scene=scene_id,.beat=right},NULL));
    CHECK(sample(&state,scene,entity,SECOND)==0 && strstr(fl_script_runtime_error(fl_scene_state_scripts(&state),0),"Unbound"));
    CHECK(fl_project_asset(project,asset) && fl_entity_component_instance(fl_scene_entity(scene,entity),attachment));
    fl_scene_state_clear(&state); fl_project_destroy(project);
}

/* Exercise real Lua execution, component ownership, conflict rejection and persistence. */
/* Explicit fixed-step state survives sampling while hidden Lua locals reset on every callback. */
static void simulation(const char *root)
{
    char directory[FL_PATH_MAX]; CHECK(fl_fs_join(directory,sizeof(directory),root,"simulation"));
    Fl_Project *project=fl_project_create(directory,"Simulation",NULL); CHECK(project); if (!project) return;
    Fl_Id scene_id=fl_project_scene_at(project,0)->id, entity=0;
    CHECK(fl_widget_create(project,scene_id,0,FL_WIDGET_TEXT,0,0,&entity));
    const char *source=
        "local hidden=0; return {step_hz=60,"
        "initialize=function(ctx) ctx.state={x=0,count=0,input=0,random=0,integer=9223372036854775807,buffer='a\\0b'} end,"
        "step=function(ctx) hidden=hidden+1; local s=ctx.state; s.hidden=hidden; s.x=s.x+ctx.time.dt*10; s.count=s.count+1;"
        "s.input=s.input+ctx.entity.pose.y*ctx.time.dt; s.random=s.random+math.random() end,"
        "sample=function(ctx) local s=ctx.state; ctx.entity.pose.x=s.x; ctx.entity.pose.y=s.count;"
        "ctx.entity.bounds.width=s.input; ctx.entity.bounds.height=s.hidden or 0;"
        "ctx.entity.opacity=s.random/(ctx.time.step+1); ctx.entity.text=tostring(s.integer)..s.buffer; s.x=999999 end}";
    CHECK(script(project,scene_id,entity,source,true));
    const Fl_Scene *scene=fl_project_scene(project,scene_id);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene_id,.entity=entity,.property=FL_PROP_Y,
        .anchor={.offset=0},.value={0}},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene_id,.entity=entity,.property=FL_PROP_Y,
        .anchor={.offset=5*SECOND},.value={30}},NULL));
    /* Binary strings are valid simulation state but sampled text still requires valid text. */
    Fl_SceneState state={0}; sample(&state,scene,entity,SECOND);
    CHECK(strstr(fl_script_runtime_error(fl_scene_state_scripts(&state),0),"UTF-8")); fl_scene_state_clear(&state);
    source=
        "local hidden=0; return {step_hz=60,"
        "initialize=function(ctx) ctx.state={x=0,count=0,input=0,random=0,integer=9223372036854775807,buffer='a\\0b'} end,"
        "step=function(ctx) hidden=hidden+1; local s=ctx.state; s.hidden=hidden; s.x=s.x+ctx.time.dt*10; s.count=s.count+1;"
        "s.input=s.input+ctx.entity.pose.y*ctx.time.dt; s.random=s.random+math.random() end,"
        "sample=function(ctx) local s=ctx.state; assert(#s.buffer==3); ctx.entity.pose.x=s.x; ctx.entity.pose.y=s.count;"
        "ctx.entity.bounds.width=s.input; ctx.entity.bounds.height=s.hidden or 0;"
        "ctx.entity.opacity=s.random/(ctx.time.step+1); ctx.entity.text=tostring(s.integer); s.x=999999 end}";
    CHECK(script(project,scene_id,entity,source,true));
    CHECK(fabs(sample(&state,scene,entity,SECOND)-10)<1e-10);
    CHECK(state.entities[0].pose.y==60 && state.entities[0].bounds.height==1 && fabs(state.entities[0].bounds.width-3.05)<1e-10);
    CHECK(state.entities[0].text_override && !strcmp(state.entities[0].text,"9223372036854775807"));
    Fl_Tick times[]={2*SECOND,4*SECOND,SECOND/2,3*SECOND+SECOND/4,0,SECOND,4*SECOND,SECOND/3};
    for (size_t i=0;i<sizeof(times)/sizeof(*times);++i) {
        Fl_SceneState fresh={0};
        double warm=sample(&state,scene,entity,times[i]), cold=sample(&fresh,scene,entity,times[i]);
        CHECK(fabs(warm-cold)<1e-10 && fabs(warm-(double)(times[i]/2000)/6)<1e-10);
        CHECK(fabs(state.entities[0].bounds.width-fresh.entities[0].bounds.width)<1e-10 &&
              state.entities[0].opacity==fresh.entities[0].opacity && state.entities[0].pose.y==fresh.entities[0].pose.y);
        fl_scene_state_clear(&fresh);
    }
    const Fl_Entity *owner=fl_scene_entity(scene,entity);
    const Fl_Component *attached=fl_entity_component(owner,FL_COMPONENT_SCRIPT);
    Fl_Script behavior; CHECK(fl_script_decode(attached,&behavior));
    behavior.time.duration=5*SECOND;
    Fl_Component mapped={0}; CHECK(fl_script_encode(&behavior,&mapped)); mapped.id=attached->id;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=mapped},NULL)); fl_component_clear(&mapped);
    Fl_Id beat=scene->beats[0].id;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_UPDATE,.scene=scene_id,.beat=beat,.name="Stretched",.beat_duration=10*SECOND},NULL));
    CHECK(fabs(sample(&state,scene,entity,2*SECOND)-10)<1e-10);
    CHECK(fabs(state.entities[0].bounds.width-6.1)<1e-10);
    attached=fl_entity_component(fl_scene_entity(scene,entity),FL_COMPONENT_SCRIPT);
    CHECK(fl_script_decode(attached,&behavior)); behavior.time.duration=SECOND; behavior.time.mode=FL_RETIME_FIXED; behavior.time.loop=true;
    CHECK(fl_script_encode(&behavior,&mapped)); mapped.id=attached->id;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=mapped},NULL)); fl_component_clear(&mapped);
    CHECK(fabs(sample(&state,scene,entity,SECOND+SECOND/4)-2.5)<1e-10);
    CHECK(state.entities[0].pose.y==15 && fabs(state.entities[0].bounds.width-1.7)<1e-10);
    CHECK(fabs(sample(&state,scene,entity,SECOND/4)-2.5)<1e-10);
    CHECK(fabs(state.entities[0].bounds.width-0.2)<1e-10);
    CHECK(!script(project,scene_id,entity,"return {step_hz=7,step=function(ctx) end}",true));
    CHECK(strstr(fl_project_diagnostic(project),"step_hz"));
    scene=fl_project_scene(project,scene_id);
    CHECK(script(project,scene_id,entity,"return {initialize=function(ctx) ctx.state={callback=function() end} end, sample=function(ctx) end}",true));
    sample(&state,scene,entity,SECOND); CHECK(strstr(fl_script_runtime_error(fl_scene_state_scripts(&state),0),"functions"));
    fl_scene_state_clear(&state); fl_project_destroy(project);
}

/* Imports retain resources, reject cycles and reconstruct shared modules once per isolated invocation. */
static void imports(const char *directory)
{
    char path[FL_PATH_MAX]; CHECK(fl_fs_join(path,sizeof(path),directory,"imports"));
    Fl_Project *project=fl_project_create(path,"Imports",NULL); CHECK(project); if (!project) return;
    Fl_Id scene=fl_project_scene_at(project,0)->id,entity=0,helper=0;
    const char *library="local n=0; return {next=function() n=n+1;return n end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Helper",.source=library,.source_size=strlen(library)},&helper));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Imported motion"},&entity));
    char source[512]; snprintf(source,sizeof(source),"return {imports={one='%llu',two='%llu'},sample=function(ctx) ctx.entity.pose.x=ctx.imports.one.next()+ctx.imports.two.next() end}",(unsigned long long)helper,(unsigned long long)helper);
    CHECK(script(project,scene,entity,source,true));
    Fl_SceneState state={0};
    CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND)==3);
    CHECK(sample(&state,fl_project_scene(project,scene),entity,2*SECOND)==3);
    CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND)==3);
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=helper},NULL));
    const char *changed="local n=10;return {next=function() n=n+1;return n end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=helper,.source=changed,.source_size=strlen(changed)},NULL));
    CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND)==23);
    Fl_Id nested=0;
    snprintf(source,sizeof(source),"local m={imports={child='%llu'}};m.next=function()return m.imports.child.next() end;return m",(unsigned long long)helper);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Nested",.source=source,.source_size=strlen(source)},&nested));
    snprintf(source,sizeof(source),"return {imports={nested='%llu'},sample=function(ctx)ctx.entity.pose.x=ctx.imports.nested.next() end}",(unsigned long long)nested);
    CHECK(script(project,scene,entity,source,true));
    CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND)==11);
    char cycle[128]; snprintf(cycle,sizeof(cycle),"return {imports={nested='%llu'},next=function()return 1 end}",(unsigned long long)nested);
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=helper,.source=cycle,.source_size=strlen(cycle)},NULL));
    CHECK(strstr(fl_project_diagnostic(project),"recursive"));
    const char *noise="return {draw=function()return math.random() end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Noise",.source=noise,.source_size=strlen(noise)},&helper));
    snprintf(source,sizeof(source),"return {imports={noise='%llu'},step_hz=60,initialize=function(ctx)ctx.state={sum=0} end,step=function(ctx)ctx.state.sum=ctx.state.sum+ctx.imports.noise.draw() end,sample=function(ctx)ctx.entity.pose.x=ctx.state.sum end}",(unsigned long long)helper);
    CHECK(script(project,scene,entity,source,true));
    double half=sample(&state,fl_project_scene(project,scene),entity,SECOND/2),whole=sample(&state,fl_project_scene(project,scene),entity,SECOND);
    CHECK(half>0 && whole>half && fabs(whole-2*half)>1e-9);
    CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND/2)==half);
    CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND)==whole);
    fl_scene_state_clear(&state); CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND)==whole);
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(path,NULL); CHECK(project);
    if (project) {CHECK(sample(&state,fl_project_scene(project,scene),entity,SECOND)==whole); fl_scene_state_clear(&state); fl_project_destroy(project);}
}

/* Typed bindings survive persistence and read native time inputs independently of behavior order. */
static void bindings(const char *directory)
{
    char dir[FL_PATH_MAX]; CHECK(fl_fs_join(dir,sizeof(dir),directory,"bindings"));
    Fl_Project *project=fl_project_create(dir,"Bindings",NULL); CHECK(project); if (!project) return;
    Fl_Id scene_id=fl_project_scene_at(project,0)->id,reader=0,target=0,asset=0,resource=0,attachment=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene_id,.name="Reader"},&reader));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene_id,.name="Target"},&target));
    unsigned char bytes[FL_POSE_BYTES]; fl_pose_encode(&(Fl_Pose){.x=37,.sx=1,.sy=1,.sz=1},bytes);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=target,.component={.type=FL_COMPONENT_POSE,.version=1,.size=FL_POSE_BYTES,.data=bytes}},NULL));
    CHECK(script(project,scene_id,target,"return {sample=function(ctx) ctx.entity.pose.x=999 end}",true));
    const char *source="return {bindings={target={type='entity'},transform={type='component'},region={type='region'},resource={type='asset'},unused={type='entity',optional=true}},sample=function(ctx) assert(ctx.bindings.transform.type=='pose'); assert(ctx.bindings.resource.name=='Bound resource'); assert(ctx.bindings.region.end_tick>ctx.bindings.region.start_tick); ctx.entity.pose.x=ctx.bindings.target.pose.x;ctx.bindings.target.pose.x=888 end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Reader behavior",.source=source,.source_size=strlen(source)},&asset));
    const char *library="return {}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Bound resource",.source=library,.source_size=strlen(library)},&resource));
    const Fl_Scene *scene=fl_project_scene(project,scene_id);
    Fl_Id transform=fl_entity_component(fl_scene_entity(scene,target),FL_COMPONENT_POSE)->id;
    char references[512]; snprintf(references,sizeof(references),"{\"target\":{\"type\":\"entity\",\"id\":\"%llu\"},\"transform\":{\"type\":\"component\",\"entity\":\"%llu\",\"id\":\"%llu\"},\"region\":{\"type\":\"region\",\"id\":\"%llu\"},\"resource\":{\"type\":\"asset\",\"id\":\"%llu\"}}",(unsigned long long)target,(unsigned long long)target,(unsigned long long)transform,(unsigned long long)scene->beats[0].id,(unsigned long long)resource);
    Fl_Script configuration={.script=asset,.enabled=true,.bindings=references,.bindings_size=strlen(references)};
    Fl_Component component={0}; CHECK(fl_script_encode(&configuration,&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=reader,.component=component},&attachment)); fl_component_clear(&component);
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=resource},NULL));
    const char *invalid="return {bindings={target={type='asset'}},sample=function(ctx) end}";
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=asset,.source=invalid,.source_size=strlen(invalid)},NULL));
    Fl_SceneState state={0}; CHECK(sample(&state,fl_project_scene(project,scene_id),reader,SECOND)==37);
    CHECK(!fl_script_runtime_component_error(fl_scene_state_scripts(&state),attachment)[0]);
    char simulated[2048]; const char *callback=strstr(source,",sample="); CHECK(callback);
    snprintf(simulated,sizeof(simulated),"%.*s,step_hz=10,initialize=function(ctx) ctx.state={x=0} end,step=function(ctx) ctx.state.x=ctx.state.x+ctx.bindings.target.pose.x*ctx.time.dt end,sample=function(ctx) ctx.entity.pose.x=ctx.state.x end}",(int)(callback-source),source);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=asset,.source=simulated,.source_size=strlen(simulated)},NULL));
    CHECK(fabs(sample(&state,fl_project_scene(project,scene_id),reader,SECOND)-37)<1e-10);
    CHECK(fabs(sample(&state,fl_project_scene(project,scene_id),reader,SECOND/2)-18.5)<1e-10);
    CHECK(fabs(sample(&state,fl_project_scene(project,scene_id),reader,SECOND)-37)<1e-10);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=asset,.source=source,.source_size=strlen(source)},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_PARENT,.scene=scene_id,.entity=target,.parent=reader},NULL));
    Fl_Id prototype=0,instance=0;
    CHECK(fl_prototype_capture(project,scene_id,reader,"Bound hierarchy",&prototype));
    CHECK(fl_prototype_instantiate(project,scene_id,0,prototype,100,100,&instance));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene_id),SECOND));
    bool generated=false;
    for (size_t i=0;i<state.count;++i) {
        const Fl_Origin *origin=fl_composition_origin(state.composition,i);
        if (origin && origin->generated && origin->owner==instance && origin->entity==reader) {
            generated=true; CHECK(state.entities[i].pose.x==37);
            CHECK(!fl_scene_state_entity_error(&state,i)[0]);
        }
    }
    CHECK(generated);
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(dir,NULL); CHECK(project); if (!project) return;
    CHECK(sample(&state,fl_project_scene(project,scene_id),reader,SECOND)==37);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene_id,.entity=target},NULL));
    sample(&state,fl_project_scene(project,scene_id),reader,SECOND);
    CHECK(strstr(fl_script_runtime_component_error(fl_scene_state_scripts(&state),attachment),"unresolved"));
    configuration.bindings="{\"target\":{\"type\":\"unknown\",\"id\":1}}"; configuration.bindings_size=strlen(configuration.bindings);
    CHECK(!fl_script_encode(&configuration,&component));
    fl_scene_state_clear(&state); fl_project_destroy(project);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char path[FL_PATH_MAX];
    CHECK(fl_fs_mkdir_p(argv[1]));
    instances(argv[1]);
    simulation(argv[1]);
    retiming(argv[1]);
    CHECK(fl_fs_join(path, sizeof(path), argv[1], "project.flow.json")); fl_fs_remove(path);
    Fl_Project *project=fl_project_create(argv[1], "Lua components", NULL);
    CHECK(project); if (!project) return 1;
    Fl_Id scene_id=fl_project_scene_at(project,0)->id, box, second;
    CHECK(fl_widget_create(project,scene_id,0,FL_WIDGET_RECTANGLE,25,40,&box));
    CHECK(fl_widget_create(project,scene_id,box,FL_WIDGET_RECTANGLE,10,20,&second));
    const Fl_Scene *scene=fl_project_scene(project,scene_id);
    const Fl_Entity *entity=fl_scene_entity(scene,box);
    CHECK(entity->components[0].type==FL_COMPONENT_POSE);
    uint64_t revision=scene->revision;
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_POSE}},NULL));
    CHECK(scene->revision==revision);
    Fl_SceneState state={0};
    const char *motion="return { sample = function(ctx) local self=ctx.entity; local time=ctx.time.seconds;  self.pose.x = self.pose.x + time * 120 end }";
    CHECK(script(project,scene_id,box,motion,true));
    revision=scene->revision;
    CHECK(sample(&state,scene,box,SECOND)==305);
    CHECK(sample(&state,scene,box,2*SECOND)==425);
    CHECK(sample(&state,scene,box,SECOND/2)==245);
    CHECK(sample(&state,scene,box,SECOND)==305);
    CHECK(scene->revision==revision);
    Fl_Pose transform;
    CHECK(fl_pose_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_POSE),&transform));
    CHECK(transform.x==185);
    CHECK(!*fl_script_runtime_error(fl_scene_state_scripts(&state),0));
    Fl_SceneGeometry geometry={0};
    CHECK(fl_scene_geometry_update(&geometry,scene,&state));
    CHECK(fabs(geometry.entities[1].world.v[2] - 155)<1e-9);
    fl_scene_geometry_clear(&geometry);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene_id,.entity=box,.property=FL_PROP_Y,
        .anchor=fl_anchor_at(scene,0),.value={100}},NULL));
    const Fl_Animation *animation=fl_entity_animation(fl_scene_entity(scene,box));
    CHECK(animation && animation->track_count==1 && animation->tracks[0].property==FL_PROP_Y);
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==305 && state.entities[0].pose.y==100);
    CHECK(script(project,scene_id,second,"return { sample = function(ctx) local self=ctx.entity; local time=ctx.time.seconds;  self.pose.x = time * 10 end }",true));
    CHECK(sample(&state,scene,second,SECOND)==10);
    CHECK(sample(&state,scene,box,SECOND)==305);
    /* Malformed executable declarations never replace the last accepted behavior. */
    CHECK(!script(project,scene_id,box,"return {}",true) && strstr(fl_project_diagnostic(project),"sample(ctx)"));
    scene=fl_project_scene(project,scene_id);
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.animation:play('Run') end }","nil");
    CHECK(sample(&state,scene,box,2*SECOND)==185);
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==305);
    /* A keyed Inspector-like configure must not bake evaluated script channels into base values. */
    CHECK(fl_pose_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_POSE),&transform));
    transform.y=state.entities[0].pose.y;
    unsigned char bytes[FL_POSE_BYTES]; fl_pose_encode(&transform,bytes);
    Fl_Component component={.type = FL_COMPONENT_POSE, .version = 1, .size = FL_POSE_BYTES, .data = bytes};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene_id,.entity=box,.name="Renamed",.visible=true,
        .components=&component,.component_count=1,.keyed=true,.tick=SECOND},NULL));
    CHECK(fl_pose_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_POSE),&transform) && transform.x==185);
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==1);
    /* Growing a copied animation during configure must remain owned by the replacement component. */
    for (unsigned k=1;k<7;++k) {
        transform.y=100+k;fl_pose_encode(&transform,bytes);
        CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene_id,.entity=box,.name="Renamed",.visible=true,
            .components=&component,.component_count=1,.keyed=true,.tick=(Fl_Tick)k*SECOND/2},NULL));
    }
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==7);
    CHECK(fl_project_save(project));
    fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(argv[1],NULL);CHECK(project);if (!project) return 1;
    scene=fl_project_scene(project,scene_id);
    CHECK(!fl_project_dirty(project));
    CHECK(sample(&state,scene,box,SECOND)==305);
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==7);
    /* The global environment and random stream reset for each evaluation. */
    CHECK(script(project,scene_id,box,"n=(n or 0)+1; return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x=n+math.random() end }",true));
    double a=sample(&state,scene,box,SECOND);
    CHECK(sample(&state,scene,box,2*SECOND)==a && a>1 && a<2);
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  while true do end end }","instruction limit");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x=0/0 end }","invalid x");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x=99; error('failed') end }","failed");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x=string.rep('a',1000000000) end }","memory");
    CHECK(!script(project,scene_id,box,"return { sample=function(",true) && strstr(fl_project_diagnostic(project),"expected"));
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  io.open('file') end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  pcall(function() end) end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  os.execute('echo nope') end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  require('x') end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose=nil end }","table");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  math.randomseed() end }","number expected");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x=tonumber(tostring({})) end }","scalar");
    CHECK(script(project,scene_id,box,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  local sum=0; for k,v in pairs({c=3,a=1,b=2}) do sum=sum*10+v end; self.pose.x=sum end }",true));
    CHECK(sample(&state,scene,box,SECOND)==123);
    CHECK(sample(&state,scene,box,2*SECOND)==123);
    /* Scripts may replace a text entity's content; the document keeps its own text. */
    Fl_Id label = 0;
    CHECK(fl_widget_create(project, scene_id, 0, FL_WIDGET_TEXT, 0, 0, &label));
    CHECK(script(project, scene_id, label, "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.text = self.text .. ' ' .. math.floor(t) end }", true));
    scene = fl_project_scene(project, scene_id);
    const size_t label_index = (size_t)(fl_scene_entity(scene, label) - scene->entities);
    sample(&state, scene, label, 3 * SECOND);
    CHECK(state.entities[label_index].text_override && !strcmp(state.entities[label_index].text, "Text 3"));
    const Fl_Component *base_text = fl_entity_component(fl_scene_entity(scene, label), FL_COMPONENT_TEXT);
    CHECK(base_text->size == 4 && !memcmp(base_text->data, "Text", 4));
    CHECK(script(project, scene_id, label, "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.text = 5 end }", true));
    sample(&state, scene, label, SECOND);
    CHECK(strstr(fl_script_runtime_error(fl_scene_state_scripts(&state), label_index), "string") && !state.entities[label_index].text_override);
    CHECK(script(project, scene_id, label, "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.text = self.text end }", true));
    sample(&state, scene, label, SECOND);
    CHECK(!state.entities[label_index].text_override);
    CHECK(script(project, scene_id, label,
        "return {sample=function(ctx) ctx.entity.text=string.rep('long text ',1000) end}", true));
    sample(&state, scene, label, SECOND);
    CHECK(state.entities[label_index].text_override && state.entities[label_index].text_size == 10000 &&
          strlen(state.entities[label_index].text) == 10000);
    Fl_Id suffix = 0;
    const char *suffix_source = "return {sample=function(ctx) ctx.entity.text=ctx.entity.text..'!' end}";
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Suffix",.source=suffix_source,.source_size=strlen(suffix_source)}, &suffix));
    Fl_Component suffix_component = {0};
    CHECK(fl_script_encode(&(Fl_Script){.script=suffix,.enabled=true,.layer=1}, &suffix_component));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=label,.component=suffix_component}, NULL));
    fl_component_clear(&suffix_component);
    sample(&state, scene, label, SECOND);
    CHECK(state.entities[label_index].text_size == 10001 && state.entities[label_index].text[10000] == '!');
    CHECK(script(project, scene_id, label,
        "return {sample=function(ctx) ctx.entity.text=string.char(255) end}", true));
    sample(&state, scene, label, SECOND);
    CHECK(!state.entities[label_index].text_override && strstr(fl_script_runtime_error(fl_scene_state_scripts(&state),label_index), "UTF-8"));
    CHECK(script(project, scene_id, box,
        "return {sample=function(ctx) ctx.entity.crop.x=0.2;ctx.entity.crop.width=0.5 end}", true));
    sample(&state, scene, box, SECOND);
    const size_t box_index = (size_t)(fl_scene_entity(scene,box)-scene->entities);
    CHECK(state.entities[box_index].crop[0] == 0.2 && state.entities[box_index].crop[2] == 0.5);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = scene_id, .entity = label}, NULL));
    /* A corrected source recovers immediately after compilation/runtime/memory errors. */
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==305);
    CHECK(script(project,scene_id,box,motion,false));
    CHECK(sample(&state,scene,box,SECOND)==185);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_ANIMATION}},NULL));
    CHECK(!fl_entity_animation(fl_scene_entity(scene,box)) && !fl_scene_track_count(scene));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_SCRIPT}},NULL));
    CHECK(sample(&state,scene,box,SECOND)==185);
    Fl_Component invalid={0};
    CHECK(!fl_script_encode(&(Fl_Script){.script=0,.enabled=true},&invalid));
    CHECK(!fl_script_source_valid("",0) && !fl_script_source_valid("a\0b",3) && fl_script_source_valid("x",1));
    CHECK(!fl_source_asset_name_valid("") && !fl_source_asset_name_valid("a\nb") && fl_source_asset_name_valid("Counter"));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene_id,.entity=box},NULL));
    CHECK(scene->entity_count==0 && fl_scene_track_count(scene)==0);
    CHECK(fl_project_save(project));
    fl_scene_state_clear(&state);
    repository(project, scene_id, argv[1]);
    imports(argv[1]);
    bindings(argv[1]);
    return FLOW_TEST_RESULT("fl_script_tests");
}
