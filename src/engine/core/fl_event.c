#include "fl_event.h"

#include "fl_thread.h"

#include <stdlib.h>
#include <string.h>

#define SLOT_NONE        UINT32_MAX
#define LIST_MIN_CAP     8u
#define QUEUE_MIN_CAP    1024u
#define RECORD_ALIGN     _Alignof(MaxAlign)

typedef union MaxAlign {
    long double ld;
    long long   ll;
    double      d;
    void       *p;
    void      (*fn)(void);
} MaxAlign;

typedef enum SlotState {
    SLOT_FREE = 0,
    SLOT_PENDING,   /* subscribed during a dispatch; joins its list on flush */
    SLOT_ACTIVE,
    SLOT_DEAD,      /* unsubscribed; still referenced until flush            */
} SlotState;

typedef struct Slot {
    Fl_EventFn   fn;
    void        *user_data;
    const void  *owner;
    uint64_t     order;
    Fl_EventType type;
    int32_t      priority;
    uint32_t     generation;
    uint32_t     next_free;
    SlotState    state;
} Slot;

/* Slot indices ordered by (priority desc, order asc). */
typedef struct SubList {
    uint32_t *items;
    uint32_t  count;
    uint32_t  cap;
    uint32_t  reserved;   /* capacity promised to pending subscriptions */
    bool      dirty;      /* contains SLOT_DEAD entries                 */
} SubList;

typedef struct TypeInfo {
    char  *name;
    size_t payload_size;
} TypeInfo;

typedef struct Queue {
    unsigned char *data;
    size_t         size;
    size_t         cap;
    size_t         count;
} Queue;

typedef struct RecordHeader {
    Fl_EventType type;
    size_t       payload_size;
} RecordHeader;

struct Fl_EventBus {
    uint64_t       owner_thread;
    Fl_EventWakeFn wake;
    void          *wake_data;
    uint32_t       max_depth;
    uint32_t       depth;
    uint64_t       sequence;
    uint64_t       next_order;
    bool           needs_flush;

    /* Guarded by lock: written on the owner thread, read by fl_event_post. */
    Fl_Mutex       lock;
    TypeInfo      *types;
    uint32_t       type_count;
    uint32_t       type_cap;
    Queue          inbox;

    /* Owner-thread only. */
    SubList       *type_subs;   /* parallel to types, index = type - 1 */
    SubList        any_subs;
    SubList        pending;     /* slot indices in SLOT_PENDING or SLOT_DEAD */
    Slot          *slots;
    uint32_t       slot_count;
    uint32_t       slot_cap;
    uint32_t       free_head;
    Queue          work;
};

static bool on_owner_thread(const Fl_EventBus *bus)
{
    return bus->owner_thread == fl_thread_current_id();
}

static size_t align_up(size_t value, size_t align, bool *ok)
{
    if (value > SIZE_MAX - (align - 1)) {
        *ok = false;
        return 0;
    }
    return (value + align - 1) & ~(align - 1);
}

static uint32_t grow_u32_capacity(uint32_t current, uint32_t needed)
{
    if (needed <= current) return current;
    uint32_t cap = current ? current : LIST_MIN_CAP;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2u) return needed;
        cap *= 2u;
    }
    return cap;
}

static bool list_reserve(SubList *list, uint32_t needed)
{
    if (needed <= list->cap) return true;
    const uint32_t cap = grow_u32_capacity(list->cap, needed);
    if ((size_t)cap > SIZE_MAX / sizeof(uint32_t)) return false;
    uint32_t *items = realloc(list->items, (size_t)cap * sizeof(uint32_t));
    if (!items) return false;
    list->items = items;
    list->cap = cap;
    return true;
}

static void list_free(SubList *list)
{
    free(list->items);
    *list = (SubList){0};
}

static SubList *list_for_type(Fl_EventBus *bus, Fl_EventType type)
{
    return type == FL_EVENT_TYPE_ANY ? &bus->any_subs : &bus->type_subs[type - 1u];
}

/* Insert into an ordered list whose capacity is already sufficient. */
static void list_insert_ordered(Fl_EventBus *bus, SubList *list, uint32_t slot_index)
{
    const int32_t priority = bus->slots[slot_index].priority;
    uint32_t at = list->count;
    for (uint32_t i = 0; i < list->count; ++i) {
        if (bus->slots[list->items[i]].priority < priority) {
            at = i;
            break;
        }
    }
    memmove(&list->items[at + 1u], &list->items[at],
            (size_t)(list->count - at) * sizeof(uint32_t));
    list->items[at] = slot_index;
    list->count++;
}

