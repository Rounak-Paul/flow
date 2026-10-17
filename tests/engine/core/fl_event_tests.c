#include "core/fl_event.h"
#include "core/fl_thread.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

typedef struct Value {
    int v;
} Value;

/* Records the order in which observers ran. */
typedef struct Trace {
    int calls[64];
    int count;
} Trace;

typedef struct Probe {
    Trace       *trace;
    int          id;
    bool         consume;
    Fl_EventBus *bus;
    Fl_EventType type;
    Fl_Subscription self;
    Fl_Subscription other;
    int          last_value;
} Probe;

static void trace_push(Trace *trace, int id)
{
    if (trace->count < (int)(sizeof(trace->calls) / sizeof(trace->calls[0])))
        trace->calls[trace->count++] = id;
}

static bool record(const Fl_Event *event, void *user_data)
{
    Probe *probe = user_data;
    trace_push(probe->trace, probe->id);
    if (event->payload_size == sizeof(Value))
        probe->last_value = ((const Value *)event->payload)->v;
    return probe->consume;
}

static Fl_Subscription sub(Fl_EventBus *bus, Fl_EventType type, Probe *probe, int32_t priority)
{
    return fl_event_subscribe(bus, &(Fl_SubscribeDesc){
        .type = type, .fn = record, .user_data = probe, .priority = priority,
    });
}

static void test_type_registry(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    CHECK(bus != NULL);

    Fl_EventType a = fl_event_type_register(bus, "test.a", sizeof(Value));
    Fl_EventType b = fl_event_type_register(bus, "test.b", 0);
    CHECK(a != FL_EVENT_TYPE_INVALID && b != FL_EVENT_TYPE_INVALID && a != b);
    CHECK(fl_event_type_register(bus, "test.a", sizeof(Value)) == a);
    CHECK(fl_event_type_register(bus, "test.a", sizeof(Value) + 1) == FL_EVENT_TYPE_INVALID);
    CHECK(fl_event_type_find(bus, "test.b") == b);
    CHECK(fl_event_type_find(bus, "missing") == FL_EVENT_TYPE_INVALID);
    CHECK(strcmp(fl_event_type_name(bus, a), "test.a") == 0);
    CHECK(fl_event_type_payload_size(bus, a) == sizeof(Value));
    CHECK(fl_event_type_payload_size(bus, 999) == SIZE_MAX);

    char long_name[200];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(fl_event_type_register(bus, long_name, 0) == FL_EVENT_TYPE_INVALID);
    CHECK(fl_event_type_register(bus, "", 0) == FL_EVENT_TYPE_INVALID);
    CHECK(fl_event_type_register(bus, NULL, 0) == FL_EVENT_TYPE_INVALID);

    for (int i = 0; i < 500; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "grow.%d", i);
        CHECK(fl_event_type_register(bus, name, (size_t)i) != FL_EVENT_TYPE_INVALID);
    }
    CHECK(fl_event_type_find(bus, "test.a") == a);
    CHECK(fl_event_bus_destroy(bus));
}

static void test_priority_order_and_any(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", sizeof(Value));
    Trace trace = {0};
    Probe p1 = { .trace = &trace, .id = 1 }, p2 = { .trace = &trace, .id = 2 };
    Probe p3 = { .trace = &trace, .id = 3 }, p4 = { .trace = &trace, .id = 4 };
    Probe pa = { .trace = &trace, .id = 9 };

    sub(bus, t, &p1, 0);
    sub(bus, t, &p2, 10);
    sub(bus, t, &p3, 0);
    sub(bus, FL_EVENT_TYPE_ANY, &pa, 5);
    sub(bus, t, &p4, -3);

    Value v = { 42 };
    CHECK(fl_event_publish(bus, t, &v, sizeof(v)) == FL_PUBLISH_DELIVERED);
    const int expected[] = { 2, 9, 1, 3, 4 };
    CHECK(trace.count == 5);
    for (int i = 0; i < 5 && i < trace.count; ++i) CHECK(trace.calls[i] == expected[i]);
    CHECK(p4.last_value == 42 && pa.last_value == 42);
    CHECK(fl_event_bus_destroy(bus));
}

static void test_consume_stops_delivery(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Trace trace = {0};
    Probe first = { .trace = &trace, .id = 1, .consume = true };
    Probe second = { .trace = &trace, .id = 2 };
    sub(bus, t, &first, 1);
    sub(bus, t, &second, 0);
    CHECK(fl_event_publish(bus, t, NULL, 0) == FL_PUBLISH_CONSUMED);
    CHECK(trace.count == 1 && trace.calls[0] == 1);
    CHECK(fl_event_bus_destroy(bus));
}

