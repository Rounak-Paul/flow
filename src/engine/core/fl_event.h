#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Flow event bus — the observer backbone every subsystem communicates through.
 *
 * Ownership and threading
 *   A bus is owned by the thread that creates it (the owner thread). Every
 *   function is owner-thread only except fl_event_post, which any thread may
 *   call while the bus is alive. Producer threads must stop posting before the
 *   bus is destroyed.
 *
 * Event types
 *   Types are registered at runtime by name with a fixed payload size, so
 *   modules declare their own events without a central list. Registering an
 *   existing name with the same payload size returns the existing type.
 *
 * Delivery
 *   fl_event_publish delivers synchronously on the owner thread. Subscribers
 *   run in descending priority, ties in subscription order; subscribers to
 *   FL_EVENT_TYPE_ANY take part in the same ordering. A subscriber returning
 *   true consumes the event and stops further delivery.
 *
 *   Publishing from inside a subscriber dispatches the nested event
 *   immediately, up to the bus's maximum dispatch depth. Subscriptions added
 *   during a dispatch take effect once the outermost dispatch returns;
 *   unsubscribed observers are never invoked again, even mid-dispatch.
 *
 *   fl_event_post copies the payload into a thread-safe queue and invokes the
 *   bus's wake hook when the queue becomes non-empty. The owner thread delivers
 *   queued events, in post order, by calling fl_event_bus_dispatch.
 *
 * Payloads
 *   Payloads are plain bytes of exactly the registered size. A published
 *   payload is borrowed for the duration of delivery; a posted payload is
 *   copied, so any pointers inside it must stay valid until delivered.
 */

typedef struct Fl_EventBus Fl_EventBus;

/* Runtime-registered event type identifier. */
typedef uint32_t Fl_EventType;

#define FL_EVENT_TYPE_INVALID ((Fl_EventType)0u)
#define FL_EVENT_TYPE_ANY     ((Fl_EventType)UINT32_MAX)

/* Default nesting limit for fl_event_publish called from subscribers. */
#define FL_EVENT_DEFAULT_MAX_DEPTH 64u

/* Maximum length of an event type name, excluding the terminator. */
#define FL_EVENT_NAME_MAX 127u

/* Delivered event. Valid only for the duration of the subscriber call. */
typedef struct Fl_Event {
    Fl_EventType type;
    const void  *payload;      /* NULL when payload_size is 0           */
    size_t       payload_size;
    uint64_t     sequence;     /* bus-wide delivery order, starting at 1 */
} Fl_Event;

/*
 * Observer callback.
 *
 * event      Event being delivered.
 * user_data  Pointer supplied at subscription.
 * Returns    true to consume the event and stop delivery to later subscribers.
 */
typedef bool (*Fl_EventFn)(const Fl_Event *event, void *user_data);

/*
 * Wake hook invoked from fl_event_post (on the posting thread) when the queue
 * transitions from empty to non-empty, and from fl_event_bus_dispatch when
 * events posted during delivery remain queued.
 *
 * user_data  Pointer supplied in Fl_EventBusDesc.
 */
typedef void (*Fl_EventWakeFn)(void *user_data);

/* Subscription handle. Zero-initialised handles are always invalid. */
typedef struct Fl_Subscription {
    uint32_t index;
    uint32_t generation;
} Fl_Subscription;

typedef struct Fl_EventBusDesc {
    Fl_EventWakeFn wake;           /* optional; must be thread-safe          */
    void          *wake_data;
    uint32_t       max_depth;      /* 0 = FL_EVENT_DEFAULT_MAX_DEPTH         */
} Fl_EventBusDesc;

typedef struct Fl_SubscribeDesc {
    Fl_EventType type;             /* registered type or FL_EVENT_TYPE_ANY   */
    Fl_EventFn   fn;               /* required                               */
    void        *user_data;
    int32_t      priority;         /* higher runs first; default 0           */
    const void  *owner;            /* optional group key for bulk removal    */
} Fl_SubscribeDesc;

typedef enum Fl_PublishResult {
    FL_PUBLISH_DELIVERED = 0,      /* every eligible subscriber ran          */
    FL_PUBLISH_CONSUMED,           /* a subscriber stopped delivery          */
    FL_PUBLISH_INVALID,            /* bad bus/type/payload or wrong thread   */
    FL_PUBLISH_DEPTH_EXCEEDED,     /* nesting limit reached; nothing ran     */
} Fl_PublishResult;