static bool slot_precedes(const Fl_EventBus *bus, uint32_t a, uint32_t b)
{
    const Slot *sa = &bus->slots[a];
    const Slot *sb = &bus->slots[b];
    if (sa->priority != sb->priority) return sa->priority > sb->priority;
    return sa->order < sb->order;
}

static bool slot_alloc(Fl_EventBus *bus, uint32_t *out_index)
{
    if (bus->free_head != SLOT_NONE) {
        *out_index = bus->free_head;
        bus->free_head = bus->slots[*out_index].next_free;
        return true;
    }
    if (bus->slot_count == SLOT_NONE) return false;
    if (bus->slot_count == bus->slot_cap) {
        uint32_t cap = grow_u32_capacity(bus->slot_cap, bus->slot_count + 1u);
        if (cap == SLOT_NONE) cap = SLOT_NONE - 1u;
        if ((size_t)cap > SIZE_MAX / sizeof(Slot)) return false;
        Slot *slots = realloc(bus->slots, (size_t)cap * sizeof(Slot));
        if (!slots) return false;
        bus->slots = slots;
        bus->slot_cap = cap;
    }
    *out_index = bus->slot_count;
    bus->slots[*out_index] = (Slot){ .generation = 1u, .next_free = SLOT_NONE };
    bus->slot_count++;
    return true;
}

static void slot_release(Fl_EventBus *bus, uint32_t index)
{
    Slot *slot = &bus->slots[index];
    uint32_t generation = slot->generation + 1u;
    *slot = (Slot){
        .generation = generation ? generation : 1u,
        .next_free  = bus->free_head,
        .state      = SLOT_FREE,
    };
    bus->free_head = index;
}

static void list_compact(Fl_EventBus *bus, SubList *list)
{
    if (!list->dirty) return;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list->count; ++i) {
        uint32_t index = list->items[i];
        if (bus->slots[index].state == SLOT_DEAD)
            slot_release(bus, index);
        else
            list->items[kept++] = index;
    }
    list->count = kept;
    list->dirty = false;
}

/* Apply deferred subscription changes. Runs only at dispatch depth 0 and never allocates. */
static void flush(Fl_EventBus *bus)
{
    for (uint32_t i = 0; i < bus->pending.count; ++i) {
        uint32_t index = bus->pending.items[i];
        Slot *slot = &bus->slots[index];
        SubList *list = list_for_type(bus, slot->type);
        list->reserved--;
        if (slot->state == SLOT_PENDING) {
            slot->state = SLOT_ACTIVE;
            list_insert_ordered(bus, list, index);
        } else {
            slot_release(bus, index);
        }
    }
    bus->pending.count = 0;

    for (uint32_t t = 0; t < bus->type_count; ++t)
        list_compact(bus, &bus->type_subs[t]);
    list_compact(bus, &bus->any_subs);
    bus->needs_flush = false;
}

static bool type_valid(const Fl_EventBus *bus, Fl_EventType type)
{
    return type != FL_EVENT_TYPE_INVALID && type != FL_EVENT_TYPE_ANY &&
           type <= bus->type_count;
}

static bool payload_valid(const Fl_EventBus *bus, Fl_EventType type,
                          const void *payload, size_t payload_size)
{
    if (payload_size != bus->types[type - 1u].payload_size) return false;
    return payload_size == 0 || payload != NULL;
}

