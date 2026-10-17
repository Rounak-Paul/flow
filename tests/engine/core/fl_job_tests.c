#include "core/fl_atomic.h"
#include "core/fl_job.h"
#include "core/fl_thread.h"

#include "flow_test.h"

#include <stdlib.h>
#include <string.h>

static void increment(void *data)
{
    fl_atomic_fetch_add_u32((volatile uint32_t *)data, 1u);
}

static void test_many_jobs_run_once(void)
{
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 4 });
    CHECK(jobs != NULL);
    CHECK(fl_jobs_worker_count(jobs) == 4);

    enum { COUNT = 20000 };
    static volatile uint32_t counters[COUNT];
    static Fl_JobHandle handles[COUNT];
    for (int i = 0; i < COUNT; ++i) {
        handles[i] = fl_job_submit(jobs, &(Fl_JobDesc){
            .fn = increment, .data = (void *)&counters[i],
            .priority = (Fl_JobPriority)(i % FL_JOB_PRIORITY_COUNT) });
        CHECK(handles[i].generation != 0);
    }
    for (int i = 0; i < COUNT; ++i) fl_job_wait(jobs, handles[i]);
    bool all_once = true;
    for (int i = 0; i < COUNT; ++i) all_once = all_once && counters[i] == 1u;
    CHECK(all_once);
    CHECK(fl_job_is_done(jobs, handles[0]));
    CHECK(fl_jobs_destroy(jobs));
}

typedef struct Gate {
    Fl_Mutex   lock;
    Fl_CondVar cond;
    bool       open;
    bool       entered;
} Gate;

static void gate_block(void *data)
{
    Gate *gate = data;
    fl_mutex_lock(&gate->lock);
    gate->entered = true;
    fl_cond_broadcast(&gate->cond);
    while (!gate->open) fl_cond_wait(&gate->cond, &gate->lock);
    fl_mutex_unlock(&gate->lock);
}

typedef struct Order {
    Fl_Mutex lock;
    int      sequence[16];
    int      count;
} Order;

typedef struct OrderedJob {
    Order *order;
    int    id;
} OrderedJob;

static void record_order(void *data)
{
    OrderedJob *job = data;
    fl_mutex_lock(&job->order->lock);
    job->order->sequence[job->order->count++] = job->id;
    fl_mutex_unlock(&job->order->lock);
}

static void test_priority_order(void)
{
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 1 });
    Gate gate = {0};
    fl_mutex_init(&gate.lock);
    fl_cond_init(&gate.cond);
    Order order = {0};
    fl_mutex_init(&order.lock);

    Fl_JobHandle blocker = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = gate_block, .data = &gate });
    fl_mutex_lock(&gate.lock);
    while (!gate.entered) fl_cond_wait(&gate.cond, &gate.lock);
    fl_mutex_unlock(&gate.lock);

    OrderedJob low = { &order, 3 }, normal = { &order, 2 }, high = { &order, 1 }, high2 = { &order, 4 };
    Fl_JobHandle h[4];
    h[0] = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = record_order, .data = &low, .priority = FL_JOB_PRIORITY_LOW });
    h[1] = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = record_order, .data = &normal, .priority = FL_JOB_PRIORITY_NORMAL });
    h[2] = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = record_order, .data = &high, .priority = FL_JOB_PRIORITY_HIGH });
    h[3] = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = record_order, .data = &high2, .priority = FL_JOB_PRIORITY_HIGH });

    fl_mutex_lock(&gate.lock);
    gate.open = true;
    fl_cond_broadcast(&gate.cond);
    fl_mutex_unlock(&gate.lock);
    fl_job_wait(jobs, blocker);
    for (int i = 0; i < 4; ++i) fl_job_wait(jobs, h[i]);

    const int expected[] = { 1, 4, 2, 3 };
    CHECK(order.count == 4);
    for (int i = 0; i < 4; ++i) CHECK(order.sequence[i] == expected[i]);
    CHECK(fl_jobs_destroy(jobs));
}