static void test_payload_validation(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", sizeof(Value));
    Fl_EventType empty = fl_event_type_register(bus, "empty", 0);
    Value v = { 1 };
    CHECK(fl_event_publish(bus, t, &v, sizeof(v) - 1) == FL_PUBLISH_INVALID);
    CHECK(fl_event_publish(bus, t, NULL, sizeof(v)) == FL_PUBLISH_INVALID);
    CHECK(fl_event_publish(bus, FL_EVENT_TYPE_ANY, &v, sizeof(v)) == FL_PUBLISH_INVALID);
    CHECK(fl_event_publish(bus, FL_EVENT_TYPE_INVALID, NULL, 0) == FL_PUBLISH_INVALID);
    CHECK(fl_event_publish(bus, 77, NULL, 0) == FL_PUBLISH_INVALID);
    CHECK(fl_event_publish(bus, empty, NULL, 0) == FL_PUBLISH_DELIVERED);
    CHECK(!fl_event_post(bus, t, &v, 2));
    CHECK(!fl_event_post(bus, 77, NULL, 0));
    CHECK(!fl_event_bus_has_pending(bus));
    CHECK(fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t }).generation == 0);
    CHECK(fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = 77, .fn = record }).generation == 0);
    CHECK(fl_event_bus_destroy(bus));
}

/* Unsubscribes itself and a later observer, then publishes a nested event. */
static bool reentrant_observer(const Fl_Event *event, void *user_data)
{
    Probe *probe = user_data;
    trace_push(probe->trace, probe->id);
    if (event->type != probe->type) return false;
    CHECK(fl_event_unsubscribe(probe->bus, probe->self));
    CHECK(fl_event_unsubscribe(probe->bus, probe->other));
    CHECK(!fl_event_unsubscribe(probe->bus, probe->other));
    return false;
}

static void test_unsubscribe_during_dispatch(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Trace trace = {0};
    Probe killer = { .trace = &trace, .id = 1, .bus = bus, .type = t };
    Probe victim = { .trace = &trace, .id = 2 };
    Probe survivor = { .trace = &trace, .id = 3 };

    killer.self = fl_event_subscribe(bus, &(Fl_SubscribeDesc){
        .type = t, .fn = reentrant_observer, .user_data = &killer, .priority = 2 });
    killer.other = sub(bus, t, &victim, 1);
    Fl_Subscription survivor_sub = sub(bus, t, &survivor, 0);

    CHECK(fl_event_publish(bus, t, NULL, 0) == FL_PUBLISH_DELIVERED);
    CHECK(trace.count == 2 && trace.calls[0] == 1 && trace.calls[1] == 3);
    CHECK(!fl_event_subscription_alive(bus, killer.self));
    CHECK(fl_event_subscription_alive(bus, survivor_sub));

    trace.count = 0;
    CHECK(fl_event_publish(bus, t, NULL, 0) == FL_PUBLISH_DELIVERED);
    CHECK(trace.count == 1 && trace.calls[0] == 3);
    CHECK(fl_event_bus_destroy(bus));
}

typedef struct Adder {
    Fl_EventBus *bus;
    Fl_EventType type;
    Probe       *late;
    Fl_Subscription added;
    int          runs;
} Adder;

static bool add_during_dispatch(const Fl_Event *event, void *user_data)
{
    (void)event;
    Adder *adder = user_data;
    if (adder->runs++ == 0) {
        adder->added = sub(adder->bus, adder->type, adder->late, 100);
        CHECK(fl_event_subscription_alive(adder->bus, adder->added));
    }
    return false;
}

static void test_subscribe_during_dispatch_is_deferred(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Trace trace = {0};
    Probe late = { .trace = &trace, .id = 7 };
    Adder adder = { .bus = bus, .type = t, .late = &late };
    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t, .fn = add_during_dispatch, .user_data = &adder });

    fl_event_publish(bus, t, NULL, 0);
    CHECK(trace.count == 0);
    fl_event_publish(bus, t, NULL, 0);
    CHECK(trace.count == 1 && trace.calls[0] == 7);
    CHECK(fl_event_bus_destroy(bus));
}

static void test_pending_subscription_cancelled_before_flush(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Trace trace = {0};
    Probe late = { .trace = &trace, .id = 7 };
    Adder adder = { .bus = bus, .type = t, .late = &late };

    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t, .fn = add_during_dispatch, .user_data = &adder, .priority = 1 });
    fl_event_publish(bus, t, NULL, 0);
    CHECK(fl_event_unsubscribe(bus, adder.added));
    CHECK(!fl_event_subscription_alive(bus, adder.added));
    fl_event_publish(bus, t, NULL, 0);
    CHECK(trace.count == 0);
    CHECK(fl_event_bus_destroy(bus));
}

typedef struct Nester {
    Fl_EventBus *bus;
    Fl_EventType type;
    int          depth_reached;
    int          exceeded;
} Nester;