static Fl_PublishResult deliver(Fl_EventBus *bus, Fl_EventType type,
                                const void *payload, size_t payload_size)
{
    if (bus->depth >= bus->max_depth) return FL_PUBLISH_DEPTH_EXCEEDED;
    bus->depth++;

    const Fl_Event event = {
        .type         = type,
        .payload      = payload_size ? payload : NULL,
        .payload_size = payload_size,
        .sequence     = ++bus->sequence,
    };

    /* Lists are re-fetched by index each step: nested registrations may move
       the containers, but their contents only change at depth 0. */
    Fl_PublishResult result = FL_PUBLISH_DELIVERED;
    uint32_t ti = 0, ai = 0;
    for (;;) {
        const SubList *typed = &bus->type_subs[type - 1u];
        const SubList *any = &bus->any_subs;
        const bool has_typed = ti < typed->count;
        const bool has_any = ai < any->count;
        if (!has_typed && !has_any) break;

        uint32_t index;
        if (has_typed && (!has_any || slot_precedes(bus, typed->items[ti], any->items[ai])))
            index = typed->items[ti++];
        else
            index = any->items[ai++];

        const Slot *slot = &bus->slots[index];
        if (slot->state != SLOT_ACTIVE) continue;
        Fl_EventFn fn = slot->fn;
        void *user_data = slot->user_data;
        if (fn(&event, user_data)) {
            result = FL_PUBLISH_CONSUMED;
            break;
        }
    }

    bus->depth--;
    if (bus->depth == 0 && bus->needs_flush) flush(bus);
    return result;
}

static void mark_dead(Fl_EventBus *bus, uint32_t index)
{
    Slot *slot = &bus->slots[index];
    if (slot->state == SLOT_ACTIVE) list_for_type(bus, slot->type)->dirty = true;
    slot->state = SLOT_DEAD;
    bus->needs_flush = true;
}

static bool queue_reserve(Queue *queue, size_t extra)
{
    if (extra > SIZE_MAX - queue->size) return false;
    size_t needed = queue->size + extra;
    if (needed <= queue->cap) return true;
    size_t cap = queue->cap ? queue->cap : QUEUE_MIN_CAP;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2u) {
            cap = needed;
            break;
        }
        cap *= 2u;
    }
    unsigned char *data = realloc(queue->data, cap);
    if (!data) return false;
    queue->data = data;
    queue->cap = cap;
    return true;
}

static void queue_free(Queue *queue)
{
    free(queue->data);
    *queue = (Queue){0};
}

Fl_EventBus *fl_event_bus_create(const Fl_EventBusDesc *desc)
{
    Fl_EventBus *bus = calloc(1, sizeof(*bus));
    if (!bus) return NULL;
    if (!fl_mutex_init(&bus->lock)) {
        free(bus);
        return NULL;
    }
    bus->owner_thread = fl_thread_current_id();
    bus->wake = desc ? desc->wake : NULL;
    bus->wake_data = desc ? desc->wake_data : NULL;
    bus->max_depth = desc && desc->max_depth ? desc->max_depth : FL_EVENT_DEFAULT_MAX_DEPTH;
    bus->free_head = SLOT_NONE;
    return bus;
}

bool fl_event_bus_destroy(Fl_EventBus *bus)
{
    if (!bus) return true;
    if (!on_owner_thread(bus) || bus->depth > 0) return false;

    for (uint32_t t = 0; t < bus->type_count; ++t) {
        free(bus->types[t].name);
        list_free(&bus->type_subs[t]);
    }
    free(bus->types);
    free(bus->type_subs);
    list_free(&bus->any_subs);
    list_free(&bus->pending);
    free(bus->slots);
    queue_free(&bus->inbox);
    queue_free(&bus->work);
    fl_mutex_destroy(&bus->lock);
    free(bus);
    return true;
}

static Fl_EventType find_type_locked(const Fl_EventBus *bus, const char *name)
{
    for (uint32_t t = 0; t < bus->type_count; ++t)
        if (strcmp(bus->types[t].name, name) == 0) return (Fl_EventType)(t + 1u);
    return FL_EVENT_TYPE_INVALID;
}

static bool name_valid(const char *name)
{
    if (!name || !name[0]) return false;
    for (size_t len = 0; len <= FL_EVENT_NAME_MAX; ++len)
        if (name[len] == '\0') return true;
    return false;
}

