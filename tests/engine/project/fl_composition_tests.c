#include "flow_test.h"
#include "project/fl_project.h"
#include "project/fl_prototype.h"
#include "project/fl_prototype_authoring.h"
#include "project/fl_scope_authoring.h"
#include "components/fl_local_composition.h"
#include "components/fl_prototype_instance.h"
#include "components/fl_script.h"
#include "time/fl_composition.h"
#include "time/fl_scene_state.h"
#include "doc/fl_scene_geometry.h"
#include "doc/fl_document.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Make a reusable native hierarchy, optionally including behavior and a nested prototype. */
static char *source(Fl_Id script, Fl_Id nested)
{
    unsigned char root_pose[FL_POSE_BYTES],child_pose[FL_POSE_BYTES],child_bounds[FL_BOUNDS_BYTES],color[4]={255,80,40,255};
    fl_pose_encode(&(Fl_Pose){.sx=1,.sy=1,.sz=1},root_pose);
    fl_pose_encode(&(Fl_Pose){.sx=1,.sy=1,.sz=1},child_pose);
    fl_bounds_encode(&(Fl_Bounds){20,30,0},child_bounds);
    Fl_Component root[2]={{.id=1,.type=FL_COMPONENT_POSE,.version=1,.data=root_pose,.size=FL_POSE_BYTES}}, child[4]={
        {.id=7,.type=FL_COMPONENT_POSE,.version=1,.data=child_pose,.size=FL_POSE_BYTES},
        {.id=2,.type=FL_COMPONENT_BOUNDS,.version=1,.data=child_bounds,.size=FL_BOUNDS_BYTES},
        {.id=3,.type=FL_COMPONENT_RECTANGLE,.version=1,.data=color,.size=4}};
    size_t nr=1,nc=3;
    if (script) {CHECK(fl_script_encode(&(Fl_Script){.script=script,.enabled=true},&child[nc])); child[nc++].id=4;}
    if (nested) {CHECK(fl_prototype_instance_encode(&(Fl_PrototypeInstance){.asset=nested},&root[nr])); root[nr++].id=5;}
    Fl_Entity entities[]={{.id=1,.name="Root",.visible=true,.components=root,.component_count=nr},
        {.id=2,.parent=1,.name="Shape",.visible=true,.components=child,.component_count=nc}};
    Fl_Prototype prototype={.root=1,.next_id=3,.next_component_id=8,.scope={.duration=1200000,.entities=entities,.entity_count=2}};
    Fl_Json context={0}; cJSON *json=fl_prototype_write(&context,&prototype);
    char *text=json ? cJSON_PrintUnformatted(json) : NULL; cJSON_Delete(json);
    if (script) fl_component_clear(&child[3]); if (nested) fl_component_clear(&root[1]);
    CHECK(text && !context.error[0]); return text;
}

/* Store or replace a prototype through the shared atomic document boundary. */
static Fl_Id put(Fl_Project *project, Fl_Id asset, const char *name, const char *text)
{
    Fl_Id created=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_PROTOTYPE,.asset=asset,.name=name,.source=text,.source_size=strlen(text)},&created));
    return asset ? asset : created;
}

/* Attach or replace the singleton live instance component. */
static bool attach(Fl_Project *project, Fl_Id scene, Fl_Id entity, Fl_Id asset, const char *overrides, bool fixed)
{
    Fl_PrototypeInstance instance={.asset=asset,.overrides=overrides,.overrides_size=overrides ? strlen(overrides) : 0,.time=fl_time_mapping_default()};
    if (fixed) instance.time.mode=FL_RETIME_FIXED;
    Fl_Component component={0}; if (!fl_prototype_instance_encode(&instance,&component)) return false;
    bool ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component},NULL);
    fl_component_clear(&component); return ok;
}

/* Find a generated child by stable provenance rather than array position or generated hash. */
static size_t find(const Fl_SceneState *state, Fl_Id owner, Fl_Id local, size_t depth)
{
    for (size_t i=0;i<state->count;++i) {
        const Fl_Origin *origin=fl_composition_origin(state->composition,i);
        if (origin && origin->generated && origin->owner==owner && origin->entity==local && origin->depth==depth) return i;
    }
    return SIZE_MAX;
}

