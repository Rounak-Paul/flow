#pragma once
#include "project/fl_project.h"

/*
 * Linear undo/redo over whole changesets. An entry owns the project state before the changeset; undo and redo
 * are refused when anything else changed the document since the history last touched it, so no edit is ever
 * overwritten silently. Owner thread only.
 */
typedef struct Fl_History Fl_History;

#define FL_HISTORY_LABEL_MAX 80u

/* Create a history keeping at most max_entries undo entries and max_bytes of snapshots; NULL on failure. */
Fl_History *fl_history_create(size_t max_entries, size_t max_bytes);
/* Release every snapshot. NULL is safe. */
void fl_history_destroy(Fl_History *history);
/* Drop all entries, e.g. when the project changes. */
void fl_history_clear(Fl_History *history);

/* Capture the state a changeset starts from. Any history made stale by outside edits is discarded first.
   False on allocation failure or when a changeset is already open. */
bool fl_history_begin(Fl_History *history, const Fl_Project *project, const char *label);
/* Keep the open changeset as an undo entry and drop redo entries. */
bool fl_history_commit(Fl_History *history, const Fl_Project *project);
/* Restore the project to the state at begin and close the changeset; false when restoring failed. */
bool fl_history_rollback(Fl_History *history, Fl_Project *project);
/* Close the changeset without keeping an entry (it changed nothing). */
void fl_history_discard(Fl_History *history);

/* Undo/redo the newest entry. On success label receives its name (capacity FL_HISTORY_LABEL_MAX).
   *conflict is set when refused because the document changed outside the history. */
bool fl_history_undo(Fl_History *history, Fl_Project *project, char label[FL_HISTORY_LABEL_MAX], bool *conflict);
bool fl_history_redo(Fl_History *history, Fl_Project *project, char label[FL_HISTORY_LABEL_MAX], bool *conflict);
/* Number of entries available to undo and redo. */
size_t fl_history_undo_count(const Fl_History *history);
size_t fl_history_redo_count(const Fl_History *history);
/* Borrow the label of the entry the next undo/redo would apply, or NULL. */
const char *fl_history_undo_label(const Fl_History *history);
const char *fl_history_redo_label(const Fl_History *history);

/* Whether any retained undo/redo/open snapshot requires these asset bytes. */
bool fl_history_has_asset(const Fl_History *history, const unsigned char hash[32]);

/* Close an engine-owned changeset after its document backup has already been restored. */
void fl_history_cancel_restored(Fl_History *history, const Fl_Project *project);