Fl_EventType fl_event_type_register(Fl_EventBus *bus, const char *name, size_t payload_size)
{
    if (!bus || !name_valid(name) || !on_owner_thread(bus)) return FL_EVENT_TYPE_INVALID;

    Fl_EventType existing = find_type_locked(bus, name);
    if (existing != FL_EVENT_TYPE_INVALID)
        return bus->types[existing - 1u].payload_size == payload_size ? existing
                                                                       : FL_EVENT_TYPE_INVALID;
    if (bus->type_count >= FL_EVENT_TYPE_ANY - 1u) return FL_EVENT_TYPE_INVALID;

    size_t len = strlen(name);
    char *copy = malloc(len + 1u);
    if (!copy) return FL_EVENT_TYPE_INVALID;
    memcpy(copy, name, len + 1u);

    if (bus->type_count == bus->type_cap) {
        const uint32_t cap = grow_u32_capacity(bus->type_cap, bus->type_count + 1u);
        if ((size_t)cap > SIZE_MAX / sizeof(TypeInfo) ||
            (size_t)cap > SIZE_MAX / sizeof(SubList)) {
            free(copy);
            return FL_EVENT_TYPE_INVALID;
        }
        SubList *subs = realloc(bus->type_subs, (size_t)cap * sizeof(SubList));
        if (!subs) {
            free(copy);
            return FL_EVENT_TYPE_INVALID;
        }
        bus->type_subs = subs;

        fl_mutex_lock(&bus->lock);
        TypeInfo *types = realloc(bus->types, (size_t)cap * sizeof(TypeInfo));
        if (types) {
            bus->types = types;
            bus->type_cap = cap;
        }
        fl_mutex_unlock(&bus->lock);
        if (!types) {
            free(copy);
            return FL_EVENT_TYPE_INVALID;
        }
    }

    bus->type_subs[bus->type_count] = (SubList){0};
    fl_mutex_lock(&bus->lock);
    bus->types[bus->type_count] = (TypeInfo){ .name = copy, .payload_size = payload_size };
    bus->type_count++;
    fl_mutex_unlock(&bus->lock);
    return (Fl_EventType)bus->type_count;
}

Fl_EventType fl_event_type_find(const Fl_EventBus *bus, const char *name)
{
    if (!bus || !name_valid(name) || !on_owner_thread(bus)) return FL_EVENT_TYPE_INVALID;
    return find_type_locked(bus, name);
}

const char *fl_event_type_name(const Fl_EventBus *bus, Fl_EventType type)
{
    if (!bus || !on_owner_thread(bus) || !type_valid(bus, type)) return NULL;
    return bus->types[type - 1u].name;
}

size_t fl_event_type_payload_size(const Fl_EventBus *bus, Fl_EventType type)
{
    if (!bus || !on_owner_thread(bus) || !type_valid(bus, type)) return SIZE_MAX;
    return bus->types[type - 1u].payload_size;
}

Fl_Subscription fl_event_subscribe(Fl_EventBus *bus, const Fl_SubscribeDesc *desc)
{
    const Fl_Subscription none = {0};
    if (!bus || !desc || !desc->fn || !on_owner_thread(bus)) return none;
    if (desc->type != FL_EVENT_TYPE_ANY && !type_valid(bus, desc->type)) return none;

    SubList *list = list_for_type(bus, desc->type);
    const bool deferred = bus->depth > 0;
    if (list->count > UINT32_MAX - list->reserved - 1u) return none;
    if (!list_reserve(list, list->count + list->reserved + 1u)) return none;
    if (deferred && !list_reserve(&bus->pending, bus->pending.count + 1u)) return none;

    uint32_t index;
    if (!slot_alloc(bus, &index)) return none;

    Slot *slot = &bus->slots[index];
    slot->fn = desc->fn;
    slot->user_data = desc->user_data;
    slot->owner = desc->owner;
    slot->priority = desc->priority;
    slot->type = desc->type;
    slot->order = bus->next_order++;

    if (deferred) {
        slot->state = SLOT_PENDING;
        list->reserved++;
        bus->pending.items[bus->pending.count++] = index;
        bus->needs_flush = true;
    } else {
        slot->state = SLOT_ACTIVE;
        list_insert_ordered(bus, list, index);
    }
    return (Fl_Subscription){ .index = index, .generation = slot->generation };
}

static bool handle_live(const Fl_EventBus *bus, Fl_Subscription subscription)
{
    if (subscription.generation == 0 || subscription.index >= bus->slot_count) return false;
    const Slot *slot = &bus->slots[subscription.index];
    return slot->generation == subscription.generation &&
           (slot->state == SLOT_ACTIVE || slot->state == SLOT_PENDING);
}

