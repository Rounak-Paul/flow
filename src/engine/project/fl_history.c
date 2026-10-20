#include "project/fl_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One changeset: the state before it, the state after it once undone, and the generation each side expects. */
typedef struct Fl_HistoryEntry {
    char label[FL_HISTORY_LABEL_MAX];
    Fl_Snapshot *before, *after;
    uint64_t expected;          /* project generation at which this entry may be undone (undo list) or redone (redo list) */
} Fl_HistoryEntry;

struct Fl_History {
    Fl_HistoryEntry *undo, *redo;
    size_t undo_count, redo_count, undo_capacity, redo_capacity;
    size_t max_entries, max_bytes;
    Fl_HistoryEntry open;
    bool is_open;
};

/* Bytes held by one entry. */
static size_t entry_bytes(const Fl_HistoryEntry *e)
{
    return fl_snapshot_bytes(e->before) + fl_snapshot_bytes(e->after);
}

static void release(Fl_HistoryEntry *e)
{
    fl_snapshot_destroy(e->before); fl_snapshot_destroy(e->after);
    *e = (Fl_HistoryEntry){0};
}

/* Append to a growable entry list. */
static bool push(Fl_HistoryEntry **list, size_t *count, size_t *capacity, const Fl_HistoryEntry *entry)
{
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 8;
        Fl_HistoryEntry *grown = realloc(*list, next * sizeof(**list));
        if (!grown) return false;
        *list = grown; *capacity = next;
    }
    (*list)[(*count)++] = *entry;
    return true;
}

Fl_History *fl_history_create(size_t max_entries, size_t max_bytes)
{
    if (!max_entries || !max_bytes) return NULL;
    Fl_History *h = calloc(1, sizeof(*h));
    if (h) {h->max_entries = max_entries; h->max_bytes = max_bytes;}
    return h;
}

void fl_history_clear(Fl_History *h)
{
    if (!h) return;
    for (size_t i = 0; i < h->undo_count; ++i) release(&h->undo[i]);
    for (size_t i = 0; i < h->redo_count; ++i) release(&h->redo[i]);
    h->undo_count = h->redo_count = 0;
}

void fl_history_destroy(Fl_History *h)
{
    if (!h) return;
    fl_history_clear(h);
    release(&h->open);
    free(h->undo); free(h->redo); free(h);
}

/* Bytes held by every undo and redo entry. */
static size_t total_bytes(const Fl_History *h)
{
    size_t total = 0;
    for (size_t i = 0; i < h->undo_count; ++i) total += entry_bytes(&h->undo[i]);
    for (size_t i = 0; i < h->redo_count; ++i) total += entry_bytes(&h->redo[i]);
    return total;
}

/* Drop the oldest undo entries until both budgets hold; the newest entry always survives. */
static void trim(Fl_History *h)
{
    size_t drop = 0;
    while (h->undo_count - drop > 1 && (h->undo_count - drop > h->max_entries || total_bytes(h) > h->max_bytes)) release(&h->undo[drop++]);
    if (!drop) return;
    memmove(h->undo, h->undo + drop, (h->undo_count - drop) * sizeof(*h->undo));
    h->undo_count -= drop;
}

bool fl_history_begin(Fl_History *h, const Fl_Project *p, const char *label)
{
    if (!h || !p || h->is_open) return false;
    uint64_t generation = fl_project_generation(p);
    bool current = (h->undo_count && h->undo[h->undo_count - 1].expected == generation) ||
                   (h->redo_count && h->redo[h->redo_count - 1].expected == generation);
    if (!current) fl_history_clear(h);
    if (h->undo_count==h->undo_capacity) {
        size_t capacity=h->undo_capacity ? h->undo_capacity*2 : 8;
        Fl_HistoryEntry *entries=realloc(h->undo,capacity*sizeof(*entries));
        if (!entries) return false;
        h->undo=entries; h->undo_capacity=capacity;
    }
    h->open = (Fl_HistoryEntry){0};
    snprintf(h->open.label, sizeof(h->open.label), "%s", label && label[0] ? label : "Agent changeset");
    h->open.before = fl_project_snapshot(p);
    h->is_open = h->open.before != NULL;
    return h->is_open;
}

void fl_history_discard(Fl_History *h)
{
    if (!h || !h->is_open) return;
    release(&h->open); h->is_open = false;
}

