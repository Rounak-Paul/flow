#include "project/fl_source_asset.h"
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
    return e ? state->entities[e - scene->entities].transform.x : 0;
}

/* Runtime errors, budgets and invalid outputs retain all evaluated base values. */
static void failure(Fl_Project *project, const Fl_Scene *scene, Fl_Id entity, Fl_SceneState *state, const char *source, const char *message)
{
    CHECK(script(project, scene->id, entity, source, true));
    CHECK(sample(state, scene, entity, SECOND) == 25);
    const char *error = fl_script_runtime_error(state->scripts, (size_t)(fl_scene_entity(scene, entity) - scene->entities));
    CHECK(strstr(error, message) != NULL);
}

/* The repository: shared scripts, unique names, atomic validation, cascading removal and persistence. Closes project. */
static void repository(Fl_Project *project, Fl_Id scene_id, const char *directory)
{
    const Fl_Scene *scene = fl_project_scene(project, scene_id);
    Fl_Id one = 0, two = 0, counter = 0, other = 0;
    CHECK(fl_widget_create(project, scene_id, 0, FL_WIDGET_RECTANGLE, 0, 0, &one) && fl_widget_create(project, scene_id, 0, FL_WIDGET_RECTANGLE, 0, 0, &two));
    const char *source = "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x = self.transform.x + 10 * t end }";
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
    CHECK(state.entities[first].transform.x == 10 && state.entities[second_index].transform.x == 10);
    const char *faster = "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x = 100 * t end }";
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .asset = counter, .source = faster, .source_size = strlen(faster)}, NULL));
    CHECK(fl_scene_state_update(&state, scene, SECOND));
    CHECK(state.entities[first].transform.x == 100 && state.entities[second_index].transform.x == 100);
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
    const char *sx="return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=10*t end }", *sy="return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.y=20*t end }";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="X",.source=sx,.source_size=strlen(sx)},&x));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Y",.source=sy,.source_size=strlen(sy)},&y));
    Fl_Component component={0};
    CHECK(fl_script_encode(&(Fl_Script){.script=x,.enabled=true},&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=component},&first)); fl_component_clear(&component);
    CHECK(fl_script_encode(&(Fl_Script){.script=y,.enabled=true},&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=component},&second)); fl_component_clear(&component);
    CHECK(first && second && first!=second);
    Fl_SceneState state={0}; const Fl_Scene *scene=fl_project_scene(project,scene_id);
    CHECK(fl_scene_state_update(&state,scene,SECOND)); CHECK(state.entities[0].transform.x==10 && state.entities[0].transform.y==20);
    Fl_Id transform=fl_entity_component(fl_scene_entity(scene,entity),FL_COMPONENT_TRANSFORM)->id;
    unsigned char bytes[56]; fl_transform_encode(&(Fl_Transform){.x=50,.width=100,.height=100,.scale_x=1,.scale_y=1},bytes);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component={.type=FL_COMPONENT_TRANSFORM,.version=1,.size=56,.data=bytes}},NULL));
    CHECK(fl_entity_component(fl_scene_entity(scene,entity),FL_COMPONENT_TRANSFORM)->id==transform);
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(dir,NULL); CHECK(project); if (!project) return;
    scene=fl_project_scene(project,scene_id); const Fl_Entity *owner=fl_scene_entity(scene,entity);
    CHECK(fl_entity_component_instance(owner,first) && fl_entity_component_instance(owner,second));
    CHECK(fl_scene_state_update(&state,scene,SECOND)); CHECK(state.entities[0].transform.x==10 && state.entities[0].transform.y==20);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=entity,.component.id=first},NULL));
    CHECK(!fl_entity_component_instance(fl_scene_entity(scene,entity),first) && fl_entity_component_instance(fl_scene_entity(scene,entity),second));
    CHECK(fl_scene_state_update(&state,scene,SECOND)); CHECK(state.entities[0].transform.x==50 && state.entities[0].transform.y==20);
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
    const char *source="return {parameters={distance={type='number',default=100,min=0,max=1000}},sample=function(ctx) ctx.entity.transform.x=ctx.params.distance*ctx.time.progress end}";
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
    CHECK(sample(&state,scene,entity,SECOND)==0 && strstr(fl_script_runtime_error(state.scripts,0),"Unbound"));
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
        "s.input=s.input+ctx.entity.transform.y*ctx.time.dt; s.random=s.random+math.random() end,"
        "sample=function(ctx) local s=ctx.state; ctx.entity.transform.x=s.x; ctx.entity.transform.y=s.count;"
        "ctx.entity.transform.width=s.input; ctx.entity.transform.height=s.hidden or 0;"
        "ctx.entity.opacity=s.random/(ctx.time.step+1); ctx.entity.text=tostring(s.integer)..s.buffer; s.x=999999 end}";
    CHECK(script(project,scene_id,entity,source,true));
    const Fl_Scene *scene=fl_project_scene(project,scene_id);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene_id,.entity=entity,.property=FL_PROP_Y,
        .anchor={.offset=0},.value={0}},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene_id,.entity=entity,.property=FL_PROP_Y,
        .anchor={.offset=5*SECOND},.value={30}},NULL));
    /* Binary strings are valid simulation state but sampled text still requires valid text. */
    Fl_SceneState state={0}; sample(&state,scene,entity,SECOND);
    CHECK(strstr(fl_script_runtime_error(state.scripts,0),"UTF-8")); fl_scene_state_clear(&state);
    source=
        "local hidden=0; return {step_hz=60,"
        "initialize=function(ctx) ctx.state={x=0,count=0,input=0,random=0,integer=9223372036854775807,buffer='a\\0b'} end,"
        "step=function(ctx) hidden=hidden+1; local s=ctx.state; s.hidden=hidden; s.x=s.x+ctx.time.dt*10; s.count=s.count+1;"
        "s.input=s.input+ctx.entity.transform.y*ctx.time.dt; s.random=s.random+math.random() end,"
        "sample=function(ctx) local s=ctx.state; assert(#s.buffer==3); ctx.entity.transform.x=s.x; ctx.entity.transform.y=s.count;"
        "ctx.entity.transform.width=s.input; ctx.entity.transform.height=s.hidden or 0;"
        "ctx.entity.opacity=s.random/(ctx.time.step+1); ctx.entity.text=tostring(s.integer); s.x=999999 end}";
    CHECK(script(project,scene_id,entity,source,true));
    CHECK(fabs(sample(&state,scene,entity,SECOND)-10)<1e-10);
    CHECK(state.entities[0].transform.y==60 && state.entities[0].transform.height==1 && fabs(state.entities[0].transform.width-3.05)<1e-10);
    CHECK(state.entities[0].text_override && !strcmp(state.entities[0].text,"9223372036854775807"));
    Fl_Tick times[]={2*SECOND,4*SECOND,SECOND/2,3*SECOND+SECOND/4,0,SECOND,4*SECOND,SECOND/3};
    for (size_t i=0;i<sizeof(times)/sizeof(*times);++i) {
        Fl_SceneState fresh={0};
        double warm=sample(&state,scene,entity,times[i]), cold=sample(&fresh,scene,entity,times[i]);
        CHECK(fabs(warm-cold)<1e-10 && fabs(warm-(double)(times[i]/2000)/6)<1e-10);
        CHECK(fabs(state.entities[0].transform.width-fresh.entities[0].transform.width)<1e-10 &&
              state.entities[0].opacity==fresh.entities[0].opacity && state.entities[0].transform.y==fresh.entities[0].transform.y);
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
    CHECK(fabs(state.entities[0].transform.width-6.1)<1e-10);
    attached=fl_entity_component(fl_scene_entity(scene,entity),FL_COMPONENT_SCRIPT);
    CHECK(fl_script_decode(attached,&behavior)); behavior.time.duration=SECOND; behavior.time.mode=FL_RETIME_FIXED; behavior.time.loop=true;
    CHECK(fl_script_encode(&behavior,&mapped)); mapped.id=attached->id;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=mapped},NULL)); fl_component_clear(&mapped);
    CHECK(fabs(sample(&state,scene,entity,SECOND+SECOND/4)-2.5)<1e-10);
    CHECK(state.entities[0].transform.y==15 && fabs(state.entities[0].transform.width-1.7)<1e-10);
    CHECK(fabs(sample(&state,scene,entity,SECOND/4)-2.5)<1e-10);
    CHECK(fabs(state.entities[0].transform.width-0.2)<1e-10);
    CHECK(!script(project,scene_id,entity,"return {step_hz=7,step=function(ctx) end}",true));
    CHECK(strstr(fl_project_diagnostic(project),"step_hz"));
    scene=fl_project_scene(project,scene_id);
    CHECK(script(project,scene_id,entity,"return {initialize=function(ctx) ctx.state={callback=function() end} end, sample=function(ctx) end}",true));
    sample(&state,scene,entity,SECOND); CHECK(strstr(fl_script_runtime_error(state.scripts,0),"functions"));
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
    CHECK(entity->components[0].type==FL_COMPONENT_TRANSFORM);
    uint64_t revision=scene->revision;
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_TRANSFORM}},NULL));
    CHECK(scene->revision==revision);
    Fl_SceneState state={0};
    const char *motion="return { sample = function(ctx) local self=ctx.entity; local time=ctx.time.seconds;  self.transform.x = self.transform.x + time * 120 end }";
    CHECK(script(project,scene_id,box,motion,true));
    revision=scene->revision;
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(sample(&state,scene,box,2*SECOND)==265);
    CHECK(sample(&state,scene,box,SECOND/2)==85);
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(scene->revision==revision);
    Fl_Transform transform;
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_TRANSFORM),&transform));
    CHECK(transform.x==25);
    CHECK(!*fl_script_runtime_error(state.scripts,0));
    Fl_SceneGeometry geometry={0};
    CHECK(fl_scene_geometry_update(&geometry,scene,&state));
    CHECK(fabs(geometry.entities[1].world.v[2] - 155)<1e-9);
    fl_scene_geometry_clear(&geometry);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene_id,.entity=box,.property=FL_PROP_Y,
        .anchor=fl_anchor_at(scene,0),.value={100}},NULL));
    const Fl_Animation *animation=fl_entity_animation(fl_scene_entity(scene,box));
    CHECK(animation && animation->track_count==1 && animation->tracks[0].property==FL_PROP_Y);
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==145 && state.entities[0].transform.y==100);
    CHECK(script(project,scene_id,second,"return { sample = function(ctx) local self=ctx.entity; local time=ctx.time.seconds;  self.transform.x = time * 10 end }",true));
    CHECK(sample(&state,scene,second,SECOND)==10);
    CHECK(sample(&state,scene,box,SECOND)==145);
    /* Malformed executable declarations never replace the last accepted behavior. */
    CHECK(!script(project,scene_id,box,"return {}",true) && strstr(fl_project_diagnostic(project),"sample(ctx)"));
    scene=fl_project_scene(project,scene_id);
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.animation:play('Run') end }","nil");
    CHECK(sample(&state,scene,box,2*SECOND)==25);
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==145);
    /* A keyed Inspector-like configure must not bake evaluated script channels into base values. */
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_TRANSFORM),&transform));
    transform.y=state.entities[0].transform.y;
    unsigned char bytes[56]; fl_transform_encode(&transform,bytes);
    Fl_Component component={.type = FL_COMPONENT_TRANSFORM, .version = 1, .size = 56, .data = bytes};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene_id,.entity=box,.name="Renamed",.visible=true,
        .components=&component,.component_count=1,.keyed=true,.tick=SECOND},NULL));
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_TRANSFORM),&transform) && transform.x==25);
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==1);
    /* Growing a copied animation during configure must remain owned by the replacement component. */
    for (unsigned k=1;k<7;++k) {
        transform.y=100+k;fl_transform_encode(&transform,bytes);
        CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene_id,.entity=box,.name="Renamed",.visible=true,
            .components=&component,.component_count=1,.keyed=true,.tick=(Fl_Tick)k*SECOND/2},NULL));
    }
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==7);
    CHECK(fl_project_save(project));
    fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(argv[1],NULL);CHECK(project);if (!project) return 1;
    scene=fl_project_scene(project,scene_id);
    CHECK(!fl_project_dirty(project));
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==7);
    /* The global environment and random stream reset for each evaluation. */
    CHECK(script(project,scene_id,box,"n=(n or 0)+1; return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=n+math.random() end }",true));
    double a=sample(&state,scene,box,SECOND);
    CHECK(sample(&state,scene,box,2*SECOND)==a && a>1 && a<2);
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  while true do end end }","instruction limit");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=0/0 end }","invalid x");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=99; error('failed') end }","failed");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=string.rep('a',1000000000) end }","memory");
    CHECK(!script(project,scene_id,box,"return { sample=function(",true) && strstr(fl_project_diagnostic(project),"expected"));
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  io.open('file') end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  pcall(function() end) end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  os.execute('echo nope') end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  require('x') end }","nil");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform=nil end }","table");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  math.randomseed() end }","number expected");
    failure(project,scene,box,&state,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=tonumber(tostring({})) end }","scalar");
    CHECK(script(project,scene_id,box,"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  local sum=0; for k,v in pairs({c=3,a=1,b=2}) do sum=sum*10+v end; self.transform.x=sum end }",true));
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
    CHECK(strstr(fl_script_runtime_error(state.scripts, label_index), "string") && !state.entities[label_index].text_override);
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
    CHECK(fl_script_encode(&(Fl_Script){.script=suffix,.enabled=true}, &suffix_component));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=label,.component=suffix_component}, NULL));
    fl_component_clear(&suffix_component);
    sample(&state, scene, label, SECOND);
    CHECK(state.entities[label_index].text_size == 10001 && state.entities[label_index].text[10000] == '!');
    CHECK(script(project, scene_id, label,
        "return {sample=function(ctx) ctx.entity.text=string.char(255) end}", true));
    sample(&state, scene, label, SECOND);
    CHECK(!state.entities[label_index].text_override && strstr(fl_script_runtime_error(state.scripts,label_index), "UTF-8"));
    CHECK(script(project, scene_id, box,
        "return {sample=function(ctx) ctx.entity.crop.x=0.2;ctx.entity.crop.width=0.5 end}", true));
    sample(&state, scene, box, SECOND);
    const size_t box_index = (size_t)(fl_scene_entity(scene,box)-scene->entities);
    CHECK(state.entities[box_index].crop[0] == 0.2 && state.entities[box_index].crop[2] == 0.5);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = scene_id, .entity = label}, NULL));
    /* A corrected source recovers immediately after compilation/runtime/memory errors. */
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(script(project,scene_id,box,motion,false));
    CHECK(sample(&state,scene,box,SECOND)==25);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_ANIMATION}},NULL));
    CHECK(!fl_entity_animation(fl_scene_entity(scene,box)) && !fl_scene_track_count(scene));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_SCRIPT}},NULL));
    CHECK(sample(&state,scene,box,SECOND)==25);
    Fl_Component invalid={0};
    CHECK(!fl_script_encode(&(Fl_Script){.script=0,.enabled=true},&invalid));
    CHECK(!fl_script_source_valid("",0) && !fl_script_source_valid("a\0b",3) && fl_script_source_valid("x",1));
    CHECK(!fl_source_asset_name_valid("") && !fl_source_asset_name_valid("a\nb") && fl_source_asset_name_valid("Counter"));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene_id,.entity=box},NULL));
    CHECK(scene->entity_count==0 && fl_scene_track_count(scene)==0);
    CHECK(fl_project_save(project));
    fl_scene_state_clear(&state);
    repository(project, scene_id, argv[1]);
    return FLOW_TEST_RESULT("fl_script_tests");
}
