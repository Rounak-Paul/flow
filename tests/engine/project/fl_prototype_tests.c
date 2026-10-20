#include "flow_test.h"
#include "project/fl_project.h"
#include "project/fl_prototype.h"
#include "project/fl_source_asset.h"
#include "project/fl_asset_import.h"
#include "project/fl_history.h"
#include "components/fl_script.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <stdlib.h>
#include <string.h>

/* Encode a borrowed hierarchy into standalone prototype source; caller frees. */
static char *source_of(const Fl_Scene *scene, Fl_Id root)
{
    Fl_Prototype prototype={.root=root,.next_id=100000,.next_component_id=100000,.scope=*scene};
    Fl_Json context={0}; cJSON *json=fl_prototype_write(&context,&prototype);
    char *source=json ? cJSON_PrintUnformatted(json) : NULL; cJSON_Delete(json);
    CHECK(source && !context.error[0]); return source;
}

/* Reject one malformed document while leaving the output safe to release. */
static void rejected(cJSON *json)
{
    char *source=cJSON_PrintUnformatted(json); Fl_Json context={0}; Fl_Prototype prototype={0};
    CHECK(source && !fl_prototype_read(&context,source,strlen(source),&prototype));
    CHECK(context.error[0] && !prototype.scope.entities);
    fl_prototype_clear(&prototype); free(source); cJSON_Delete(json);
}

/* Local identities, rooted hierarchy and time are validated independently of project resource ids. */
static void malformed(const char *source)
{
    Fl_Json context={0}; Fl_Prototype prototype={0};
    CHECK(fl_prototype_read(&context,source,strlen(source),&prototype));
    CHECK(prototype.scope.entity_count==2 && fl_scene_entity(&prototype.scope,prototype.root));
    fl_prototype_clear(&prototype);
    cJSON *json=cJSON_Parse(source); cJSON_AddNumberToObject(json,"canvas",100); rejected(json);
    json=cJSON_Parse(source); cJSON_ReplaceItemInObjectCaseSensitive(json,"duration",cJSON_CreateNumber(0)); rejected(json);
    json=cJSON_Parse(source); cJSON_ReplaceItemInObjectCaseSensitive(json,"next_id",cJSON_CreateNumber(1)); rejected(json);
    json=cJSON_Parse(source); cJSON_ReplaceItemInObjectCaseSensitive(json,"root",cJSON_CreateNumber(9999)); rejected(json);
    json=cJSON_Parse(source); cJSON *entities=cJSON_GetObjectItemCaseSensitive(json,"entities");
    cJSON_ReplaceItemInObjectCaseSensitive(entities->child->next,"parent",cJSON_CreateNumber(0)); rejected(json);
    json=cJSON_Parse(source); entities=cJSON_GetObjectItemCaseSensitive(json,"entities");
    cJSON_ReplaceItemInObjectCaseSensitive(entities->child->next,"id",cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(entities->child,"id"),true)); rejected(json);
    json=cJSON_Parse(source); entities=cJSON_GetObjectItemCaseSensitive(json,"entities");
    cJSON *first=cJSON_GetObjectItemCaseSensitive(entities->child,"components")->child;
    cJSON *second=cJSON_GetObjectItemCaseSensitive(entities->child->next,"components")->child;
    cJSON_ReplaceItemInObjectCaseSensitive(second,"id",cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(first,"id"),true)); rejected(json);
    const char *invalid="{\"version\":1}";
    CHECK(!fl_source_asset_validate(FL_ASSET_PROTOTYPE,invalid,strlen(invalid),NULL,0));
    CHECK(!fl_source_asset_validate(FL_ASSET_PROTOTYPE,"{}\0{}",5,NULL,0));
    CHECK(!fl_source_asset_format(FL_ASSET_IMAGE));
}