static bool nest(const Fl_Event *event, void *user_data)
{
    (void)event;
    Nester *nester = user_data;
    nester->depth_reached++;
    if (fl_event_publish(nester->bus, nester->type, NULL, 0) == FL_PUBLISH_DEPTH_EXCEEDED)
        nester->exceeded++;
    return false;
}

static void test_nested_publish_depth_limit(void)
{
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){ .max_depth = 4 });
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Nester nester = { .bus = bus, .type = t };
    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t, .fn = nest, .user_data = &nester });
    CHECK(fl_event_publish(bus, t, NULL, 0) == FL_PUBLISH_DELIVERED);
    CHECK(nester.depth_reached == 4);
    CHECK(nester.exceeded == 1);
    CHECK(fl_event_bus_destroy(bus));
}

static bool try_destroy(const Fl_Event *event, void *user_data)
{
    (void)event;
    Fl_EventBus *bus = user_data;
    CHECK(!fl_event_bus_destroy(bus));
    CHECK(fl_event_bus_dispatch(bus) == 0);
    return false;
}

static void test_destroy_refused_inside_dispatch(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t, .fn = try_destroy, .user_data = bus });
    fl_event_publish(bus, t, NULL, 0);
    CHECK(fl_event_post(bus, t, NULL, 0));
    CHECK(fl_event_bus_dispatch(bus) == 1);
    CHECK(fl_event_bus_destroy(bus));
}

static void test_owner_bulk_unsubscribe_and_generations(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Trace trace = {0};
    Probe a = { .trace = &trace, .id = 1 }, b = { .trace = &trace, .id = 2 };
    Probe c = { .trace = &trace, .id = 3 };
    int owner_key = 0;

    Fl_Subscription sa = fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t, .fn = record, .user_data = &a, .owner = &owner_key });
    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = FL_EVENT_TYPE_ANY, .fn = record, .user_data = &b, .owner = &owner_key });
    sub(bus, t, &c, 0);

    CHECK(fl_event_unsubscribe_owner(bus, &owner_key) == 2);
    CHECK(fl_event_unsubscribe_owner(bus, &owner_key) == 0);
    CHECK(fl_event_unsubscribe_owner(bus, NULL) == 0);
    fl_event_publish(bus, t, NULL, 0);
    CHECK(trace.count == 1 && trace.calls[0] == 3);

    Probe d = { .trace = &trace, .id = 4 };
    Fl_Subscription sd = sub(bus, t, &d, 0);
    CHECK(!fl_event_subscription_alive(bus, sa));
    CHECK(!fl_event_unsubscribe(bus, sa));
    CHECK(fl_event_subscription_alive(bus, sd));
    CHECK(!fl_event_subscription_alive(bus, (Fl_Subscription){0}));
    CHECK(fl_event_bus_destroy(bus));
}

typedef struct WakeCounter {
    Fl_Mutex lock;
    int      wakes;
} WakeCounter;

static void count_wake(void *user_data)
{
    WakeCounter *counter = user_data;
    fl_mutex_lock(&counter->lock);
    counter->wakes++;
    fl_mutex_unlock(&counter->lock);
}

typedef struct Producer {
    Fl_EventBus *bus;
    Fl_EventType type;
    int          base;
    int          count;
    int          rejected;
} Producer;

static void produce(void *user_data)
{
    Producer *producer = user_data;
    for (int i = 0; i < producer->count; ++i) {
        Value v = { producer->base + i };
        if (!fl_event_post(producer->bus, producer->type, &v, sizeof(v))) producer->rejected++;
    }
}

typedef struct Collector {
    int last_seen[4];
    int total;
    bool in_order;
    uint64_t last_sequence;
} Collector;

static bool collect(const Fl_Event *event, void *user_data)
{
    Collector *collector = user_data;
    int v = ((const Value *)event->payload)->v;
    int producer = v / 100000;
    int index = v % 100000;
    if (producer < 0 || producer >= 4 || index != collector->last_seen[producer] + 1)
        collector->in_order = false;
    else
        collector->last_seen[producer] = index;
    if (event->sequence <= collector->last_sequence) collector->in_order = false;
    collector->last_sequence = event->sequence;
    collector->total++;
    return false;
}

