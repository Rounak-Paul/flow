#pragma once

#include "fl_event.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Multithreaded job system.
 *
 * A fixed pool of worker threads executes jobs from three priority queues
 * (FIFO within a priority). A job may depend on other jobs; it becomes
 * runnable once every dependency has finished. Waiting is cooperative: a
 * thread blocked in fl_job_wait executes other runnable jobs meanwhile, so
 * waiting from inside a job never deadlocks the pool.
 *
 * Completion is observable without polling: jobs submitted with notify set
 * post a FL_JOB_DONE_EVENT on the system's event bus from the worker that
 * finished them, delivered on the bus owner thread at its next dispatch.
 *
 * Every function is thread-safe, including calls from inside jobs.
 */

typedef struct Fl_JobSystem Fl_JobSystem;

/* Job handle. Zero-initialised handles are invalid and count as finished. */
typedef struct Fl_JobHandle {
    uint32_t index;
    uint32_t generation;
} Fl_JobHandle;

/*
 * Job body.
 *
 * data  Pointer supplied in Fl_JobDesc; must stay valid until the job finishes.
 */
typedef void (*Fl_JobFn)(void *data);

/*
 * Range body for fl_job_parallel_for, covering indices [begin, end).
 */
typedef void (*Fl_JobRangeFn)(void *data, size_t begin, size_t end);

/*
 * Worker lifecycle hook, run on each worker thread right after it starts and
 * right before it exits (e.g. crash-handler attachment, profiler naming).
 *
 * worker_index  0-based worker number.
 * user_data     Pointer supplied in Fl_JobSystemDesc.
 */
typedef void (*Fl_JobThreadHookFn)(uint32_t worker_index, void *user_data);

typedef enum Fl_JobPriority {
    FL_JOB_PRIORITY_HIGH = 0,
    FL_JOB_PRIORITY_NORMAL,
    FL_JOB_PRIORITY_LOW,
    FL_JOB_PRIORITY_COUNT
} Fl_JobPriority;

/* Event type name registered on the bus; payload is Fl_JobDoneEvent. */
#define FL_JOB_DONE_EVENT "job.done"

typedef struct Fl_JobDoneEvent {
    Fl_JobHandle job;
    uint64_t     tag;
} Fl_JobDoneEvent;

typedef struct Fl_JobSystemDesc {
    uint32_t           worker_count;   /* 0 = logical CPUs - 1, at least 1       */
    Fl_EventBus       *bus;            /* optional; required for notify          */
    Fl_JobThreadHookFn on_thread_start;
    Fl_JobThreadHookFn on_thread_exit;
    void              *hook_data;
} Fl_JobSystemDesc;

typedef struct Fl_JobDesc {
    Fl_JobFn            fn;               /* required                            */
    void               *data;
    Fl_JobPriority      priority;         /* default FL_JOB_PRIORITY_HIGH (0)    */
    const Fl_JobHandle *dependencies;     /* finished/invalid handles are ignored */
    uint32_t            dependency_count;
    bool                notify;           /* post FL_JOB_DONE_EVENT when done     */
    uint64_t            tag;              /* caller value echoed in the event     */
} Fl_JobDesc;

/*
 * Create a job system and start its workers. When desc->bus is set this must
 * be called on the bus owner thread (it registers FL_JOB_DONE_EVENT).
 *
 * Returns  The system, or NULL on invalid arguments or OS/allocation failure.
 */
Fl_JobSystem *fl_jobs_create(const Fl_JobSystemDesc *desc);

/*
 * Wait for every submitted job (including jobs they submit), stop the
 * workers and free the system. NULL succeeds.
 *
 * Returns  false, leaving the system running, when called from a worker
 *          thread or from inside any job (including one a waiting thread is
 *          executing on the pool's behalf).
 */
bool fl_jobs_destroy(Fl_JobSystem *jobs);

/* Number of worker threads. */
uint32_t fl_jobs_worker_count(const Fl_JobSystem *jobs);

/* Report whether the calling thread is one of this system's workers. */
bool fl_jobs_on_worker(Fl_JobSystem *jobs);

/*
 * Completion event type on the system's bus.
 *
 * Returns  FL_EVENT_TYPE_INVALID when the system has no bus.
 */
Fl_EventType fl_jobs_done_event(const Fl_JobSystem *jobs);

/*
 * Submit a job.
 *
 * Returns  Handle, or a zero handle on invalid arguments (missing fn, bad
 *          priority, notify without a bus) or allocation failure. A zero
 *          handle means the job was not scheduled.
 */
Fl_JobHandle fl_job_submit(Fl_JobSystem *jobs, const Fl_JobDesc *desc);

/* Report whether a job has finished (zero/stale handles count as finished). */
bool fl_job_is_done(Fl_JobSystem *jobs, Fl_JobHandle job);

/*
 * Block until a job finishes, executing other runnable jobs meanwhile.
 */
void fl_job_wait(Fl_JobSystem *jobs, Fl_JobHandle job);

/*
 * Run fn over [0, count) in parallel batches of at least min_batch indices,
 * returning once every index has been processed. Falls back to running
 * inline when scheduling resources are unavailable.
 *
 * min_batch  Smallest batch size; 0 is treated as 1.
 */
void fl_job_parallel_for(Fl_JobSystem *jobs, size_t count, size_t min_batch,
                         Fl_JobRangeFn fn, void *data, Fl_JobPriority priority);

/* Borrow the event bus receiving this system's job completion notifications, or NULL. */
Fl_EventBus *fl_jobs_event_bus(const Fl_JobSystem *jobs);