/*
 * Create a bus owned by the calling thread.
 *
 * desc     Optional configuration; NULL uses defaults.
 * Returns  A new bus, or NULL on allocation or OS failure.
 */
Fl_EventBus *fl_event_bus_create(const Fl_EventBusDesc *desc);

/*
 * Destroy a bus, dropping queued events and subscriptions.
 *
 * bus      Bus to destroy; NULL is a no-op that succeeds.
 * Returns  false (and leaves the bus intact) when called from a non-owner
 *          thread or from inside a dispatch; true otherwise.
 */
bool fl_event_bus_destroy(Fl_EventBus *bus);

/*
 * Register an event type, or look up an existing one with the same name.
 *
 * bus           Target bus.
 * name          Unique dotted name, e.g. "doc.ops_applied"; 1..127 bytes.
 * payload_size  Exact payload size in bytes; 0 for payload-less events.
 * Returns       The type, or FL_EVENT_TYPE_INVALID on invalid arguments, a
 *               payload-size conflict with an existing registration, a
 *               non-owner thread, or allocation failure.
 */
Fl_EventType fl_event_type_register(Fl_EventBus *bus, const char *name, size_t payload_size);

/*
 * Find a registered type by name.
 *
 * Returns  The type, or FL_EVENT_TYPE_INVALID when unknown or arguments are invalid.
 */
Fl_EventType fl_event_type_find(const Fl_EventBus *bus, const char *name);

/*
 * Name of a registered type.
 *
 * Returns  The bus-owned name, valid for the bus lifetime; NULL when unknown.
 */
const char *fl_event_type_name(const Fl_EventBus *bus, Fl_EventType type);

/*
 * Registered payload size of a type.
 *
 * Returns  Payload size in bytes, or SIZE_MAX when the type is unknown.
 */
size_t fl_event_type_payload_size(const Fl_EventBus *bus, Fl_EventType type);

/*
 * Subscribe an observer.
 *
 * bus      Target bus.
 * desc     Subscription parameters; desc->fn is required.
 * Returns  A valid handle, or a zero handle on invalid arguments, a non-owner
 *          thread, or allocation failure.
 */
Fl_Subscription fl_event_subscribe(Fl_EventBus *bus, const Fl_SubscribeDesc *desc);

/*
 * Remove one subscription. Safe inside subscribers, including the
 * subscription currently being invoked.
 *
 * Returns  true when the handle referred to a live subscription.
 */
bool fl_event_unsubscribe(Fl_EventBus *bus, Fl_Subscription subscription);

/*
 * Remove every subscription registered with the given owner key.
 *
 * owner    Non-NULL owner key passed in Fl_SubscribeDesc.
 * Returns  Number of subscriptions removed.
 */
size_t fl_event_unsubscribe_owner(Fl_EventBus *bus, const void *owner);

/*
 * Report whether a handle still refers to a live subscription.
 */
bool fl_event_subscription_alive(const Fl_EventBus *bus, Fl_Subscription subscription);

/*
 * Deliver an event synchronously on the owner thread.
 *
 * bus           Target bus.
 * type          Registered type (not FL_EVENT_TYPE_ANY).
 * payload       Pointer to payload_size bytes; may be NULL only when 0.
 * payload_size  Must equal the type's registered payload size.
 * Returns       Delivery outcome; see Fl_PublishResult.
 */
Fl_PublishResult fl_event_publish(Fl_EventBus *bus, Fl_EventType type,
                                  const void *payload, size_t payload_size);

/*
 * Queue an event for delivery on the owner thread. Callable from any thread.
 *
 * Returns  true when queued; false on invalid arguments or allocation failure.
 */
bool fl_event_post(Fl_EventBus *bus, Fl_EventType type,
                   const void *payload, size_t payload_size);

/*
 * Deliver every event queued before this call, in post order. Events posted
 * during delivery stay queued for the next call and re-trigger the wake hook.
 *
 * Returns  Number of queued events delivered; 0 on a non-owner thread or
 *          when called from inside a subscriber.
 */
size_t fl_event_bus_dispatch(Fl_EventBus *bus);

/*
 * Report whether queued events are waiting for fl_event_bus_dispatch.
 * Callable from any thread.
 */
bool fl_event_bus_has_pending(Fl_EventBus *bus);
