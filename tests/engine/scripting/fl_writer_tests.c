#include "flow_test.h"
#include "components/fl_script.h"
#include "project/fl_project.h"
#include "time/fl_scene_state.h"
#include <math.h>
#include <string.h>

/* Store an ordinary module through the same source validation used by files and MCP. */
static Fl_Id module(Fl_Project *project, const char *name, const char *source)
{
    Fl_Id id=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name=name,.source=source,.source_size=strlen(source)},&id));
    return id;
}

/* Attach a module with explicit writer layer and optional named target wiring. */
static bool attach(Fl_Project *project, Fl_Id scene, Fl_Id entity, Fl_Id asset, int32_t layer, Fl_Id target, Fl_Id *id)
{
    char bindings[128]; snprintf(bindings,sizeof(bindings),"{\"target\":{\"type\":\"entity\",\"id\":\"%llu\"}}",(unsigned long long)target);
    Fl_Script script={.script=asset,.enabled=true,.layer=layer,.bindings=target ? bindings : NULL,.bindings_size=target ? strlen(bindings) : 0};
    Fl_Component component={0}; if (!fl_script_encode(&script,&component)) return false;
    bool ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component},id);
    fl_component_clear(&component); return ok;
}

int main(int argc, char **argv)
{
    CHECK(argc==2); if (argc!=2) return FLOW_TEST_RESULT("fl_writer_tests");
    Fl_Project *project=fl_project_create(argv[1],"Writers",NULL); CHECK(project); if (!project) return FLOW_TEST_RESULT("fl_writer_tests");
    Fl_Id scene=fl_project_scene_at(project,0)->id,owner=0,target=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Owner"},&owner));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Target"},&target));
    unsigned char bytes[56]; fl_transform_encode(&(Fl_Transform){.x=10,.width=100,.height=100,.scale_x=1,.scale_y=1},bytes);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=owner,.component={.type=FL_COMPONENT_TRANSFORM,.version=1,.data=bytes,.size=56}},NULL));
    const char *low="return {bindings={target={type='entity'}},outputs={self={x='add'},target={y='replace'}},sample=function(ctx) ctx.entity.transform.x=5;ctx.outputs.target.transform.y=17 end}";
    Fl_Id low_asset=module(project,"Add and target",low);
    Fl_Id high_asset=module(project,"Multiply","return {outputs={self={x='multiply'}},sample=function(ctx) ctx.entity.transform.x=2 end}");
    Fl_Id peer_asset=module(project,"Coherent peer","return {outputs={self={y='replace'}},sample=function(ctx) ctx.entity.transform.y=ctx.entity.transform.x end}");
    Fl_Id high=0,first=0,peer=0;
    CHECK(attach(project,scene,owner,high_asset,2,0,&high));
    CHECK(attach(project,scene,owner,low_asset,1,target,&first));
    CHECK(attach(project,scene,owner,peer_asset,1,0,&peer));
    Fl_SceneState state={0}; CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
    CHECK(state.entities[0].transform.x==30 && state.entities[0].transform.y==10 && state.entities[1].transform.y==17);
    CHECK(!attach(project,scene,owner,high_asset,1,0,NULL));
    CHECK(strstr(fl_project_diagnostic(project),"Ambiguous writers"));
    Fl_Id reader_asset=module(project,"Sample dependency","return {bindings={target={type='entity'}},reads={target={x=true}},outputs={self={x='replace'}},sample=function(ctx) assert(ctx.inputs.target.transform.y==nil);ctx.entity.transform.x=ctx.inputs.target.transform.x+3 end}");
    Fl_Id reader=0; CHECK(attach(project,scene,target,reader_asset,3,owner,&reader));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
    CHECK(state.entities[1].transform.x==33);
    CHECK(!attach(project,scene,target,reader_asset,0,owner,NULL));
    CHECK(strstr(fl_project_diagnostic(project),"dependency"));
    const char *cycle="return {bindings={target={type='entity'}},reads={target={x=true}},outputs={self={x='add'},target={y='replace'}},sample=function(ctx) end}";
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=low_asset,.source=cycle,.source_size=strlen(cycle)},NULL));
    CHECK(strstr(fl_project_diagnostic(project),"dependency"));
    const char *bad="return {bindings={target={type='entity'}},outputs={self={x='add'},target={y='replace'}},sample=function(ctx) ctx.entity.transform.x=5;ctx.outputs.target.transform.y=17;ctx.entity.transform.width=999 end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=low_asset,.source=bad,.source_size=strlen(bad)},NULL));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
    CHECK(state.entities[0].transform.x==10 && state.entities[0].transform.y==10 && state.entities[1].transform.y==0);
    CHECK(strstr(fl_script_runtime_component_error(fl_scene_state_scripts(&state),first),"undeclared"));
    CHECK(strstr(fl_script_runtime_component_error(fl_scene_state_scripts(&state),high),"Upstream"));
    CHECK(strstr(fl_script_runtime_component_error(fl_scene_state_scripts(&state),reader),"Upstream"));
    CHECK(state.entities[1].transform.x==0);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=low_asset,.source=low,.source_size=strlen(low)},NULL));
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(argv[1],NULL); CHECK(project);
    if (project) {
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
        CHECK(state.entities[0].transform.x==30 && state.entities[0].transform.y==10 && state.entities[1].transform.y==17);
        CHECK(state.entities[1].transform.x==33);
        fl_scene_state_clear(&state); fl_project_destroy(project);
    }
    return FLOW_TEST_RESULT("fl_writer_tests");
}
