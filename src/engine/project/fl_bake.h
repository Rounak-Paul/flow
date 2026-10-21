#pragma once
#include "fl_project.h"
#include "core/fl_job.h"

/* Explicit capture interval in scene ticks; maximum step bounds sampling density, tolerance bounds
   numeric reduction error at captured ticks. Outside the interval the baked endpoints hold. */
typedef struct Fl_BakeOptions { Fl_Tick start, end, step; double tolerance; } Fl_BakeOptions;
typedef enum Fl_BakeStatus { FL_BAKE_IDLE, FL_BAKE_RUNNING, FL_BAKE_READY, FL_BAKE_FAILED, FL_BAKE_CANCELLED, FL_BAKE_APPLIED } Fl_BakeStatus;
typedef struct Fl_Bake Fl_Bake;
/* Create a native bake owner using an existing job pool, or a private single worker when jobs is NULL. */
Fl_Bake *fl_bake_create(Fl_JobSystem *jobs);
/* Cancel and fence outstanding work before releasing its immutable document view. Owner thread only. */
void fl_bake_destroy(Fl_Bake *bake);
/* Capture immutable input and schedule sampling; requires no active project changeset. */
bool fl_bake_start(Fl_Bake *bake, Fl_Project *project, Fl_Id scene, const Fl_BakeOptions *options);
/* Request cancellation without waiting or modifying the document. */
void fl_bake_cancel(Fl_Bake *bake);
/* Poll completion; diagnostics are borrowed until the next start/destroy. */
Fl_BakeStatus fl_bake_status(const Fl_Bake *bake);
const char *fl_bake_error(const Fl_Bake *bake);
/* Apply ready native data as one atomic undoable edit, rejecting any intervening project mutation. */
bool fl_bake_apply(Fl_Bake *bake, Fl_Project *project, Fl_Id *owner);

/* Match ID-only completion notifications from the borrowed pool's event bus. */
bool fl_bake_matches(const Fl_Bake *bake, const Fl_JobDoneEvent *event);