bool fl_history_rollback(Fl_History *h, Fl_Project *p)
{
    if (!h || !h->is_open) return false;
    bool ok = fl_project_restore(p, h->open.before);
    fl_history_discard(h);
    /* The document equals its pre-changeset state again, so the entries that were current still are. */
    if (ok && h->undo_count) h->undo[h->undo_count - 1].expected = fl_project_generation(p);
    if (ok && h->redo_count) h->redo[h->redo_count - 1].expected = fl_project_generation(p);
    return ok;
}

bool fl_history_commit(Fl_History *h, const Fl_Project *p)
{
    if (!h || !h->is_open) return false;
    for (size_t i = 0; i < h->redo_count; ++i) release(&h->redo[i]);
    h->redo_count = 0;
    h->open.expected = fl_project_generation(p);
    bool ok = push(&h->undo, &h->undo_count, &h->undo_capacity, &h->open);
    if (ok) {h->open = (Fl_HistoryEntry){0}; trim(h);}
    else release(&h->open);
    h->is_open = false;
    return ok;
}

/* Move the newest entry of one list to the other, stamping both lists' tops with the current generation.
   The destination push is the only fallible step, so it happens before the project is touched. */
static bool transfer(Fl_History *h, Fl_Project *p, bool undo, char label[FL_HISTORY_LABEL_MAX], bool *conflict)
{
    if (!h || !p) return false;
    Fl_HistoryEntry **from = undo ? &h->undo : &h->redo, **to = undo ? &h->redo : &h->undo;
    size_t *from_count = undo ? &h->undo_count : &h->redo_count, *to_count = undo ? &h->redo_count : &h->undo_count;
    size_t *to_capacity = undo ? &h->redo_capacity : &h->undo_capacity;
    if (conflict) *conflict = false;
    if (!h || !p || h->is_open || !*from_count) return false;
    Fl_HistoryEntry *entry = &(*from)[*from_count - 1];
    if (entry->expected != fl_project_generation(p)) {if (conflict) *conflict = true; return false;}
    if (undo && !entry->after && !(entry->after = fl_project_snapshot(p))) return false;
    Fl_HistoryEntry moved = *entry;
    if (!push(to, to_count, to_capacity, &moved)) return false;
    if (!fl_project_restore(p, undo ? entry->before : entry->after)) {--*to_count; return false;}
    (*to)[*to_count - 1].expected = fl_project_generation(p);
    if (label) snprintf(label, FL_HISTORY_LABEL_MAX, "%s", entry->label);
    --*from_count;
    if (*from_count) (*from)[*from_count - 1].expected = fl_project_generation(p);
    return true;
}

bool fl_history_undo(Fl_History *h, Fl_Project *p, char label[FL_HISTORY_LABEL_MAX], bool *conflict)
{
    return transfer(h, p, true, label, conflict);
}

bool fl_history_redo(Fl_History *h, Fl_Project *p, char label[FL_HISTORY_LABEL_MAX], bool *conflict)
{
    return transfer(h, p, false, label, conflict);
}

size_t fl_history_undo_count(const Fl_History *h) {return h ? h->undo_count : 0;}
size_t fl_history_redo_count(const Fl_History *h) {return h ? h->redo_count : 0;}
const char *fl_history_undo_label(const Fl_History *h) {return h && h->undo_count ? h->undo[h->undo_count - 1].label : NULL;}
const char *fl_history_redo_label(const Fl_History *h) {return h && h->redo_count ? h->redo[h->redo_count - 1].label : NULL;}

bool fl_history_has_asset(const Fl_History *h, const unsigned char hash[32])
{
    if (!h || !hash) return false;
    if (fl_snapshot_has_asset(h->open.before,hash) || fl_snapshot_has_asset(h->open.after,hash)) return true;
    for (size_t i=0;i<h->undo_count;++i) if (fl_snapshot_has_asset(h->undo[i].before,hash) || fl_snapshot_has_asset(h->undo[i].after,hash)) return true;
    for (size_t i=0;i<h->redo_count;++i) if (fl_snapshot_has_asset(h->redo[i].before,hash) || fl_snapshot_has_asset(h->redo[i].after,hash)) return true;
    return false;
}

void fl_history_cancel_restored(Fl_History *h, const Fl_Project *p)
{
    if (!h || !p) return;
    fl_history_discard(h);
    if (h->undo_count) h->undo[h->undo_count-1].expected=fl_project_generation(p);
    if (h->redo_count) h->redo[h->redo_count-1].expected=fl_project_generation(p);
}
