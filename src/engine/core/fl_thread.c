#include "fl_thread.h"

#include <stdlib.h>
#include <string.h>

static _Thread_local char g_thread_name[FL_THREAD_NAME_MAX + 1u];

static void store_name(const char *name)
{
    size_t len = 0;
    if (name)
        while (len < FL_THREAD_NAME_MAX && name[len]) ++len;
    if (len) memcpy(g_thread_name, name, len);
    g_thread_name[len] = '\0';
}

const char *fl_thread_current_name(void)
{
    return g_thread_name;
}

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

typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);

void fl_thread_set_name(const char *name)
{
    store_name(name);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    SetThreadDescriptionFn set_description =
        (SetThreadDescriptionFn)(void (*)(void))GetProcAddress(kernel, "SetThreadDescription");
    if (!set_description) return;
    wchar_t wide[FL_THREAD_NAME_MAX + 1u];
    if (MultiByteToWideChar(CP_UTF8, 0, g_thread_name, -1, wide, (int)(FL_THREAD_NAME_MAX + 1u)) > 0)
        set_description(GetCurrentThread(), wide);
}

uint32_t fl_thread_hardware_concurrency(void)
{
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count ? (uint32_t)count : 1u;
}

bool fl_mutex_init(Fl_Mutex *mutex)
{
    if (!mutex) return false;
    InitializeSRWLock((PSRWLOCK)&mutex->srw);
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

bool fl_cond_init(Fl_CondVar *cond)
{
    if (!cond) return false;
    InitializeConditionVariable((PCONDITION_VARIABLE)&cond->cv);
    return true;
}

void fl_cond_destroy(Fl_CondVar *cond)
{
    (void)cond;
}

void fl_cond_wait(Fl_CondVar *cond, Fl_Mutex *mutex)
{
    SleepConditionVariableSRW((PCONDITION_VARIABLE)&cond->cv, (PSRWLOCK)&mutex->srw, INFINITE, 0);
}

bool fl_cond_wait_ms(Fl_CondVar *cond, Fl_Mutex *mutex, uint32_t timeout_ms)
{
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)&cond->cv, (PSRWLOCK)&mutex->srw, timeout_ms, 0) != 0;
}

void fl_cond_signal(Fl_CondVar *cond)
{
    WakeConditionVariable((PCONDITION_VARIABLE)&cond->cv);
}

void fl_cond_broadcast(Fl_CondVar *cond)
{
    WakeAllConditionVariable((PCONDITION_VARIABLE)&cond->cv);
}

#else

#include <errno.h>
#include <time.h>
#include <unistd.h>

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

void fl_thread_set_name(const char *name)
{
    store_name(name);
    /* Linux limits kernel thread names to 15 bytes plus terminator. */
    char os_name[16];
    size_t len = strlen(g_thread_name);
    if (len > sizeof(os_name) - 1u) len = sizeof(os_name) - 1u;
    memcpy(os_name, g_thread_name, len);
    os_name[len] = '\0';
#if defined(__APPLE__)
    pthread_setname_np(os_name);
#else
    pthread_setname_np(pthread_self(), os_name);
#endif
}

uint32_t fl_thread_hardware_concurrency(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (uint32_t)count : 1u;
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

bool fl_cond_init(Fl_CondVar *cond)
{
    return cond && pthread_cond_init(&cond->handle, NULL) == 0;
}

void fl_cond_destroy(Fl_CondVar *cond)
{
    if (cond) pthread_cond_destroy(&cond->handle);
}

void fl_cond_wait(Fl_CondVar *cond, Fl_Mutex *mutex)
{
    pthread_cond_wait(&cond->handle, &mutex->handle);
}

bool fl_cond_wait_ms(Fl_CondVar *cond, Fl_Mutex *mutex, uint32_t timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000u;
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {++deadline.tv_sec; deadline.tv_nsec -= 1000000000L;}
    return pthread_cond_timedwait(&cond->handle, &mutex->handle, &deadline) != ETIMEDOUT;
}

void fl_cond_signal(Fl_CondVar *cond)
{
    pthread_cond_signal(&cond->handle);
}

void fl_cond_broadcast(Fl_CondVar *cond)
{
    pthread_cond_broadcast(&cond->handle);
}

#endif
