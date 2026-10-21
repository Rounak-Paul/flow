#include "fl_job.h"

#include "fl_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOB_NONE          UINT32_MAX
#define JOB_MIN_CAP       64u
#define BATCHES_PER_WORKER 4u

/* Jobs currently executing on this thread, including helped jobs. */
static _Thread_local uint32_t t_job_depth;

typedef enum JobState {
    JOB_FREE = 0,
    JOB_WAITING,    /* has unfinished dependencies */
    JOB_QUEUED,
    JOB_RUNNING,
} JobState;

typedef struct Job {
    Fl_JobFn  fn;
    void     *data;
    uint64_t  tag;
    uint32_t *successors;      /* jobs waiting on this one; capacity reused */
    uint32_t  successor_count;
    uint32_t  successor_cap;
    uint32_t  remaining;       /* unfinished dependencies                  */
    uint32_t  generation;
    uint32_t  next;            /* queue or free-list link                  */
    uint8_t   priority;
    uint8_t   state;
    bool      notify;
} Job;

typedef struct JobQueue {
    uint32_t head;
    uint32_t tail;
} JobQueue;

typedef struct Worker {
    Fl_JobSystem *jobs;
    Fl_Thread    *thread;
    uint64_t      thread_id;
    uint32_t      index;
} Worker;

struct Fl_JobSystem {
    Fl_Mutex      lock;
    Fl_CondVar    work_ready;     /* runnable jobs or shutdown             */
    Fl_CondVar    job_finished;   /* any job completed                     */
    Job          *slots;
    uint32_t      slot_count;
    uint32_t      slot_cap;
    uint32_t      free_head;
    JobQueue      queues[FL_JOB_PRIORITY_COUNT];
    uint32_t      live;           /* submitted, not yet finished           */
    uint32_t      finish_waiters;
    bool          stopping;

    Worker       *workers;
    uint32_t      worker_count;

    Fl_EventBus  *bus;
    Fl_EventType  done_event;
    Fl_JobThreadHookFn on_thread_start;
    Fl_JobThreadHookFn on_thread_exit;
    void         *hook_data;
};

static bool handle_live_locked(const Fl_JobSystem *jobs, Fl_JobHandle job)
{
    if (job.generation == 0 || job.index >= jobs->slot_count) return false;
    const Job *slot = &jobs->slots[job.index];
    return slot->state != JOB_FREE && slot->generation == job.generation;
}

static void enqueue_locked(Fl_JobSystem *jobs, uint32_t index)
{
    Job *job = &jobs->slots[index];
    JobQueue *queue = &jobs->queues[job->priority];
    job->state = JOB_QUEUED;
    job->next = JOB_NONE;
    if (queue->tail == JOB_NONE) queue->head = index;
    else jobs->slots[queue->tail].next = index;
    queue->tail = index;
    fl_cond_signal(&jobs->work_ready);
}

static uint32_t dequeue_locked(Fl_JobSystem *jobs)
{
    for (uint32_t p = 0; p < FL_JOB_PRIORITY_COUNT; ++p) {
        JobQueue *queue = &jobs->queues[p];
        if (queue->head == JOB_NONE) continue;
        uint32_t index = queue->head;
        queue->head = jobs->slots[index].next;
        if (queue->head == JOB_NONE) queue->tail = JOB_NONE;
        return index;
    }
    return JOB_NONE;
}

static bool slot_alloc_locked(Fl_JobSystem *jobs, uint32_t *out_index)
{
    if (jobs->free_head != JOB_NONE) {
        *out_index = jobs->free_head;
        jobs->free_head = jobs->slots[*out_index].next;
        return true;
    }
    if (jobs->slot_count == jobs->slot_cap) {
        if (jobs->slot_cap >= JOB_NONE / 2u) return false;
        uint32_t cap = jobs->slot_cap ? jobs->slot_cap * 2u : JOB_MIN_CAP;
        if ((size_t)cap > SIZE_MAX / sizeof(Job)) return false;
        Job *slots = realloc(jobs->slots, (size_t)cap * sizeof(Job));
        if (!slots) return false;
        jobs->slots = slots;
        jobs->slot_cap = cap;
    }
    *out_index = jobs->slot_count++;
    jobs->slots[*out_index] = (Job){ .generation = 1u, .next = JOB_NONE };
    return true;
}

