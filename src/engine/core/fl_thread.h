#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
typedef struct Fl_Mutex {
    void *srw;
} Fl_Mutex;
#else
#include <pthread.h>
typedef struct Fl_Mutex {
    pthread_mutex_t handle;
} Fl_Mutex;
#endif

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
