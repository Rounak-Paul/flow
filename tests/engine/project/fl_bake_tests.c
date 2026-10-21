#include "flow_test.h"
#include "project/fl_bake.h"
#include "project/fl_history.h"
#include "project/fl_prototype_authoring.h"
#include "project/fl_scope_authoring.h"
#include "components/fl_prototype_instance.h"
#include "components/fl_script.h"
#include "time/fl_scene_state.h"
#include "core/fl_thread.h"
#include <math.h>
#include <string.h>

/* Wait a bounded time for an immutable worker result without driving the native UI. */
static Fl_BakeStatus finished(Fl_Bake *bake)
{
    Fl_Mutex mutex; Fl_CondVar condition;
    CHECK(fl_mutex_init(&mutex)); CHECK(fl_cond_init(&condition)); fl_mutex_lock(&mutex);
    for (unsigned i=0;i<5000 && fl_bake_status(bake)==FL_BAKE_RUNNING;++i) fl_cond_wait_ms(&condition,&mutex,1);
    fl_mutex_unlock(&mutex); fl_cond_destroy(&condition); fl_mutex_destroy(&mutex);
    return fl_bake_status(bake);
}

/* Exercise native capture, replacement, persistence, undo and stale-result protection. */
int main(int argc, char **argv)
{
    CHECK(argc==2); if (argc!=2) return FLOW_TEST_RESULT("fl_bake_tests");
    Fl_Project *project=fl_project_create(argv[1],"Bake",NULL); CHECK(project);
    if (!project) return FLOW_TEST_RESULT("fl_bake_tests");
    Fl_Id scene=fl_project_scene_at(project,0)->id,entity=0,asset=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Motion"},&entity));
    const char *source="return {outputs={self={x='replace'}},sample=function(ctx) ctx.entity.transform.x=ctx.time.seconds*20 end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Motion",.source=source,.source_size=strlen(source)},&asset));
    Fl_Component script={0}; CHECK(fl_script_encode(&(Fl_Script){.script=asset,.enabled=true},&script));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=script},NULL)); fl_component_clear(&script);
    Fl_Id prototype=0,instance=0;
    CHECK(fl_prototype_capture(project,scene,entity,"Reusable motion",&prototype));
    CHECK(fl_prototype_instantiate(project,scene,0,prototype,100,0,&instance));
    const Fl_Component *attachment=fl_entity_component(fl_scene_entity(fl_project_scene(project,scene),instance),FL_COMPONENT_PROTOTYPE);
    Fl_PrototypeInstance configuration; CHECK(fl_prototype_instance_decode(attachment,&configuration));
    Fl_Id generator=attachment->id;
    configuration.time.mode=FL_RETIME_FIXED; configuration.time.rate=2; configuration.time.loop=true; configuration.time.duration=120000;
    Fl_Component replacement={0}; CHECK(fl_prototype_instance_encode(&configuration,&replacement)); replacement.id=generator;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=instance,.component=replacement},NULL)); fl_component_clear(&replacement);
    Fl_SceneState original={0}; double expected[5][2];
    for (unsigned k=0;k<5;++k) {
        CHECK(fl_scene_state_update(&original,fl_project_scene(project,scene),(Fl_Tick)k*24000));
        const Fl_Scene *flat=fl_scene_state_scene(&original,fl_project_scene(project,scene)); unsigned index=0;
        for (size_t i=0;i<flat->entity_count;++i) if (!strcmp(flat->entities[i].name,"Motion") && index<2) expected[k][index++]=original.entities[i].transform.x;
        CHECK(index==2);
    }
    fl_scene_state_clear(&original);
    Fl_Bake *bake=fl_bake_create(NULL); CHECK(bake);
    Fl_BakeOptions options={.start=0,.end=120000,.step=12000,.tolerance=0.001};
    CHECK(fl_bake_start(bake,project,scene,&options));
    CHECK(finished(bake)==FL_BAKE_READY); 
    Fl_Id owner=0; CHECK(fl_bake_apply(bake,project,&owner)); CHECK(owner);
    CHECK(fl_project_scene(project,scene)->entity_count==1);
    Fl_SceneState state={0}; CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),60000));
    const Fl_Scene *flat=fl_scene_state_scene(&state,fl_project_scene(project,scene)); bool found=false;
    for (size_t i=0;i<flat->entity_count;++i) if (!strcmp(flat->entities[i].name,"Motion")) {
        found=true; CHECK(fabs(state.entities[i].transform.x-10)<0.001 || fabs(state.entities[i].transform.x)<0.001); CHECK(!fl_entity_component(&flat->entities[i],FL_COMPONENT_SCRIPT));
    }
    CHECK(found);
    for (unsigned k=0;k<5;++k) {
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),(Fl_Tick)k*24000));
        flat=fl_scene_state_scene(&state,fl_project_scene(project,scene)); unsigned index=0;
        for (size_t i=0;i<flat->entity_count;++i) if (!strcmp(flat->entities[i].name,"Motion") && index<2) CHECK(fabs(state.entities[i].transform.x-expected[k][index++])<0.001);
        CHECK(index==2);
    }
    fl_scene_state_clear(&state); CHECK(fl_project_save(project));
    char label[FL_HISTORY_LABEL_MAX]; bool conflict=false;
    CHECK(fl_history_undo(fl_project_history(project),project,label,&conflict));
    CHECK(fl_scene_entity(fl_project_scene(project,scene),entity));
    CHECK(fl_bake_start(bake,project,scene,&options)); CHECK(finished(bake)==FL_BAKE_READY);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_RENAME,.scene=scene,.entity=entity,.name="Changed"},NULL));
    CHECK(!fl_bake_apply(bake,project,NULL));
    CHECK(fl_bake_start(bake,project,scene,&options)); fl_bake_cancel(bake);
    CHECK(finished(bake)==FL_BAKE_CANCELLED); CHECK(!fl_bake_apply(bake,project,NULL));
    fl_bake_destroy(bake); fl_project_destroy(project);
    project=fl_project_open(argv[1],NULL); CHECK(project);
    if (project) {
        CHECK(fl_project_scene(project,scene)->entity_count==1);
        CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=prototype},NULL));
        CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=asset},NULL));
        CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),60000)); fl_scene_state_clear(&state); fl_project_destroy(project);
    }
    return FLOW_TEST_RESULT("fl_bake_tests");
}