static void slot_release_locked(Fl_JobSystem *jobs, uint32_t index)
{
    Job *job = &jobs->slots[index];
    uint32_t generation = job->generation + 1u;
    job->generation = generation ? generation : 1u;
    job->state = JOB_FREE;
    job->fn = NULL;
    job->data = NULL;
    job->successor_count = 0;
    job->next = jobs->free_head;
    jobs->free_head = index;
}

static bool reserve_successor_locked(Job *job, uint32_t extra)
{
    if (job->successor_count > UINT32_MAX - extra) return false;
    uint32_t needed = job->successor_count + extra;
    if (needed <= job->successor_cap) return true;
    uint32_t cap = job->successor_cap ? job->successor_cap : 4u;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2u) {
            cap = needed;
            break;
        }
        cap *= 2u;
    }
    uint32_t *successors = realloc(job->successors, (size_t)cap * sizeof(uint32_t));
    if (!successors) return false;
    job->successors = successors;
    job->successor_cap = cap;
    return true;
}

/* Finish a job: release dependents, recycle the slot, wake waiters. */
static void complete_locked(Fl_JobSystem *jobs, uint32_t index)
{
    Job *job = &jobs->slots[index];
    const Fl_JobDoneEvent done = {
        .job = { .index = index, .generation = job->generation },
        .tag = job->tag,
    };
    const bool notify = job->notify;

    for (uint32_t i = 0; i < job->successor_count; ++i) {
        Job *successor = &jobs->slots[job->successors[i]];
        if (--successor->remaining == 0) enqueue_locked(jobs, job->successors[i]);
    }
    slot_release_locked(jobs, index);
    jobs->live--;

    if (jobs->finish_waiters) fl_cond_broadcast(&jobs->job_finished);
    if (jobs->stopping && jobs->live == 0) fl_cond_broadcast(&jobs->work_ready);
    if (notify) fl_event_post(jobs->bus, jobs->done_event, &done, sizeof(done));
}

/* Run a dequeued job with the lock released around its body. */
static void run_locked(Fl_JobSystem *jobs, uint32_t index)
{
    Job *job = &jobs->slots[index];
    job->state = JOB_RUNNING;
    Fl_JobFn fn = job->fn;
    void *data = job->data;
    fl_mutex_unlock(&jobs->lock);
    t_job_depth++;
    fn(data);
    t_job_depth--;
    fl_mutex_lock(&jobs->lock);
    complete_locked(jobs, index);
}

static void worker_main(void *user_data)
{
    Worker *worker = user_data;
    Fl_JobSystem *jobs = worker->jobs;

    char name[FL_THREAD_NAME_MAX + 1u];
    snprintf(name, sizeof(name), "flow-job-%u", worker->index);
    fl_thread_set_name(name);
    if (jobs->on_thread_start) jobs->on_thread_start(worker->index, jobs->hook_data);

    fl_mutex_lock(&jobs->lock);
    worker->thread_id = fl_thread_current_id();
    for (;;) {
        uint32_t index = dequeue_locked(jobs);
        if (index != JOB_NONE) {
            run_locked(jobs, index);
            continue;
        }
        if (jobs->stopping && jobs->live == 0) break;
        fl_cond_wait(&jobs->work_ready, &jobs->lock);
    }
    fl_mutex_unlock(&jobs->lock);

    if (jobs->on_thread_exit) jobs->on_thread_exit(worker->index, jobs->hook_data);
}

static void free_system(Fl_JobSystem *jobs)
{
    for (uint32_t i = 0; i < jobs->slot_count; ++i) free(jobs->slots[i].successors);
    free(jobs->slots);
    free(jobs->workers);
    fl_cond_destroy(&jobs->job_finished);
    fl_cond_destroy(&jobs->work_ready);
    fl_mutex_destroy(&jobs->lock);
    free(jobs);
}

/* Stop and join the first `started` workers; used by create failure and destroy. */
static void stop_workers(Fl_JobSystem *jobs, uint32_t started)
{
    fl_mutex_lock(&jobs->lock);
    jobs->stopping = true;
    fl_cond_broadcast(&jobs->work_ready);
    fl_mutex_unlock(&jobs->lock);
    for (uint32_t i = 0; i < started; ++i) fl_thread_join(jobs->workers[i].thread);
}