static void test_dependencies(void)
{
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 4 });
    for (int round = 0; round < 200; ++round) {
        Order order = {0};
        fl_mutex_init(&order.lock);
        OrderedJob a = { &order, 1 }, b = { &order, 2 }, c = { &order, 3 }, d = { &order, 4 };
        Fl_JobHandle ha = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = record_order, .data = &a });
        Fl_JobHandle hb = fl_job_submit(jobs, &(Fl_JobDesc){
            .fn = record_order, .data = &b, .dependencies = &ha, .dependency_count = 1 });
        Fl_JobHandle hc = fl_job_submit(jobs, &(Fl_JobDesc){
            .fn = record_order, .data = &c, .dependencies = &ha, .dependency_count = 1 });
        const Fl_JobHandle join[] = { hb, hc, hb, (Fl_JobHandle){0} };
        Fl_JobHandle hd = fl_job_submit(jobs, &(Fl_JobDesc){
            .fn = record_order, .data = &d, .dependencies = join, .dependency_count = 4 });
        CHECK(hd.generation != 0);
        fl_job_wait(jobs, hd);
        CHECK(order.count == 4);
        CHECK(order.sequence[0] == 1);
        CHECK(order.sequence[3] == 4);
        fl_mutex_destroy(&order.lock);
    }

    volatile uint32_t ran = 0;
    Fl_JobHandle first = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = increment, .data = (void *)&ran });
    fl_job_wait(jobs, first);
    Fl_JobHandle after = fl_job_submit(jobs, &(Fl_JobDesc){
        .fn = increment, .data = (void *)&ran, .dependencies = &first, .dependency_count = 1 });
    fl_job_wait(jobs, after);
    CHECK(ran == 2u);
    CHECK(fl_jobs_destroy(jobs));
}

typedef struct Nested {
    Fl_JobSystem     *jobs;
    volatile uint32_t done;
    int               depth;
} Nested;

static void nested_job(void *data)
{
    Nested *nested = data;
    if (nested->depth > 0) {
        Nested child = { .jobs = nested->jobs, .depth = nested->depth - 1 };
        Fl_JobHandle handle = fl_job_submit(nested->jobs, &(Fl_JobDesc){ .fn = nested_job, .data = &child });
        fl_job_wait(nested->jobs, handle);
        fl_atomic_fetch_add_u32(&nested->done, child.done);
    }
    fl_atomic_fetch_add_u32(&nested->done, 1u);
}

static void test_wait_inside_job_single_worker(void)
{
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 1 });
    Nested root = { .jobs = jobs, .depth = 50 };
    Fl_JobHandle handle = fl_job_submit(jobs, &(Fl_JobDesc){ .fn = nested_job, .data = &root });
    fl_job_wait(jobs, handle);
    CHECK(root.done == 51u);
    CHECK(fl_jobs_destroy(jobs));
}

typedef struct SumData {
    const uint32_t   *values;
    volatile uint32_t *visits;
    volatile uint64_t total;
} SumData;

static void sum_range(void *data, size_t begin, size_t end)
{
    SumData *sum = data;
    uint64_t local = 0;
    for (size_t i = begin; i < end; ++i) {
        local += sum->values[i];
        fl_atomic_fetch_add_u32(&sum->visits[i], 1u);
    }
    fl_atomic_fetch_add_u64(&sum->total, local);
}

static void test_parallel_for(void)
{
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 3 });
    enum { N = 100003 };
    uint32_t *values = malloc(N * sizeof(uint32_t));
    volatile uint32_t *visits = calloc(N, sizeof(uint32_t));
    uint64_t expected = 0;
    for (uint32_t i = 0; i < N; ++i) {
        values[i] = i * 7u + 1u;
        expected += values[i];
    }
    SumData sum = { .values = values, .visits = visits };
    fl_job_parallel_for(jobs, N, 64, sum_range, &sum, FL_JOB_PRIORITY_NORMAL);
    CHECK(sum.total == expected);
    bool once = true;
    for (uint32_t i = 0; i < N; ++i) once = once && visits[i] == 1u;
    CHECK(once);

    SumData tiny = { .values = values, .visits = visits };
    memset((void *)visits, 0, N * sizeof(uint32_t));
    fl_job_parallel_for(jobs, 5, 1000, sum_range, &tiny, FL_JOB_PRIORITY_HIGH);
    CHECK(tiny.total == values[0] + values[1] + values[2] + values[3] + values[4]);
    fl_job_parallel_for(jobs, 0, 1, sum_range, &tiny, FL_JOB_PRIORITY_HIGH);
    fl_job_parallel_for(NULL, 3, 1, sum_range, &tiny, FL_JOB_PRIORITY_HIGH);

    free(values);
    free((void *)visits);
    CHECK(fl_jobs_destroy(jobs));
}

typedef struct DoneLog {
    int      count;
    uint64_t tag_sum;
} DoneLog;

static bool on_done(const Fl_Event *event, void *user_data)
{
    DoneLog *log = user_data;
    const Fl_JobDoneEvent *done = event->payload;
    log->count++;
    log->tag_sum += done->tag;
    return false;
}

static void noop(void *data)
{
    (void)data;
}

