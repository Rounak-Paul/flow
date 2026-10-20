#include "flow_test.h"
#include "core/fl_job.h"
#include "project/fl_project.h"
#include "components/fl_script.h"
#include "time/fl_scene_state.h"
#include "time/fl_evaluation.h"
#include <stdlib.h>
#include <string.h>

/* Isolated state is worker-owned until the job completes; the read view remains owner-owned. */
typedef struct Evaluation {
    const Fl_Scene *scene;
    Fl_SceneState state;
    atomic_bool cancel;
    bool completed;
} Evaluation;

/* Sample every requested point from immutable source bytes, without accessing the live project. */
static void evaluate(void *data)
{
    Evaluation *evaluation=data;
    evaluation->completed=fl_scene_state_update_cancel(&evaluation->state,evaluation->scene,
                                                      FL_TICKS_PER_SECOND,&evaluation->cancel);
}

/* Verify source/document lifetime separation, mutation rejection and cooperative cancellation. */
int main(int argc, char **argv)
{
    if (argc!=2) return 2;
    Fl_Project *project=fl_project_create(argv[1],"Read view",NULL);
    CHECK(project); if (!project) return 1;
    Fl_Id scene=fl_project_scene_at(project,0)->id, entity=0, asset=0;
    const char *source="return {sample=function(ctx) ctx.entity.transform.x=42; ctx.entity.text=string.rep('text',1000) end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Text"},&entity));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,
        .component={.type=FL_COMPONENT_TEXT,.version=1,.data=(const unsigned char *)"Original",.size=8}},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="Behavior",.source=source,.source_size=strlen(source)},&asset));
    Fl_Component script={0}; CHECK(fl_script_encode(&(Fl_Script){.script=asset,.enabled=true},&script));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=script},NULL));
    fl_component_clear(&script);
    CHECK(fl_project_edit_begin(project,"pending",false));
    CHECK(!fl_project_read_view(project)); CHECK(fl_project_edit_cancel(project));
    Fl_Project *view=fl_project_read_view(project);
    CHECK(view && fl_project_is_read_view(view) && !fl_project_is_read_view(project));
    CHECK(!fl_project_apply(view,&(Fl_Op){.kind=FL_OP_SCENE_REMOVE,.scene=scene},NULL));
    CHECK(!fl_project_edit_begin(view,"forbidden",false) && !fl_project_save(view) && fl_project_reload(view)==FL_RELOAD_INVALID);
    const char *changed="return {sample=function(ctx) ctx.entity.transform.x=99 end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.asset =asset,.source=changed,.source_size=strlen(changed)},NULL));
    fl_project_destroy(project);
    if (view) {
        const Fl_Scene *frozen=fl_project_scene(view,scene);
        CHECK(frozen && frozen->project==view && !strcmp(fl_project_asset(view,asset)->source,source));
        Fl_EventBus *bus=fl_event_bus_create(NULL); CHECK(bus);
        Fl_JobSystem *jobs=fl_jobs_create(&(Fl_JobSystemDesc){.worker_count=1,.bus=bus}); CHECK(jobs);
        Evaluation evaluation={.scene=frozen}; atomic_init(&evaluation.cancel,false);
        Fl_JobHandle job=fl_job_submit(jobs,&(Fl_JobDesc){.fn=evaluate,.data=&evaluation});
        CHECK(job.generation); fl_job_wait(jobs,job);
        CHECK(evaluation.completed && evaluation.state.entities[0].transform.x==42 &&
              evaluation.state.entities[0].text_size==4000);
        atomic_store(&evaluation.cancel,true);
        job=fl_job_submit(jobs,&(Fl_JobDesc){.fn=evaluate,.data=&evaluation});
        CHECK(job.generation); fl_job_wait(jobs,job);
        CHECK(!evaluation.completed && !evaluation.state.valid);
        atomic_store(&evaluation.cancel,false);
        job=fl_job_submit(jobs,&(Fl_JobDesc){.fn=evaluate,.data=&evaluation});
        CHECK(job.generation); fl_job_wait(jobs,job);
        CHECK(evaluation.completed && evaluation.state.entities[0].transform.x==42);
        Fl_Evaluation *session=fl_evaluation_create(jobs,view,scene); CHECK(session);
        CHECK(fl_evaluation_request(session,FL_TICKS_PER_SECOND)==FL_EVALUATION_PENDING);
        CHECK(!fl_evaluation_state(session));
        CHECK(fl_evaluation_wait(session,FL_TICKS_PER_SECOND));
        const Fl_SceneState *published=fl_evaluation_state(session);
        CHECK(published && published->entities[0].transform.x==42 && published->entities[0].text_size==4000);
        const char *published_text=published ? published->entities[0].text : NULL;
        CHECK(fl_evaluation_request(session,2*FL_TICKS_PER_SECOND)==FL_EVALUATION_PENDING);
        CHECK(fl_evaluation_state(session)==published && published_text && !strcmp(published_text,published->entities[0].text));
        CHECK(fl_evaluation_wait(session,2*FL_TICKS_PER_SECOND));
        CHECK(fl_evaluation_state(session)->tick==2*FL_TICKS_PER_SECOND);
        CHECK(fl_evaluation_request(session,3*FL_TICKS_PER_SECOND)==FL_EVALUATION_PENDING);
        fl_evaluation_cancel(session);
        CHECK(fl_evaluation_wait(session,4*FL_TICKS_PER_SECOND));
        CHECK(fl_evaluation_state(session)->tick==4*FL_TICKS_PER_SECOND);
        CHECK(fl_evaluation_request(session,0)==FL_EVALUATION_PENDING);
        fl_evaluation_destroy(session);
        fl_event_bus_dispatch(bus);
        CHECK(fl_jobs_destroy(jobs)); CHECK(fl_event_bus_destroy(bus));
        fl_scene_state_clear(&evaluation.state); fl_project_destroy(view);
    }
    return FLOW_TEST_RESULT("fl_read_view_tests");
}