/* Structural Lua uses native typed descriptors and recompiles only with document inputs. */
static void test_build(const char *directory)
{
    char path[FL_PATH_MAX]; CHECK(fl_fs_join(path,sizeof(path),directory,"procedural"));
    Fl_Project *project=fl_project_create(path,"Procedural",NULL); CHECK(project); if (!project) return;
    Fl_Id scene=fl_project_scene_at(project,0)->id,entity=0,asset=0;
    const char *lua="return {parameters={width={type='number',default=40,min=1,max=100}},build=function(ctx) return {version=1,root=1,next_id=2,next_component_id=4,duration=120000,beats={},clips={},entities={{id=1,parent=0,name='Generated',visible=true,components={{id=1,type='pose',version=1,x=0,y=0,z=0,rx=0,ry=0,rz=0,scale_x=1,scale_y=1,scale_z=1,pivot_x=0,pivot_y=0,pivot_z=0},{id=2,type='bounds',version=1,width=ctx.params.width,height=20,depth=0},{id=3,type='rectangle',version=1,color={255,0,0,255}}}}}} end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Generator",.source=lua,.source_size=strlen(lua)},&asset));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Owner"},&entity));
    Fl_Component script={0}; const char *parameters="{\"width\":32}";
    CHECK(fl_script_encode(&(Fl_Script){.script=asset,.enabled=true,.parameters=parameters,.parameters_size=strlen(parameters)},&script));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=script},NULL)); fl_component_clear(&script);
    Fl_SceneState state={0}; CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),12000) && state.count==2);
    Fl_Id generated=0;
    if (state.count==2) {
        const Fl_Scene *flat=fl_scene_state_scene(&state,fl_project_scene(project,scene)); generated=flat->entities[1].id;
        CHECK(state.entities[1].bounds.width==32);
        const Fl_Origin *origin=fl_composition_origin(state.composition,1); CHECK(origin && origin->generated && origin->owner==entity && origin->entity==1);
        CHECK(fl_scene_state_scripts(&state));
    }
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),24000) && state.count==2);
    if (state.count==2) CHECK(fl_scene_state_scene(&state,fl_project_scene(project,scene))->entities[1].id==generated);
    Fl_Id script_id=fl_entity_script_component(fl_scene_entity(fl_project_scene(project,scene),entity),asset)->id;
    parameters="{\"width\":64}";
    CHECK(fl_script_encode(&(Fl_Script){.script=asset,.enabled=true,.parameters=parameters,.parameters_size=strlen(parameters)},&script));
    script.id=script_id;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=script},NULL)); fl_component_clear(&script);
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),24000) && state.count==2 && state.entities[1].bounds.width==64);
    const char *cyclic="return {parameters={width={type='number',default=40}},build=function(ctx) local value={};value.self=value;return value end}";
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=asset,.source=cyclic,.source_size=strlen(cyclic)},NULL));
    CHECK(strstr(fl_project_diagnostic(project),"cycle"));
    const char *unbounded="return {parameters={width={type='number',default=40}},build=function(ctx) while true do end end}";
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=asset,.source=unbounded,.source_size=strlen(unbounded)},NULL));
    CHECK(strstr(fl_project_diagnostic(project),"instruction limit"));
    CHECK(!strcmp(fl_project_asset(project,asset)->source,lua));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),24000));
    Fl_Origin address=*fl_composition_origin(state.composition,1); Fl_Json edit_context={0};
    const char *edit_text="{\"op\":\"component.patch\",\"component\":\"2\",\"value\":{\"width\":55}}";
    cJSON *edit=fl_json_parse(&edit_context,edit_text,strlen(edit_text));
    CHECK(edit && fl_scope_edit(project,scene,&address,edit)); cJSON_Delete(edit);
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),24000) && state.entities[1].bounds.width==55);
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(path,NULL); CHECK(project);
    if (project) {CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),24000) && state.count==2 && state.entities[1].bounds.width==55); fl_scene_state_clear(&state); fl_project_destroy(project);}
}

