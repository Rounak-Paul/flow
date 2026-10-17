#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
typedef struct Fl_Mutex {
    void *srw;
} Fl_Mutex;
typedef struct Fl_CondVar {
    void *cv;
} Fl_CondVar;
/* Static initialiser for an Fl_Mutex with static storage duration. */
#define FL_MUTEX_INITIALIZER { 0 }
#else
#include <pthread.h>
typedef struct Fl_Mutex {
    pthread_mutex_t handle;
} Fl_Mutex;
typedef struct Fl_CondVar {
    pthread_cond_t handle;
} Fl_CondVar;
/* Static initialiser for an Fl_Mutex with static storage duration. */
#define FL_MUTEX_INITIALIZER { PTHREAD_MUTEX_INITIALIZER }
#endif

/* Maximum thread name length, excluding the terminator. */
#define FL_THREAD_NAME_MAX 31u

typedef struct Fl_Thread Fl_Thread;

/*
 * Thread entry point.
 *
 * user_data  Pointer passed to fl_thread_create.
 */
typedef void (*Fl_ThreadFn)(void *user_data);

/*
 * Start a joinable OS thread.
 *
 * fn         Entry point; required.
 * user_data  Passed unchanged to fn; must outlive the thread.
 * Returns    Thread handle, or NULL on allocation or OS failure.
 */
Fl_Thread *fl_thread_create(Fl_ThreadFn fn, void *user_data);

/*
 * Wait for a thread to finish and release its handle.
 *
 * thread   Handle from fl_thread_create; invalid after this call.
 * Returns  true when the thread was joined; false for NULL or an OS failure.
 */
bool fl_thread_join(Fl_Thread *thread);

/*
 * Identifies the calling OS thread.
 *
 * Returns  A non-zero identifier that is unique among live threads.
 */
uint64_t fl_thread_current_id(void);

/*
 * Name the calling thread for debuggers, profilers, logs and crash reports.
 * Longer names are truncated to FL_THREAD_NAME_MAX bytes.
 *
 * name  UTF-8 name; NULL or empty clears the Flow-side name.
 */
void fl_thread_set_name(const char *name);

/*
 * Name of the calling thread as set by fl_thread_set_name.
 *
 * Returns  Thread-local name, or "" when unnamed. Valid until the next
 *          fl_thread_set_name on this thread.
 */
const char *fl_thread_current_name(void);

/*
 * Number of logical processors available to the process.
 *
 * Returns  At least 1.
 */
uint32_t fl_thread_hardware_concurrency(void);

/*
 * Initialise a non-recursive mutex in caller-owned storage.
 *
 * mutex    Storage to initialise; must not already be initialised.
 * Returns  true on success, false when the OS refuses to create the mutex.
 */
bool fl_mutex_init(Fl_Mutex *mutex);

/*
 * Release the OS resources of an initialised, unlocked mutex.
 *
 * mutex  Mutex previously initialised with fl_mutex_init.
 */
void fl_mutex_destroy(Fl_Mutex *mutex);

/*
 * Block until the calling thread owns the mutex.
 *
 * mutex  Initialised mutex not already held by the calling thread.
 */
void fl_mutex_lock(Fl_Mutex *mutex);

/*
 * Release a mutex held by the calling thread.
 *
 * mutex  Mutex locked by the calling thread.
 */
void fl_mutex_unlock(Fl_Mutex *mutex);

/*
 * Initialise a condition variable in caller-owned storage.
 *
 * Returns  true on success, false when the OS refuses to create it.
 */
bool fl_cond_init(Fl_CondVar *cond);

/*
 * Release an initialised condition variable with no waiters.
 */
void fl_cond_destroy(Fl_CondVar *cond);

/*
 * Atomically release mutex and wait for a signal, then reacquire mutex.
 * Spurious wake-ups are possible; callers must re-check their predicate.
 *
 * cond   Condition variable to wait on.
 * mutex  Mutex held by the calling thread.
 */
void fl_cond_wait(Fl_CondVar *cond, Fl_Mutex *mutex);

/*
 * Wake one thread waiting on cond.
 */
void fl_cond_signal(Fl_CondVar *cond);

/*
 * Wake every thread waiting on cond.
 */
void fl_cond_broadcast(Fl_CondVar *cond);