Fl_JobSystem *fl_jobs_create(const Fl_JobSystemDesc *desc)
{
    const Fl_JobSystemDesc defaults = {0};
    if (!desc) desc = &defaults;

    Fl_JobSystem *jobs = calloc(1, sizeof(*jobs));
    if (!jobs) return NULL;
    const bool lock_ok = fl_mutex_init(&jobs->lock);
    const bool ready_ok = lock_ok && fl_cond_init(&jobs->work_ready);
    const bool finished_ok = ready_ok && fl_cond_init(&jobs->job_finished);
    if (!finished_ok) {
        if (ready_ok) fl_cond_destroy(&jobs->work_ready);
        if (lock_ok) fl_mutex_destroy(&jobs->lock);
        free(jobs);
        return NULL;
    }

    jobs->free_head = JOB_NONE;
    for (uint32_t p = 0; p < FL_JOB_PRIORITY_COUNT; ++p)
        jobs->queues[p] = (JobQueue){ .head = JOB_NONE, .tail = JOB_NONE };
    jobs->on_thread_start = desc->on_thread_start;
    jobs->on_thread_exit = desc->on_thread_exit;
    jobs->hook_data = desc->hook_data;

    if (desc->bus) {
        jobs->bus = desc->bus;
        jobs->done_event = fl_event_type_register(desc->bus, FL_JOB_DONE_EVENT, sizeof(Fl_JobDoneEvent));
        if (jobs->done_event == FL_EVENT_TYPE_INVALID) {
            free_system(jobs);
            return NULL;
        }
    }

    uint32_t count = desc->worker_count;
    if (!count) {
        uint32_t cpus = fl_thread_hardware_concurrency();
        count = cpus > 1u ? cpus - 1u : 1u;
    }
    jobs->workers = calloc(count, sizeof(Worker));
    if (!jobs->workers) {
        free_system(jobs);
        return NULL;
    }
    jobs->worker_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        jobs->workers[i] = (Worker){ .jobs = jobs, .index = i };
        jobs->workers[i].thread = fl_thread_create(worker_main, &jobs->workers[i]);
        if (!jobs->workers[i].thread) {
            stop_workers(jobs, i);
            free_system(jobs);
            return NULL;
        }
    }
    return jobs;
}

bool fl_jobs_on_worker(Fl_JobSystem *jobs)
{
    if (!jobs) return false;
    const uint64_t self = fl_thread_current_id();
    fl_mutex_lock(&jobs->lock);
    bool found = false;
    for (uint32_t i = 0; i < jobs->worker_count && !found; ++i)
        found = jobs->workers[i].thread_id == self;
    fl_mutex_unlock(&jobs->lock);
    return found;
}

bool fl_jobs_destroy(Fl_JobSystem *jobs)
{
    if (!jobs) return true;
    if (t_job_depth > 0 || fl_jobs_on_worker(jobs)) return false;
    stop_workers(jobs, jobs->worker_count);
    free_system(jobs);
    return true;
}

uint32_t fl_jobs_worker_count(const Fl_JobSystem *jobs)
{
    return jobs ? jobs->worker_count : 0u;
}

Fl_EventType fl_jobs_done_event(const Fl_JobSystem *jobs)
{
    return jobs ? jobs->done_event : FL_EVENT_TYPE_INVALID;
}

