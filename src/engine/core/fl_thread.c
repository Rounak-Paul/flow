#include "fl_thread.h"

#include <stdlib.h>

#if defined(_WIN32)

#include <process.h>
#include <windows.h>

struct Fl_Thread {
    HANDLE      handle;
    Fl_ThreadFn fn;
    void       *user_data;
};

static unsigned __stdcall thread_trampoline(void *arg)
{
    Fl_Thread *thread = arg;
    thread->fn(thread->user_data);
    return 0;
}

Fl_Thread *fl_thread_create(Fl_ThreadFn fn, void *user_data)
{
    if (!fn) return NULL;
    Fl_Thread *thread = calloc(1, sizeof(*thread));
    if (!thread) return NULL;
    thread->fn = fn;
    thread->user_data = user_data;
    uintptr_t handle = _beginthreadex(NULL, 0, thread_trampoline, thread, 0, NULL);
    if (!handle) {
        free(thread);
        return NULL;
    }
    thread->handle = (HANDLE)handle;
    return thread;
}

bool fl_thread_join(Fl_Thread *thread)
{
    if (!thread) return false;
    bool joined = WaitForSingleObject(thread->handle, INFINITE) == WAIT_OBJECT_0;
    CloseHandle(thread->handle);
    free(thread);
    return joined;
}

uint64_t fl_thread_current_id(void)
{
    return (uint64_t)GetCurrentThreadId();
}

bool fl_mutex_init(Fl_Mutex *mutex)
{
    if (!mutex) return false;
    SRWLOCK lock = SRWLOCK_INIT;
    mutex->srw = lock.Ptr;
    return true;
}

void fl_mutex_destroy(Fl_Mutex *mutex)
{
    (void)mutex;
}

void fl_mutex_lock(Fl_Mutex *mutex)
{
    AcquireSRWLockExclusive((PSRWLOCK)&mutex->srw);
}

void fl_mutex_unlock(Fl_Mutex *mutex)
{
    ReleaseSRWLockExclusive((PSRWLOCK)&mutex->srw);
}

#else

struct Fl_Thread {
    pthread_t   handle;
    Fl_ThreadFn fn;
    void       *user_data;
};

static void *thread_trampoline(void *arg)
{
    Fl_Thread *thread = arg;
    thread->fn(thread->user_data);
    return NULL;
}

Fl_Thread *fl_thread_create(Fl_ThreadFn fn, void *user_data)
{
    if (!fn) return NULL;
    Fl_Thread *thread = calloc(1, sizeof(*thread));
    if (!thread) return NULL;
    thread->fn = fn;
    thread->user_data = user_data;
    if (pthread_create(&thread->handle, NULL, thread_trampoline, thread) != 0) {
        free(thread);
        return NULL;
    }
    return thread;
}

bool fl_thread_join(Fl_Thread *thread)
{
    if (!thread) return false;
    bool joined = pthread_join(thread->handle, NULL) == 0;
    free(thread);
    return joined;
}

uint64_t fl_thread_current_id(void)
{
    static _Thread_local char marker;
    return (uint64_t)(uintptr_t)&marker;
}

bool fl_mutex_init(Fl_Mutex *mutex)
{
    return mutex && pthread_mutex_init(&mutex->handle, NULL) == 0;
}

void fl_mutex_destroy(Fl_Mutex *mutex)
{
    if (mutex) pthread_mutex_destroy(&mutex->handle);
}

void fl_mutex_lock(Fl_Mutex *mutex)
{
    pthread_mutex_lock(&mutex->handle);
}

void fl_mutex_unlock(Fl_Mutex *mutex)
{
    pthread_mutex_unlock(&mutex->handle);
}

#endif
