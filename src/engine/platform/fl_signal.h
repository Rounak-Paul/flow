#pragma once

#include <stdbool.h>

/*
 * Graceful-termination watcher.
 *
 * Converts termination requests — SIGINT, SIGTERM, SIGHUP on POSIX; console
 * Ctrl-C, Ctrl-Break, close, logoff and shutdown on Windows — into a callback
 * running on a dedicated watcher thread (ordinary thread context, so it may
 * lock, allocate and call fl_event_post). Only the first request reaches the
 * callback; any further request terminates the process immediately with
 * status 128+sig, so a hung shutdown can always be forced.
 *
 * Signals the process inherited as ignored (SIGHUP under nohup, SIGINT for
 * background shell jobs) stay ignored, as callers of such processes expect.
 *
 * POSIX: start the watcher before creating any other thread. It blocks the
 * watched signals in the calling thread, and threads created afterwards
 * inherit that mask. Child processes must restore an empty signal mask when
 * spawned.
 */

/*
 * Termination callback.
 *
 * signal_number  SIGINT (2), SIGTERM (15) or SIGHUP (1).
 * user_data      Pointer supplied to fl_termination_watch_start.
 */
typedef void (*Fl_TerminationFn)(int signal_number, void *user_data);

/*
 * Start watching for termination requests.
 *
 * Returns  false on invalid arguments, OS failure, or when already started.
 */
bool fl_termination_watch_start(Fl_TerminationFn fn, void *user_data);

/*
 * Stop the watcher and restore the calling thread's signal mask. No-op when
 * not started.
 */
void fl_termination_watch_stop(void);
