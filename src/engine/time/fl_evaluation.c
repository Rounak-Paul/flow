#include "fl_evaluation.h"
#include <stdlib.h>

struct Fl_Evaluation {
    Fl_JobSystem *jobs;
    Fl_JobHandle job;
    Fl_Project *view;
    const Fl_Scene *scene;
    Fl_SceneState front, back;
    Fl_Tick requested;
    atomic_bool cancel;
    bool completed, failed;
};

/* Only the worker accesses back until the job completion fence. */
static void evaluate(void *data)
{
    Fl_Evaluation *evaluation=data;
    evaluation->completed=fl_scene_state_update_cancel(&evaluation->back,evaluation->scene,
                                                      evaluation->requested,&evaluation->cancel);
}

Fl_Evaluation *fl_evaluation_create(Fl_JobSystem *jobs, const Fl_Project *project, Fl_Id scene)
{
    if (!jobs || !fl_jobs_done_event(jobs)) return NULL;
    Fl_Evaluation *evaluation=calloc(1,sizeof(*evaluation));
    if (!evaluation) return NULL;
    atomic_init(&evaluation->cancel,false);
    evaluation->jobs=jobs; evaluation->view=fl_project_read_view(project);
    evaluation->scene=fl_project_scene(evaluation->view,scene);
    if (!evaluation->scene) {fl_evaluation_destroy(evaluation); return NULL;}
    return evaluation;
}

void fl_evaluation_cancel(Fl_Evaluation *evaluation)
{
    if (evaluation) atomic_store_explicit(&evaluation->cancel,true,memory_order_relaxed);
}

void fl_evaluation_destroy(Fl_Evaluation *evaluation)
{
    if (!evaluation) return;
    fl_evaluation_cancel(evaluation); fl_job_wait(evaluation->jobs,evaluation->job);
    fl_scene_state_clear(&evaluation->front); fl_scene_state_clear(&evaluation->back);
    fl_project_destroy(evaluation->view); free(evaluation);
}

Fl_EvaluationStatus fl_evaluation_request(Fl_Evaluation *evaluation, Fl_Tick tick)
{
    if (!evaluation || tick<0 || tick>fl_scene_length(evaluation->scene)) return FL_EVALUATION_FAILED;
    if (evaluation->job.generation) return FL_EVALUATION_PENDING;
    if (evaluation->front.valid && evaluation->front.tick==tick) return FL_EVALUATION_READY;
    if (evaluation->failed && evaluation->requested==tick) return FL_EVALUATION_FAILED;
    evaluation->completed=false; evaluation->failed=false; evaluation->requested=tick;
    atomic_store_explicit(&evaluation->cancel,false,memory_order_relaxed);
    evaluation->job=fl_job_submit(evaluation->jobs,&(Fl_JobDesc){.fn=evaluate,.data=evaluation,.notify=true,.priority=FL_JOB_PRIORITY_HIGH});
    if (!evaluation->job.generation) {evaluation->failed=true; return FL_EVALUATION_FAILED;}
    return FL_EVALUATION_PENDING;
}

bool fl_evaluation_publish(Fl_Evaluation *evaluation)
{
    if (!evaluation || !evaluation->job.generation || !fl_job_is_done(evaluation->jobs,evaluation->job)) return false;
    evaluation->job=(Fl_JobHandle){0};
    if (atomic_load_explicit(&evaluation->cancel,memory_order_relaxed)) return false;
    evaluation->failed=!evaluation->completed;
    if (!evaluation->completed) return false;
    Fl_SceneState previous=evaluation->front; evaluation->front=evaluation->back; evaluation->back=previous;
    return true;
}

bool fl_evaluation_matches(const Fl_Evaluation *evaluation, const Fl_JobDoneEvent *event)
{
    return evaluation && event && evaluation->job.generation && evaluation->job.index==event->job.index &&
        evaluation->job.generation==event->job.generation;
}

bool fl_evaluation_wait(Fl_Evaluation *evaluation, Fl_Tick tick)
{
    if (!evaluation) return false;
    if (evaluation->job.generation) {
        if (evaluation->requested!=tick) fl_evaluation_cancel(evaluation);
        fl_job_wait(evaluation->jobs,evaluation->job); fl_evaluation_publish(evaluation);
    }
    Fl_EvaluationStatus status=fl_evaluation_request(evaluation,tick);
    if (status==FL_EVALUATION_PENDING) {
        fl_job_wait(evaluation->jobs,evaluation->job); fl_evaluation_publish(evaluation);
    }
    return evaluation->front.valid && evaluation->front.tick==tick;
}

const Fl_SceneState *fl_evaluation_state(const Fl_Evaluation *evaluation)
{
    return evaluation && evaluation->front.valid ? &evaluation->front : NULL;
}

const Fl_Scene *fl_evaluation_scene(const Fl_Evaluation *evaluation) {return evaluation ? evaluation->scene : NULL;}
bool fl_evaluation_pending(const Fl_Evaluation *evaluation) {return evaluation && evaluation->job.generation;}
