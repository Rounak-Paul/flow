#pragma once
#include "fl_scene_state.h"
#include "core/fl_job.h"

/* Owner-thread session with immutable document inputs and one cancellable worker at a time. */
typedef struct Fl_Evaluation Fl_Evaluation;
typedef enum Fl_EvaluationStatus { FL_EVALUATION_READY, FL_EVALUATION_PENDING, FL_EVALUATION_FAILED } Fl_EvaluationStatus;

/* Capture project and scene for repeated samples. jobs must publish completion events on the caller's bus.
   The source project can change or close afterwards; imported resource bytes are not part of this document view. */
Fl_Evaluation *fl_evaluation_create(Fl_JobSystem *jobs, const Fl_Project *project, Fl_Id scene);
/* Cancel and join any work before releasing the document and evaluated storage. NULL is safe. */
void fl_evaluation_destroy(Fl_Evaluation *evaluation);
/* Request tick without blocking. While busy, finish the existing request before accepting another. */
Fl_EvaluationStatus fl_evaluation_request(Fl_Evaluation *evaluation, Fl_Tick tick);
/* Adopt completed work at an owner-thread publication boundary; false means no complete result was adopted. */
bool fl_evaluation_publish(Fl_Evaluation *evaluation);
/* Whether a generic job completion belongs to this session's active work. */
bool fl_evaluation_matches(const Fl_Evaluation *evaluation, const Fl_JobDoneEvent *event);
/* Request cooperative cancellation; published results stay valid. */
void fl_evaluation_cancel(Fl_Evaluation *evaluation);
/* Complete an exact sample for an explicit blocking consumer; returns false on allocation/evaluation failure. */
bool fl_evaluation_wait(Fl_Evaluation *evaluation, Fl_Tick tick);
/* Borrow the last complete state; remains immutable until publish/wait/destroy on the owner thread. */
const Fl_SceneState *fl_evaluation_state(const Fl_Evaluation *evaluation);
/* Immutable scene used by this session, valid until destroy. */
const Fl_Scene *fl_evaluation_scene(const Fl_Evaluation *evaluation);
/* Whether a request still awaits owner-thread adoption. */
bool fl_evaluation_pending(const Fl_Evaluation *evaluation);
