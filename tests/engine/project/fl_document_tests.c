#include "flow_test.h"
#include "doc/fl_document.h"
#include "doc/fl_document_store.h"
#include "components/fl_script.h"
#include "project/fl_history.h"
#include "project/fl_asset_import.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "time/fl_scene_state.h"
#include <stdlib.h>
#include <string.h>

/* Load a test document tree with the production strict parser. */
static cJSON *load(const char *path)
{
    Fl_Json context={0}; size_t size=0; char *text=fl_fs_read_file(path,FL_DOCUMENT_LIMIT,&size);
    cJSON *root=text ? fl_json_parse(&context,text,size) : NULL;
    free(text); CHECK(root); return root;
}

/* Verify strict syntax and exact integer limits independent of project semantics. */
static void syntax(void)
{
    const char *invalid[]={"", "{\"x\":1,\"x\":2}", "{\"x\":01}", "{\"x\":1.}", "{\"x\":+1}", "{\"x\":1e}", "{\"x\":1e999}",
        "{\"x\":\"a\nb\"}", "{\"x\":\"\\u0000\"}", "{\"x\":\"\\ud800\"}", "[1,]", "{} false", "{\"x\":\"\xc0\x80\"}"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        Fl_Json c={0}; cJSON *root=fl_json_parse(&c,invalid[i],strlen(invalid[i])); CHECK(!root && c.error[0]); cJSON_Delete(root);
    }
    Fl_Json c={0}; uint64_t u=0; int64_t n=0;
    cJSON *v=fl_json_u64(UINT64_MAX); CHECK(v && fl_json_uint(&c,v,&u) && u==UINT64_MAX); cJSON_Delete(v);
    v=fl_json_i64(INT64_MIN); CHECK(v && fl_json_int(&c,v,&n) && n==INT64_MIN); cJSON_Delete(v);
    v=cJSON_CreateString("18446744073709551616"); CHECK(!fl_json_uint(&c,v,&u)); cJSON_Delete(v);
    c=(Fl_Json){0}; v=cJSON_CreateString("-1"); CHECK(!fl_json_uint(&c,v,&u)); cJSON_Delete(v);
    c=(Fl_Json){0}; v=cJSON_CreateNumber(9007199254740992.0); CHECK(!fl_json_uint(&c,v,&u)); cJSON_Delete(v);
    CHECK(!fl_document_path_valid("../outside.json") && !fl_document_path_valid("assets/../outside.lua") && !fl_document_path_valid("/absolute"));
}

/* Count coherent accepted publications, retaining reentrant mutation protection. */
static bool changed(const Fl_Event *event, void *data)
{
    ++*(size_t *)data; const Fl_ProjectChange *change=event->payload;
    CHECK(!fl_project_apply(change->project,&(Fl_Op){.kind=FL_OP_SCENE_ADD,.name="Reentrant"},NULL));
    CHECK(fl_project_reload(change->project)==FL_RELOAD_INVALID);
    return false;
}

/* Source and instance inputs publish as one executable generation, including dry-run and rejected edits. */
static void behavior_changesets(const char *root)
{
    char directory[FL_PATH_MAX]; CHECK(fl_fs_join(directory,sizeof(directory),root,"behavior_changesets"));
    Fl_Project *project=fl_project_create(directory,"Behavior changesets",NULL); CHECK(project); if (!project) return;
    Fl_Id scene=fl_project_scene_at(project,0)->id, entity=0, asset=0, instance=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Object"},&entity));
    const char *first="return {parameters={speed={type='number',default=1}},sample=function(ctx) ctx.entity.transform.x=ctx.params.speed end}";
    const char *second="return {parameters={distance={type='number',default=1}},sample=function(ctx) ctx.entity.transform.x=ctx.params.distance end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Motion",.source=first,.source_size=strlen(first)},&asset));
    Fl_Script script={.script=asset,.enabled=true,.parameters="{\"speed\":2}",.parameters_size=11};
    Fl_Component component={0}; CHECK(fl_script_encode(&script,&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component},&instance));
    fl_component_clear(&component);
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.asset =asset,.source=second,.source_size=strlen(second)},NULL));
    CHECK(!strcmp(fl_project_asset(project,asset)->source,first) && fl_project_diagnostic(project)[0]);
    CHECK(fl_project_edit_begin(project,"Update behavior interface",false));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.asset =asset,.source=second,.source_size=strlen(second)},NULL));
    script.parameters="{\"distance\":3}"; script.parameters_size=14;
    CHECK(fl_script_encode(&script,&component)); component.id=instance;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component},NULL));
    CHECK(fl_project_edit_validate(project) && fl_project_edit_commit(project)); fl_component_clear(&component);
    Fl_SceneState state={0}; CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),0));
    CHECK(state.entities[0].transform.x==3);
    script.parameters="{\"distance\":\"fast\"}"; script.parameters_size=19;
    CHECK(fl_script_encode(&script,&component)); component.id=instance;
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component},NULL));
    CHECK(fl_project_edit_begin(project,"Native preview",true));
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component},NULL));
    CHECK(fl_project_edit_cancel(project)); fl_component_clear(&component);
    CHECK(fl_project_save(project)); uint64_t identity=fl_project_identity(project);
    fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(directory,NULL); CHECK(project);
    if (project) {
        CHECK(fl_project_identity(project)!=identity && fl_scene_state_update(&state,fl_project_scene(project,scene),0));
        CHECK(state.entities[0].transform.x==3); fl_scene_state_clear(&state); fl_project_destroy(project);
    }
}

