#pragma once
#include "fl_project.h"
#include "core/fl_job.h"

typedef struct Fl_ResourceSnapshot Fl_ResourceSnapshot;
typedef enum Fl_ResourceStatus {FL_RESOURCES_PENDING,FL_RESOURCES_READY,FL_RESOURCES_FAILED,FL_RESOURCES_CANCELLED} Fl_ResourceStatus;
/* Capture an immutable document and copy referenced media bytes on a worker into an exclusively owned root.
   Source hashes and lengths are verified before READY. Project/jobs are borrowed only during creation/jobs respectively. */
Fl_ResourceSnapshot *fl_resource_snapshot_create(const Fl_Project *project, Fl_JobSystem *jobs,
                                                const Fl_Id *scenes, size_t count, char *error, size_t capacity);
/* Cancel, fence worker reads and release the view and owned files. Call on the creation thread. */
void fl_resource_snapshot_destroy(Fl_ResourceSnapshot *snapshot);
/* Poll after the worker fence; error is readable only after a terminal result. */
Fl_ResourceStatus fl_resource_snapshot_status(const Fl_ResourceSnapshot *snapshot);
const char *fl_resource_snapshot_error(const Fl_ResourceSnapshot *snapshot);
/* Borrow the captured document. Do not read resource files before READY. */
Fl_Project *fl_resource_snapshot_project(const Fl_ResourceSnapshot *snapshot);
/* Match a worker completion event without storing raw snapshot pointers in queued events. */
bool fl_resource_snapshot_matches(const Fl_ResourceSnapshot *snapshot, const Fl_JobDoneEvent *event);
/* Whether an active capture pins original store bytes against engine garbage collection. */
bool fl_resource_snapshot_leased(const char *directory, const unsigned char hash[32]);