static void test_cross_thread_post(void)
{
    WakeCounter counter = {0};
    CHECK(fl_mutex_init(&counter.lock));
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){ .wake = count_wake, .wake_data = &counter });
    Fl_EventType t = fl_event_type_register(bus, "t", sizeof(Value));

    Collector collector = { .in_order = true, .last_seen = { -1, -1, -1, -1 } };
    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t, .fn = collect, .user_data = &collector });

    enum { PER_THREAD = 5000 };
    Producer producers[4];
    Fl_Thread *threads[4];
    for (int i = 0; i < 4; ++i) {
        producers[i] = (Producer){ .bus = bus, .type = t, .base = i * 100000, .count = PER_THREAD };
        threads[i] = fl_thread_create(produce, &producers[i]);
        CHECK(threads[i] != NULL);
    }
    while (collector.total < 4 * PER_THREAD)
        fl_event_bus_dispatch(bus);
    for (int i = 0; i < 4; ++i) {
        CHECK(fl_thread_join(threads[i]));
        CHECK(producers[i].rejected == 0);
    }

    CHECK(collector.total == 4 * PER_THREAD);
    CHECK(collector.in_order);
    CHECK(!fl_event_bus_has_pending(bus));
    CHECK(counter.wakes >= 1);
    CHECK(fl_event_bus_destroy(bus));
    fl_mutex_destroy(&counter.lock);
}

typedef struct ForeignCall {
    Fl_EventBus *bus;
    Fl_EventType type;
    bool publish_rejected;
    bool subscribe_rejected;
    bool destroy_rejected;
    bool dispatch_rejected;
} ForeignCall;

static void call_from_foreign_thread(void *user_data)
{
    ForeignCall *call = user_data;
    call->publish_rejected = fl_event_publish(call->bus, call->type, NULL, 0) == FL_PUBLISH_INVALID;
    call->subscribe_rejected = fl_event_subscribe(call->bus, &(Fl_SubscribeDesc){
        .type = call->type, .fn = record }).generation == 0;
    call->destroy_rejected = !fl_event_bus_destroy(call->bus);
    call->dispatch_rejected = fl_event_bus_dispatch(call->bus) == 0;
}

static void test_owner_thread_enforced(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    ForeignCall call = { .bus = bus, .type = fl_event_type_register(bus, "t", 0) };
    Fl_Thread *thread = fl_thread_create(call_from_foreign_thread, &call);
    CHECK(fl_thread_join(thread));
    CHECK(call.publish_rejected && call.subscribe_rejected);
    CHECK(call.destroy_rejected && call.dispatch_rejected);
    CHECK(fl_event_bus_destroy(bus));
}

typedef struct Reposter {
    Fl_EventBus *bus;
    Fl_EventType type;
    int          delivered;
} Reposter;

static bool repost(const Fl_Event *event, void *user_data)
{
    (void)event;
    Reposter *reposter = user_data;
    if (reposter->delivered++ < 3) CHECK(fl_event_post(reposter->bus, reposter->type, NULL, 0));
    return false;
}

static void test_post_during_dispatch_waits_for_next_round(void)
{
    WakeCounter counter = {0};
    CHECK(fl_mutex_init(&counter.lock));
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){ .wake = count_wake, .wake_data = &counter });
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Reposter reposter = { .bus = bus, .type = t };
    fl_event_subscribe(bus, &(Fl_SubscribeDesc){ .type = t, .fn = repost, .user_data = &reposter });

    CHECK(fl_event_post(bus, t, NULL, 0));
    CHECK(counter.wakes == 1);
    CHECK(fl_event_bus_dispatch(bus) == 1);
    CHECK(fl_event_bus_has_pending(bus));
    CHECK(counter.wakes >= 2);
    while (fl_event_bus_dispatch(bus) > 0) {
    }
    CHECK(reposter.delivered == 4);
    CHECK(!fl_event_bus_has_pending(bus));
    CHECK(fl_event_bus_destroy(bus));
    fl_mutex_destroy(&counter.lock);
}

static void test_many_subscribers_and_churn(void)
{
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_EventType t = fl_event_type_register(bus, "t", 0);
    Trace trace = {0};
    static Probe probes[2000];
    static Fl_Subscription subs[2000];
    for (int i = 0; i < 2000; ++i) {
        probes[i] = (Probe){ .trace = &trace, .id = i };
        subs[i] = sub(bus, t, &probes[i], i % 7);
        CHECK(subs[i].generation != 0);
    }
    for (int i = 0; i < 2000; i += 2) CHECK(fl_event_unsubscribe(bus, subs[i]));
    for (int i = 0; i < 1000; ++i) CHECK(sub(bus, t, &probes[i], 0).generation != 0);
    CHECK(fl_event_bus_destroy(bus));
}

int main(void)
{
    test_type_registry();
    test_priority_order_and_any();
    test_consume_stops_delivery();
    test_payload_validation();
    test_unsubscribe_during_dispatch();
    test_subscribe_during_dispatch_is_deferred();
    test_pending_subscription_cancelled_before_flush();
    test_nested_publish_depth_limit();
    test_destroy_refused_inside_dispatch();
    test_owner_bulk_unsubscribe_and_generations();
    test_cross_thread_post();
    test_owner_thread_enforced();
    test_post_during_dispatch_waits_for_next_round();
    test_many_subscribers_and_churn();

    if (g_failures) {
        fprintf(stderr, "fl_event_tests: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("fl_event_tests: all passed\n");
    return 0;
}