/* Source editing, resource retention, history, watching and imports share the ordinary asset owner. */
int main(int argc, char **argv)
{
    CHECK(argc==2); if (argc!=2) return 1;
    Fl_Project *project=fl_project_create(argv[1],"Prototypes",NULL); CHECK(project); if (!project) return 1;
    Fl_Id scene=fl_project_scene_at(project,0)->id,root=0,child=0,lua=0,asset=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Root"},&root));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.parent=root,.name="Child"},&child));
    const char *behavior="return {sample=function(ctx) ctx.entity.transform.x=12 end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Motion",.source=behavior,.source_size=strlen(behavior)},&lua));
    Fl_Component component={0}; CHECK(fl_script_encode(&(Fl_Script){.script=lua,.enabled=true},&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=root,.component=component},NULL));
    fl_component_clear(&component);
    char *source=source_of(fl_project_scene(project,scene),root); malformed(source);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_PROTOTYPE,.name="Title",.source=source,.source_size=strlen(source)},&asset));
    CHECK(fl_project_asset_uses(project,lua)==2);
    char name[FL_SOURCE_NAME_MAX+1];
    CHECK(fl_source_asset_unique_name(project,FL_ASSET_PROTOTYPE,"Title",name) && !strcmp(name,"Title 2"));
    const char *unicode="動き動き動き動き動き動き動き動き動き動き動き動き動き動き動き動き";
    CHECK(fl_source_asset_unique_name(project,FL_ASSET_PROTOTYPE,unicode,name) && fl_source_asset_name_valid(name));
    CHECK(!fl_source_asset_name_valid("bad\xc0\x80"));

    const Fl_Asset *record=fl_project_asset(project,asset);
    CHECK(record && record->kind==FL_ASSET_PROTOTYPE && strstr(record->path,"assets/prototypes/") && strstr(record->path,".flproto.json"));
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=asset,.source=behavior,.source_size=strlen(behavior)},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene,.entity=root,.component={.type=FL_COMPONENT_SCRIPT}},NULL));
    CHECK(fl_project_asset_uses(project,lua)==1 && !fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=lua},NULL));
    CHECK(fl_project_asset(project,lua));
    const char *incompatible="return {parameters={required={type='number'}},sample=function(ctx) end}";
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.asset=lua,.source=incompatible,.source_size=strlen(incompatible)},NULL));
    CHECK(!strcmp(fl_project_asset(project,lua)->source,behavior));
    CHECK(fl_project_save(project));
    char path[FL_PATH_MAX]; CHECK(fl_project_asset_path(project,fl_project_asset(project,asset),path,sizeof(path)));
    fl_project_destroy(project); project=fl_project_open(argv[1],NULL); CHECK(project); if (!project) {free(source); return 1;}
    CHECK(fl_project_asset_uses(project,lua)==1 && !strcmp(fl_project_asset(project,asset)->source,source));
    CHECK(fl_fs_write_atomic(path,"{}",2));
    CHECK(fl_project_reload(project)==FL_RELOAD_INVALID && !strcmp(fl_project_asset(project,asset)->source,source));
    cJSON *json=cJSON_Parse(source), *entities=cJSON_GetObjectItemCaseSensitive(json,"entities");
    cJSON_ReplaceItemInObjectCaseSensitive(entities->child,"name",cJSON_CreateString("New default"));
    char *changed=cJSON_PrintUnformatted(json); cJSON_Delete(json);
    CHECK(changed && fl_fs_write_atomic(path,changed,strlen(changed)));
    CHECK(fl_project_reload(project)==FL_RELOAD_APPLIED && !strcmp(fl_project_asset(project,asset)->source,changed));
    CHECK(fl_history_undo(fl_project_history(project),project,NULL,NULL));
    CHECK(!strcmp(fl_project_asset(project,asset)->source,source));
    CHECK(fl_project_save(project));
    char import_path[FL_PATH_MAX]; CHECK(fl_fs_join(import_path,sizeof(import_path),argv[1],"import.FLPROTO.JSON"));
    CHECK(fl_fs_write_atomic(import_path,changed,strlen(changed)));
    Fl_StoredAsset stored={0}; CHECK(fl_asset_store_file(argv[1],import_path,&stored)==FL_IMPORT_ADDED && stored.kind==FL_ASSET_PROTOTYPE);
    Fl_StoredAsset duplicate={0};
    CHECK(fl_asset_store_file(argv[1],import_path,&duplicate)==FL_IMPORT_ADDED && duplicate.source && duplicate.source!=stored.source);
    char store_path[FL_PATH_MAX]; CHECK(fl_asset_store_path(argv[1],stored.hash,stored.extension,store_path,sizeof(store_path)) && !fl_fs_exists(store_path));
    Fl_Id imported=0; CHECK(fl_asset_record(project,&stored,"Copy",&imported)==FL_IMPORT_ADDED && !stored.source);
    Fl_Id same=0; CHECK(fl_asset_record(project,&duplicate,"Duplicate",&same)==FL_IMPORT_EXISTING && same==imported && !duplicate.source);
    CHECK(!fl_fs_exists(store_path));
    CHECK(imported!=asset && fl_project_asset_uses(project,lua)==2);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=imported},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=asset},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=lua},NULL));
    CHECK(fl_project_save(project));
    free(changed); free(source); fl_project_destroy(project);
    return FLOW_TEST_RESULT("fl_prototype_tests");
}