static void test_completion_events(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 2, .bus = bus });
    Fl_EventType done_type = fl_jobs_done_event(jobs);
    CHECK(done_type != FL_EVENT_TYPE_INVALID);
    DoneLog log = {0};
    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = done_type, .fn = on_done, .user_data = &log });

    uint64_t expected = 0;
    for (uint64_t i = 1; i <= 100; ++i) {
        CHECK(fl_job_submit(jobs, &(Fl_JobDesc){ .fn = noop, .notify = true, .tag = i }).generation != 0);
        expected += i;
    }
    CHECK(fl_job_submit(jobs, &(Fl_JobDesc){ .fn = noop }).generation != 0);
    while (log.count < 100) fl_event_bus_dispatch(bus);
    fl_event_bus_dispatch(bus);
    CHECK(log.count == 100);
    CHECK(log.tag_sum == expected);
    CHECK(fl_jobs_destroy(jobs));
    CHECK(fl_event_bus_destroy(bus));
}

typedef struct Spawner {
    Fl_JobSystem     *jobs;
    volatile uint32_t ran;
    int               remaining;
} Spawner;

static void spawn_chain(void *data)
{
    Spawner *spawner = data;
    fl_atomic_fetch_add_u32(&spawner->ran, 1u);
    if (spawner->remaining-- > 0) fl_job_submit(spawner->jobs, &(Fl_JobDesc){ .fn = spawn_chain, .data = spawner });
}

static void test_destroy_drains_spawned_jobs(void)
{
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 2 });
    Spawner spawner = { .jobs = jobs, .remaining = 500 };
    fl_job_submit(jobs, &(Fl_JobDesc){ .fn = spawn_chain, .data = &spawner });
    CHECK(fl_jobs_destroy(jobs));
    CHECK(spawner.ran == 501u);
}

typedef struct DestroyFromWorker {
    Fl_JobSystem *jobs;
    bool          refused;
    bool          on_worker;
} DestroyFromWorker;

static void destroy_from_worker(void *data)
{
    DestroyFromWorker *d = data;
    d->on_worker = fl_jobs_on_worker(d->jobs);
    d->refused = !fl_jobs_destroy(d->jobs);
}

typedef struct HookCount {
    volatile uint32_t started;
    volatile uint32_t exited;
} HookCount;

static void hook_start(uint32_t index, void *data)
{
    (void)index;
    fl_atomic_fetch_add_u32(&((HookCount *)data)->started, 1u);
}

static void hook_exit(uint32_t index, void *data)
{
    (void)index;
    fl_atomic_fetch_add_u32(&((HookCount *)data)->exited, 1u);
}

static void test_invalid_and_hooks(void)
{
    HookCount hooks = {0};
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){
        .worker_count = 3, .on_thread_start = hook_start, .on_thread_exit = hook_exit,
        .hook_data = &hooks });
    CHECK(fl_job_submit(jobs, &(Fl_JobDesc){0}).generation == 0);
    CHECK(fl_job_submit(jobs, &(Fl_JobDesc){ .fn = noop, .notify = true }).generation == 0);
    CHECK(fl_job_submit(jobs, &(Fl_JobDesc){ .fn = noop, .priority = (Fl_JobPriority)9 }).generation == 0);
    CHECK(fl_job_submit(jobs, &(Fl_JobDesc){ .fn = noop, .dependency_count = 2 }).generation == 0);
    CHECK(fl_job_submit(NULL, &(Fl_JobDesc){ .fn = noop }).generation == 0);
    CHECK(fl_job_is_done(jobs, (Fl_JobHandle){0}));
    fl_job_wait(jobs, (Fl_JobHandle){0});
    CHECK(!fl_jobs_on_worker(jobs));
    CHECK(fl_jobs_done_event(jobs) == FL_EVENT_TYPE_INVALID);

    DestroyFromWorker d = { .jobs = jobs };
    fl_job_wait(jobs, fl_job_submit(jobs, &(Fl_JobDesc){ .fn = destroy_from_worker, .data = &d }));
    CHECK(d.refused);

    CHECK(fl_jobs_destroy(jobs));
    CHECK(hooks.started == 3u && hooks.exited == 3u);
    CHECK(fl_jobs_destroy(NULL));
}

int main(void)
{
    test_many_jobs_run_once();
    test_priority_order();
    test_dependencies();
    test_wait_inside_job_single_worker();
    test_parallel_for();
    test_completion_events();
    test_destroy_drains_spawned_jobs();
    test_invalid_and_hooks();
    return FLOW_TEST_RESULT("fl_job_tests");
}