bool fl_event_unsubscribe(Fl_EventBus *bus, Fl_Subscription subscription)
{
    if (!bus || !on_owner_thread(bus) || !handle_live(bus, subscription)) return false;
    mark_dead(bus, subscription.index);
    if (bus->depth == 0) flush(bus);
    return true;
}

size_t fl_event_unsubscribe_owner(Fl_EventBus *bus, const void *owner)
{
    if (!bus || !owner || !on_owner_thread(bus)) return 0;
    size_t removed = 0;
    for (uint32_t i = 0; i < bus->slot_count; ++i) {
        const Slot *slot = &bus->slots[i];
        if (slot->owner != owner) continue;
        if (slot->state != SLOT_ACTIVE && slot->state != SLOT_PENDING) continue;
        mark_dead(bus, i);
        removed++;
    }
    if (removed && bus->depth == 0) flush(bus);
    return removed;
}

bool fl_event_subscription_alive(const Fl_EventBus *bus, Fl_Subscription subscription)
{
    return bus && on_owner_thread(bus) && handle_live(bus, subscription);
}

Fl_PublishResult fl_event_publish(Fl_EventBus *bus, Fl_EventType type,
                                  const void *payload, size_t payload_size)
{
    if (!bus || !on_owner_thread(bus) || !type_valid(bus, type) ||
        !payload_valid(bus, type, payload, payload_size))
        return FL_PUBLISH_INVALID;
    return deliver(bus, type, payload, payload_size);
}

bool fl_event_post(Fl_EventBus *bus, Fl_EventType type,
                   const void *payload, size_t payload_size)
{
    if (!bus) return false;

    bool ok = true;
    const size_t header = align_up(sizeof(RecordHeader), RECORD_ALIGN, &ok);
    const size_t body = align_up(payload_size, RECORD_ALIGN, &ok);
    if (!ok || body > SIZE_MAX - header) return false;
    const size_t record = header + body;

    fl_mutex_lock(&bus->lock);
    if (!type_valid(bus, type) || !payload_valid(bus, type, payload, payload_size) ||
        !queue_reserve(&bus->inbox, record)) {
        fl_mutex_unlock(&bus->lock);
        return false;
    }
    unsigned char *at = bus->inbox.data + bus->inbox.size;
    const RecordHeader head = { .type = type, .payload_size = payload_size };
    memcpy(at, &head, sizeof(head));
    if (payload_size) memcpy(at + header, payload, payload_size);
    bus->inbox.size += record;
    const bool was_empty = bus->inbox.count++ == 0;
    Fl_EventWakeFn wake = bus->wake;
    void *wake_data = bus->wake_data;
    fl_mutex_unlock(&bus->lock);

    if (was_empty && wake) wake(wake_data);
    return true;
}

size_t fl_event_bus_dispatch(Fl_EventBus *bus)
{
    if (!bus || !on_owner_thread(bus) || bus->depth > 0) return 0;

    fl_mutex_lock(&bus->lock);
    const Queue swapped = bus->work;
    bus->work = bus->inbox;
    bus->inbox = (Queue){ .data = swapped.data, .cap = swapped.cap };
    fl_mutex_unlock(&bus->lock);

    bool ok = true;
    const size_t header = align_up(sizeof(RecordHeader), RECORD_ALIGN, &ok);
    size_t offset = 0;
    size_t delivered = 0;
    for (size_t i = 0; i < bus->work.count; ++i) {
        RecordHeader head;
        memcpy(&head, bus->work.data + offset, sizeof(head));
        const unsigned char *payload = bus->work.data + offset + header;
        deliver(bus, head.type, head.payload_size ? payload : NULL, head.payload_size);
        offset += header + align_up(head.payload_size, RECORD_ALIGN, &ok);
        delivered++;
    }
    bus->work.size = 0;
    bus->work.count = 0;

    fl_mutex_lock(&bus->lock);
    const bool remaining = bus->inbox.count > 0;
    Fl_EventWakeFn wake = bus->wake;
    void *wake_data = bus->wake_data;
    fl_mutex_unlock(&bus->lock);

    if (remaining && wake) wake(wake_data);
    return delivered;
}

bool fl_event_bus_has_pending(Fl_EventBus *bus)
{
    if (!bus) return false;
    fl_mutex_lock(&bus->lock);
    const bool pending = bus->inbox.count > 0;
    fl_mutex_unlock(&bus->lock);
    return pending;
}