/* Exercise file/editor/source round trips, invalid-save recovery and concurrent-edit rejection. */
int main(int argc, char **argv)
{
    if (argc!=2) return 2;
    syntax();
    behavior_changesets(argv[1]);
    Fl_EventBus *bus=fl_event_bus_create(NULL); CHECK(bus);
    Fl_Project *project=fl_project_create(argv[1],"Readable",bus); CHECK(project); if (!project) return 1;
    size_t notifications=0;
    CHECK(fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_event_type_find(bus,FL_EVENT_PROJECT_CHANGED),.fn=changed,.user_data=&notifications,.owner=&notifications}).generation);
    const Fl_Scene *scene=fl_project_scene_at(project,0); Fl_Id scene_id=scene->id, entity=0, asset=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene_id,.name="Title"},&entity));
    const char *source="return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=10*t end }";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Entrance",.source=source,.source_size=strlen(source)},&asset));
    Fl_Component component={0}; CHECK(fl_script_encode(&(Fl_Script){.script=asset,.enabled=true},&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity,.component=component},NULL)); fl_component_clear(&component);
    CHECK(fl_project_save(project)); CHECK(fl_project_reload(project)==FL_RELOAD_UNCHANGED);
    char scene_path[FL_PATH_MAX], script_path[FL_PATH_MAX];
    CHECK(fl_fs_join(scene_path,sizeof(scene_path),argv[1],"scenes/0000000000000001.scene.json"));
    CHECK(fl_project_asset_path(project,fl_project_asset(project,asset),script_path,sizeof(script_path)));
    cJSON *root=load(scene_path); CHECK(!cJSON_GetObjectItemCaseSensitive(root,"scripts"));
    cJSON *entities=cJSON_GetObjectItemCaseSensitive(root,"entities");
    cJSON *components=cJSON_GetObjectItemCaseSensitive(entities->child,"components");
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(components->child,"type")->valuestring,"transform"));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root,"name",cJSON_CreateString("Externally edited")));
    Fl_Json context={0}; CHECK(fl_json_save(&context,scene_path,root)); cJSON_Delete(root);
    size_t before=notifications; uint64_t generation=fl_project_generation(project);
    CHECK(fl_project_reload(project)==FL_RELOAD_APPLIED && notifications==before+1);
    CHECK(fl_project_generation(project)==generation+1 && !fl_project_dirty(project));
    scene=fl_project_scene(project,scene_id); CHECK(scene && !strcmp(scene->name,"Externally edited") && scene->project==project);
    size_t valid_size=0; char *valid=fl_fs_read_file(scene_path,FL_DOCUMENT_LIMIT,&valid_size); CHECK(valid);
    CHECK(fl_fs_write_atomic(scene_path,"{",1)); generation=fl_project_generation(project);
    CHECK(fl_project_reload(project)==FL_RELOAD_INVALID && fl_project_generation(project)==generation && fl_project_diagnostic(project)[0]);
    CHECK(!strcmp(fl_project_scene(project,scene_id)->name,"Externally edited"));
    CHECK(fl_fs_write_atomic(scene_path,valid,valid_size)); free(valid);
    CHECK(fl_project_reload(project)==FL_RELOAD_UNCHANGED);
    const char *updated="return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x=200*t end }";
    CHECK(fl_fs_write_atomic(script_path,updated,strlen(updated)));
    CHECK(fl_project_reload(project)==FL_RELOAD_APPLIED);
    CHECK(!strcmp(fl_project_asset(project,asset)->source,updated));
    Fl_SceneState state={0}; CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene_id),FL_TICKS_PER_SECOND)); CHECK(state.entities[0].transform.x==200);
    const char *invalid_module="return {sample=123}";
    generation=fl_project_generation(project);
    CHECK(fl_fs_write_atomic(script_path,invalid_module,strlen(invalid_module)));
    CHECK(fl_project_reload(project)==FL_RELOAD_INVALID && fl_project_generation(project)==generation);
    CHECK(strstr(fl_project_diagnostic(project),"sample must be a function") && !strcmp(fl_project_asset(project,asset)->source,updated));
    CHECK(fl_fs_write_atomic(script_path,"function(",9));
    CHECK(fl_project_reload(project)==FL_RELOAD_INVALID && !strcmp(fl_project_asset(project,asset)->source,updated));
    CHECK(fl_fs_write_atomic(script_path,updated,strlen(updated))); CHECK(fl_project_reload(project)==FL_RELOAD_UNCHANGED);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_RENAME,.scene=scene_id,.name="Local pending"},NULL));
    CHECK(fl_fs_write_atomic(script_path,source,strlen(source)));
    CHECK(fl_project_reload(project)==FL_RELOAD_CONFLICT && !fl_project_save(project));
    CHECK(!strcmp(fl_project_scene(project,scene_id)->name,"Local pending"));
    size_t disk_size=0; char *disk=fl_fs_read_file(script_path,FL_SCRIPT_SOURCE_LIMIT,&disk_size); CHECK(disk && disk_size==strlen(source) && !memcmp(disk,source,disk_size)); free(disk);
    fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(argv[1],bus); CHECK(project && !strcmp(fl_project_scene(project,scene_id)->name,"Externally edited"));
    char imported[FL_PATH_MAX]; CHECK(fl_fs_join(imported,sizeof(imported),argv[1],"custom.lua"));
    const char *custom="return { sample=function() end }"; CHECK(fl_fs_write_atomic(imported,custom,strlen(custom)));
    Fl_StoredAsset stored; CHECK(fl_asset_store_file(argv[1],imported,&stored)==FL_IMPORT_ADDED && stored.kind==FL_ASSET_LUA);
    Fl_Id imported_id=0; CHECK(fl_asset_record(project,&stored,"Custom",&imported_id)==FL_IMPORT_ADDED);
    const Fl_Asset *imported_asset=fl_project_asset(project,imported_id);
    CHECK(imported_asset && imported_asset->kind==FL_ASSET_LUA && !strcmp(imported_asset->source,custom));
    CHECK(fl_project_save(project));
    /* An interrupted save can be retried in the same live host, not just after reopening. */
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_RENAME,.scene=scene_id,.name="Recovered live"},NULL));
    char blocked[FL_PATH_MAX], moved[FL_PATH_MAX];
    CHECK(fl_fs_join(blocked,sizeof(blocked),argv[1],"scenes/0000000000000001.scene.json.tmp") && fl_fs_mkdir_p(blocked));
    CHECK(!fl_project_save(project) && fl_project_dirty(project));
    CHECK(fl_fs_join(moved,sizeof(moved),argv[1],"blocked-scene-temp") && fl_fs_rename(blocked,moved));
    CHECK(fl_project_save(project) && !fl_project_dirty(project));
    /* Atomic batches publish once and failed batches publish no intermediate state. */
    size_t accepted=notifications;
    CHECK(fl_project_edit_begin(project,"Atomic pair",false));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_RENAME,.scene=scene_id,.name="Intermediate"},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_RENAME,.scene=scene_id,.name="Final"},NULL));
    CHECK(notifications==accepted && fl_project_edit_commit(project) && notifications==accepted+1);
    accepted=notifications;
    CHECK(fl_project_edit_begin(project,"Cancelled",false));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_RENAME,.scene=scene_id,.name="Never published"},NULL));
    CHECK(fl_project_edit_cancel(project) && notifications==accepted && !strcmp(fl_project_scene(project,scene_id)->name,"Final"));
    fl_project_destroy(project); fl_event_unsubscribe_owner(bus,&notifications); CHECK(fl_event_bus_destroy(bus));
    return FLOW_TEST_RESULT("fl_document_tests");
}