Fl_JobHandle fl_job_submit(Fl_JobSystem *jobs, const Fl_JobDesc *desc)
{
    const Fl_JobHandle none = {0};
    if (!jobs || !desc || !desc->fn) return none;
    if ((unsigned)desc->priority >= (unsigned)FL_JOB_PRIORITY_COUNT) return none;
    if (desc->notify && !jobs->bus) return none;
    if (desc->dependency_count && !desc->dependencies) return none;

    fl_mutex_lock(&jobs->lock);
    for (uint32_t i = 0; i < desc->dependency_count; ++i) {
        const Fl_JobHandle dep = desc->dependencies[i];
        if (handle_live_locked(jobs, dep) &&
            !reserve_successor_locked(&jobs->slots[dep.index], 1u)) {
            fl_mutex_unlock(&jobs->lock);
            return none;
        }
    }

    uint32_t index;
    if (!slot_alloc_locked(jobs, &index)) {
        fl_mutex_unlock(&jobs->lock);
        return none;
    }
    Job *job = &jobs->slots[index];
    job->fn = desc->fn;
    job->data = desc->data;
    job->tag = desc->tag;
    job->notify = desc->notify;
    job->priority = (uint8_t)desc->priority;
    job->remaining = 0;

    /* Each distinct live dependency gains exactly one successor entry, for
       which capacity was reserved above, so linking cannot fail. A repeated
       dependency is already the parent's most recent successor: this job's
       links are appended under the same lock hold. */
    for (uint32_t i = 0; i < desc->dependency_count; ++i) {
        const Fl_JobHandle dep = desc->dependencies[i];
        if (!handle_live_locked(jobs, dep)) continue;
        Job *parent = &jobs->slots[dep.index];
        if (parent->successor_count && parent->successors[parent->successor_count - 1u] == index)
            continue;
        parent->successors[parent->successor_count++] = index;
        job->remaining++;
    }

    const Fl_JobHandle handle = { .index = index, .generation = job->generation };
    jobs->live++;
    if (job->remaining == 0) enqueue_locked(jobs, index);
    else job->state = JOB_WAITING;
    fl_mutex_unlock(&jobs->lock);
    return handle;
}

bool fl_job_is_done(Fl_JobSystem *jobs, Fl_JobHandle job)
{
    if (!jobs) return true;
    fl_mutex_lock(&jobs->lock);
    const bool done = !handle_live_locked(jobs, job);
    fl_mutex_unlock(&jobs->lock);
    return done;
}

void fl_job_wait(Fl_JobSystem *jobs, Fl_JobHandle job)
{
    if (!jobs || job.generation == 0) return;
    fl_mutex_lock(&jobs->lock);
    while (handle_live_locked(jobs, job)) {
        uint32_t index = dequeue_locked(jobs);
        if (index != JOB_NONE) {
            run_locked(jobs, index);
            continue;
        }
        jobs->finish_waiters++;
        fl_cond_wait(&jobs->job_finished, &jobs->lock);
        jobs->finish_waiters--;
    }
    fl_mutex_unlock(&jobs->lock);
}

typedef struct RangeJob {
    Fl_JobRangeFn fn;
    void         *data;
    size_t        begin;
    size_t        end;
} RangeJob;

static void run_range(void *user_data)
{
    const RangeJob *range = user_data;
    range->fn(range->data, range->begin, range->end);
}

void fl_job_parallel_for(Fl_JobSystem *jobs, size_t count, size_t min_batch,
                         Fl_JobRangeFn fn, void *data, Fl_JobPriority priority)
{
    if (!fn || count == 0) return;
    if (!jobs) {
        fn(data, 0, count);
        return;
    }

    size_t batch = min_batch ? min_batch : 1u;
    const size_t target_batches = (size_t)jobs->worker_count * BATCHES_PER_WORKER;
    const size_t even = count / target_batches + (count % target_batches ? 1u : 0u);
    if (even > batch) batch = even;
    const size_t batches = count / batch + (count % batch ? 1u : 0u);
    if (batches <= 1u) {
        fn(data, 0, count);
        return;
    }

    RangeJob *ranges = calloc(batches, sizeof(RangeJob));
    Fl_JobHandle *handles = calloc(batches, sizeof(Fl_JobHandle));
    if (!ranges || !handles) {
        free(ranges);
        free(handles);
        fn(data, 0, count);
        return;
    }

    for (size_t i = 0; i < batches; ++i) {
        const size_t begin = i * batch;
        ranges[i] = (RangeJob){
            .fn = fn, .data = data, .begin = begin,
            .end = begin + batch < count ? begin + batch : count,
        };
        /* The caller's share runs inline below, so it is never queued. */
        if (i + 1u == batches) break;
        handles[i] = fl_job_submit(jobs, &(Fl_JobDesc){
            .fn = run_range, .data = &ranges[i], .priority = priority,
        });
        if (handles[i].generation == 0) run_range(&ranges[i]);
    }
    run_range(&ranges[batches - 1u]);
    for (size_t i = 0; i + 1u < batches; ++i) fl_job_wait(jobs, handles[i]);

    free(ranges);
    free(handles);
}

Fl_EventBus *fl_jobs_event_bus(const Fl_JobSystem *jobs) {return jobs ? jobs->bus : NULL;}