/* Detach retains native scopes, identities, clocks and deterministic Lua while removing source ownership. */
static void test_detach(const char *directory)
{
    char path[FL_PATH_MAX]; CHECK(fl_fs_join(path,sizeof(path),directory,"detached"));
    Fl_Project *project=fl_project_create(path,"Detached",NULL); CHECK(project); if (!project) return;
    Fl_Id scene=fl_project_scene_at(project,0)->id,lua=0,asset=0,owner=0;
    const char *behavior="return {sample=function(ctx) ctx.entity.pose.x=ctx.time.seconds*10+math.random() end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Motion",.source=behavior,.source_size=strlen(behavior)},&lua));
    char *document=source(lua,0); asset=put(project,0,"Moving shape",document); free(document);
    CHECK(fl_prototype_instantiate(project,scene,0,asset,10,20,&owner));
    const Fl_Component *component=fl_entity_component(fl_scene_entity(fl_project_scene(project,scene),owner),FL_COMPONENT_PROTOTYPE);
    Fl_Id generator=component->id; Fl_PrototypeInstance instance; CHECK(fl_prototype_instance_decode(component,&instance));
    instance.time.mode=FL_RETIME_FIXED; instance.time.rate=2; instance.time.loop=true; instance.time.duration=120000;
    Fl_Component mapped={0}; CHECK(fl_prototype_instance_encode(&instance,&mapped)); mapped.id=generator;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=owner,.component=mapped},NULL)); fl_component_clear(&mapped);
    Fl_SceneState state={0}; Fl_Tick ticks[]={0,30000,90000,150000}; double values[4]; Fl_Id generated=0;
    for (size_t i=0;i<4;++i) {
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),ticks[i]));
        size_t child=find(&state,owner,2,0); CHECK(child!=SIZE_MAX);
        if (child!=SIZE_MAX) {values[i]=state.entities[child].pose.x; generated=fl_scene_state_scene(&state,fl_project_scene(project,scene))->entities[child].id;}
    }
    CHECK(fl_prototype_detach(project,scene,owner));
    const Fl_Entity *entity=fl_scene_entity(fl_project_scene(project,scene),owner);
    CHECK(!fl_entity_component(entity,FL_COMPONENT_PROTOTYPE));
    component=fl_entity_component(entity,FL_COMPONENT_COMPOSITION); CHECK(component && component->id==generator);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=asset},NULL));
    for (size_t i=4;i-->0;) {
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),ticks[i]));
        size_t child=find(&state,owner,2,0); CHECK(child!=SIZE_MAX);
        if (child!=SIZE_MAX) {
            CHECK(state.entities[child].pose.x==values[i]);
            CHECK(fl_scene_state_scene(&state,fl_project_scene(project,scene))->entities[child].id==generated);
        }
    }
    size_t child=find(&state,owner,2,0);
    if (child!=SIZE_MAX) {
        Fl_Origin address=*fl_composition_origin(state.composition,child); Fl_Id local=0;
        const Fl_Entity *shape=&fl_scene_state_scene(&state,fl_project_scene(project,scene))->entities[child];
        CHECK(fl_composition_component_local(state.composition,child,fl_entity_component(shape,FL_COMPONENT_BOUNDS)->id,&local) && local==2);
        Fl_Json context={0}; const char *text="{\"op\":\"component.patch\",\"component\":\"2\",\"value\":{\"height\":77}}";
        cJSON *edit=fl_json_parse(&context,text,strlen(text)); CHECK(edit && fl_scope_edit(project,scene,&address,edit)); cJSON_Delete(edit);
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),0)); child=find(&state,owner,2,0);
        CHECK(child!=SIZE_MAX && state.entities[child].bounds.height==77);
        unsigned char opacity_bytes[8]; fl_opacity_encode(0.75,opacity_bytes);
        Fl_Component opacity={.type=FL_COMPONENT_OPACITY,.version=1,.data=opacity_bytes,.size=8};
        for (size_t i=0;i<16;++i) {
            CHECK(fl_scope_configure(project,scene,state.composition,child,"Detached shape",true,&opacity,1,0,0));
            CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),0)); child=find(&state,owner,2,0);
            CHECK(child!=SIZE_MAX && state.entities[child].opacity==0.75);
        }
        Fl_LocalComposition owned; CHECK(fl_local_composition_decode(fl_entity_component(fl_scene_entity(fl_project_scene(project,scene),owner),FL_COMPONENT_COMPOSITION),&owned));
        cJSON *operations=fl_json_parse(&context,owned.overrides,owned.overrides_size);
        CHECK(operations && cJSON_GetArraySize(operations)<=6); cJSON_Delete(operations);
    }
    child=find(&state,owner,2,0);
    if (child!=SIZE_MAX) {
        const Fl_Scene *flat=fl_composition_scene(state.composition); Fl_Id target=flat->entities[child].id,key=0;
        CHECK(fl_scope_timeline(project,scene,state.composition,child,&(Fl_Op){.kind=FL_OP_KEY_SET,.entity=target,.property=FL_PROP_Y,.anchor={.kind=FL_ANCHOR_ABSOLUTE,.offset=30000},.value={45}},&key));
        CHECK(key>>63); CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),15000)); child=find(&state,owner,2,0);
        CHECK(child!=SIZE_MAX && state.entities[child].pose.y==45);
        Fl_Tick root_tick=0; CHECK(fl_composition_entity_root_tick(state.composition,child,30000,15000,&root_tick) && root_tick==15000);
        Fl_Id beat=0;
        CHECK(fl_scope_timeline(project,scene,state.composition,child,&(Fl_Op){.kind=FL_OP_BEAT_ADD,.name="Extra",.beat_duration=12000},&beat));
        CHECK(beat>>63); CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),15000)); child=find(&state,owner,2,0);
        CHECK(fl_composition_entity_scope(state.composition,child)->beat_count==2);
    }
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(path,NULL); CHECK(project);
    if (project) {
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),30000)); child=find(&state,owner,2,0);
        CHECK(child!=SIZE_MAX && state.entities[child].bounds.height==77 && state.entities[child].pose.x==values[1]);
        fl_scene_state_clear(&state); fl_project_destroy(project);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc==2); if (argc!=2) return 1;
    Fl_Project *project=fl_project_create(argv[1],"Composition",NULL); CHECK(project); if (!project) return 1;
    Fl_Id scene=fl_project_scene_at(project,0)->id,script=0,a=0,b=0;
    const char *lua="return {initialize=function(ctx) ctx.state={n=0} end,step=function(ctx) ctx.state.n=ctx.state.n+ctx.time.dt end,sample=function(ctx) ctx.entity.pose.x=ctx.state.n end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Clock",.source=lua,.source_size=strlen(lua)},&script));
    char *base=source(script,0); Fl_Id asset=put(project,0,"Shape",base);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="A"},&a));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="B"},&b));
    CHECK(attach(project,scene,a,asset,"[{\"op\":\"component.patch\",\"path\":[],\"entity\":2,\"component\":2,\"value\":{\"width\":45}}]",false));
    CHECK(attach(project,scene,b,asset,NULL,true));
    Fl_SceneState state={0}; CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
    CHECK(state.count==6 && !state.error[0]);
    size_t ai=find(&state,a,2,0),bi=find(&state,b,2,0); CHECK(ai!=SIZE_MAX && bi!=SIZE_MAX);
    if (ai!=SIZE_MAX && bi!=SIZE_MAX) {
        CHECK(fabs(state.entities[ai].pose.x-2)<1e-8 && fabs(state.entities[bi].pose.x-1)<1e-8);
        CHECK(state.entities[ai].bounds.width==45 && state.entities[bi].bounds.width==20);
        const Fl_Scene *flat=fl_scene_state_scene(&state,fl_project_scene(project,scene));
        CHECK(flat->entities[ai].id!=flat->entities[bi].id);
        Fl_Id stable=flat->entities[ai].id;
        const Fl_Scene *authored=fl_project_scene(project,scene);
        Fl_Scene collision=*authored;
        Fl_Entity *collision_entities=malloc(authored->entity_count*sizeof(*collision_entities));
        CHECK(collision_entities);
        if (collision_entities) {
            memcpy(collision_entities,authored->entities,authored->entity_count*sizeof(*collision_entities));
            size_t count=collision_entities[0].component_count;
            Fl_Component *components=malloc(count*sizeof(*components)); CHECK(components);
            if (components) {
                memcpy(components,collision_entities[0].components,count*sizeof(*components));
                components[0].id=flat->entities[ai].components[0].id;
                collision_entities[0].components=components; collision.entities=collision_entities;
                char diagnostic[256];
                Fl_Composition *invalid=fl_composition_create(&collision,NULL,diagnostic,sizeof(diagnostic));
                CHECK(!invalid && strstr(diagnostic,"component identity collision")); fl_composition_destroy(invalid);
                free(components);
            }
            free(collision_entities);
        }

        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),300000));
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
        CHECK(fabs(state.entities[ai].pose.x-2)<1e-8 && fl_composition_scene(state.composition)->entities[ai].id==stable);
        Fl_SceneGeometry geometry={0}; CHECK(fl_scene_geometry_update(&geometry,fl_project_scene(project,scene),&state) && geometry.count==6);
        fl_scene_geometry_clear(&geometry);
    }
    cJSON *updated=cJSON_Parse(base), *records=cJSON_GetObjectItemCaseSensitive(updated,"entities");
    cJSON *shape=records->child->next, *bounds=NULL;
    for (cJSON *component=cJSON_GetObjectItemCaseSensitive(shape,"components")->child;component;component=component->next) {
        const cJSON *type=cJSON_GetObjectItemCaseSensitive(component,"type");
        if (cJSON_IsString(type) && !strcmp(type->valuestring,"bounds")) bounds=component;
    }
    CHECK(bounds && cJSON_ReplaceItemInObjectCaseSensitive(bounds,"height",cJSON_CreateNumber(60)));
    char *defaults=cJSON_PrintUnformatted(updated); CHECK(put(project,asset,NULL,defaults)==asset);
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
    ai=find(&state,a,2,0); CHECK(ai!=SIZE_MAX);
    if (ai!=SIZE_MAX) CHECK(state.entities[ai].bounds.width==45 && state.entities[ai].bounds.height==60);
    cJSON_Delete(cJSON_DetachItemViaPointer(records,shape)); char *removed=cJSON_PrintUnformatted(updated);
    CHECK(put(project,asset,NULL,removed)==asset);
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000) && fl_composition_unresolved(state.composition)==1);
    CHECK(put(project,asset,NULL,base)==asset);
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000)); ai=find(&state,a,2,0);
    CHECK(ai!=SIZE_MAX && !fl_composition_unresolved(state.composition));
    if (ai!=SIZE_MAX) CHECK(state.entities[ai].bounds.width==45);
    cJSON_Delete(updated); free(defaults); free(removed);
    updated=cJSON_Parse(base); records=cJSON_GetObjectItemCaseSensitive(updated,"entities");
    cJSON *addition=cJSON_Duplicate(records->child->next,true);
    cJSON_ReplaceItemInObjectCaseSensitive(addition,"id",fl_json_u64(UINT64_C(0x8000000000000001)));
    uint64_t extension=UINT64_C(0x8000000000000001);
    for (cJSON *v=cJSON_GetObjectItemCaseSensitive(addition,"components")->child;v;v=v->next) cJSON_ReplaceItemInObjectCaseSensitive(v,"id",fl_json_u64(extension++));
    cJSON *operations=cJSON_CreateArray(), *operation=cJSON_CreateObject(); cJSON_AddItemToArray(operations,operation);
    cJSON_AddStringToObject(operation,"op","entity.add"); cJSON_AddArrayToObject(operation,"path");
    cJSON_AddItemToObject(operation,"entity",fl_json_u64(UINT64_C(0x8000000000000001))); cJSON_AddItemToObject(operation,"value",addition);
    char *extra=cJSON_PrintUnformatted(operations);
    CHECK(fl_prototype_set_overrides(project,scene,a,extra,strlen(extra)));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000) && state.count==7);
    CHECK(find(&state,a,UINT64_C(0x8000000000000001),0)!=SIZE_MAX);
    free(extra); cJSON_Delete(operations); cJSON_Delete(updated);
    CHECK(attach(project,scene,a,asset,"[{\"op\":\"component.patch\",\"path\":[],\"entity\":99,\"component\":2,\"value\":{\"width\":45}}]",false));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),0) && fl_composition_unresolved(state.composition)==1);
    CHECK(!attach(project,scene,a,asset,"[{\"op\":\"component.patch\",\"path\":[],\"entity\":2,\"component\":2,\"value\":{\"width\":-1}}]",false));
    CHECK(attach(project,scene,a,asset,"[{\"op\":\"component.patch\",\"path\":[],\"entity\":2,\"component\":2,\"value\":{\"width\":-1}},{\"op\":\"component.patch\",\"path\":[],\"entity\":2,\"component\":2,\"value\":{\"width\":45}}]",false));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),0)); ai=find(&state,a,2,0);
    CHECK(ai!=SIZE_MAX && state.entities[ai].bounds.width==45);
    CHECK(attach(project,scene,a,asset,"[{\"op\":\"entity.remove\",\"path\":[],\"entity\":2}]",false));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),0) && state.count==5);
    char *outer_source=source(0,asset); Fl_Id outer=put(project,0,"Nested",outer_source);
    CHECK(attach(project,scene,a,outer,"[{\"op\":\"component.patch\",\"path\":[5],\"entity\":2,\"component\":2,\"value\":{\"height\":77}}]",false));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
    size_t nested=find(&state,a,2,1); CHECK(nested!=SIZE_MAX);
    if (nested!=SIZE_MAX) CHECK(state.entities[nested].bounds.height==77);
    updated=cJSON_Parse(outer_source); records=cJSON_GetObjectItemCaseSensitive(updated,"entities");
    cJSON *second_instance=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(records->child,"components")->child->next,true);
    CHECK(second_instance && cJSON_ReplaceItemInObjectCaseSensitive(second_instance,"id",fl_json_u64(6)));
    CHECK(cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(records->child->next,"components"),second_instance));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(updated,"next_component_id",fl_json_u64(8)));
    char *siblings=cJSON_PrintUnformatted(updated); CHECK(siblings && put(project,outer,NULL,siblings)==outer);
    free(siblings); cJSON_Delete(updated);
    CHECK(attach(project,scene,a,outer,"[{\"op\":\"component.patch\",\"path\":[5],\"entity\":2,\"component\":2,\"value\":{\"height\":77}},{\"op\":\"component.patch\",\"path\":[6],\"entity\":2,\"component\":2,\"value\":{\"height\":66}}]",false));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
    size_t matched=0;
    for (size_t i=0;i<state.count;++i) {
        const Fl_Origin *origin=fl_composition_origin(state.composition,i);
        if (!origin || origin->owner!=a || origin->entity!=2 || origin->depth!=1) continue;
        CHECK((origin->path[0]==5 && state.entities[i].bounds.height==77) || (origin->path[0]==6 && state.entities[i].bounds.height==66));
        ++matched;
    }
    CHECK(matched==2 && !fl_composition_unresolved(state.composition));
    char *cycle=source(0,outer);
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_PROTOTYPE,.asset=asset,.source=cycle,.source_size=strlen(cycle)},NULL));
    CHECK(!strcmp(fl_project_asset(project,asset)->source,base));
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(argv[1],NULL); CHECK(project);
    if (project) {
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
        nested=find(&state,a,2,1); CHECK(nested!=SIZE_MAX);
        if (nested!=SIZE_MAX) CHECK(state.entities[nested].bounds.height==77);
        Fl_Id captured=0,placed=0;
        CHECK(fl_prototype_capture(project,scene,a,"Captured",&captured));
        CHECK(fl_prototype_instantiate(project,scene,0,captured,100,200,&placed));
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000));
        const Fl_Scene *flat=fl_scene_state_scene(&state,fl_project_scene(project,scene));
        const Fl_Entity *owner=fl_scene_entity(flat,placed); CHECK(owner);
        if (owner) CHECK(state.entities[owner-flat->entities].pose.x==100 && state.entities[owner-flat->entities].pose.y==200);
        CHECK(fl_prototype_set_overrides(project,scene,placed,"[]",2));
        CHECK(!fl_prototype_capture(project,scene,UINT64_MAX,"Missing",&captured));
        atomic_bool cancel; atomic_init(&cancel,true);
        CHECK(!fl_scene_state_update_cancel(&state,fl_project_scene(project,scene),240000,&cancel));
        fl_scene_state_clear(&state); fl_project_destroy(project);
    }
    free(cycle); free(outer_source); free(base);
    test_build(argv[1]);
    test_detach(argv[1]);
    return FLOW_TEST_RESULT("fl_composition_tests");
}
