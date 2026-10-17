#include "fl_signal.h"

#include "core/fl_atomic.h"
#include "core/fl_thread.h"

#include <stdint.h>
#include <stdlib.h>

#define SIGNAL_INTERRUPT 2
#define SIGNAL_HANGUP    1
#define SIGNAL_TERMINATE 15

typedef struct Watcher {
    Fl_TerminationFn  fn;
    void             *user_data;
    volatile uint32_t requests;
    bool              started;
} Watcher;

static Watcher g_watcher;

/* Deliver one request; the second one kills the process without cleanup. */
static void deliver(int signal_number)
{
    if (fl_atomic_fetch_add_u32(&g_watcher.requests, 1u) > 0u) _Exit(128 + signal_number);
    g_watcher.fn(signal_number, g_watcher.user_data);
}

#if defined(_WIN32)

#include <windows.h>

static BOOL WINAPI console_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
        deliver(SIGNAL_INTERRUPT);
        return TRUE;
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        deliver(SIGNAL_TERMINATE);
        return TRUE;
    default:
        return FALSE;
    }
}

bool fl_termination_watch_start(Fl_TerminationFn fn, void *user_data)
{
    if (!fn || g_watcher.started) return false;
    g_watcher = (Watcher){ .fn = fn, .user_data = user_data };
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) return false;
    g_watcher.started = true;
    return true;
}

void fl_termination_watch_stop(void)
{
    if (!g_watcher.started) return;
    SetConsoleCtrlHandler(console_handler, FALSE);
    g_watcher.started = false;
}

#else

#include <pthread.h>
#include <signal.h>

/* Private wake-up used only to stop the watcher thread. */
#define STOP_SIGNAL SIGUSR2

static pthread_t        g_thread;
static sigset_t         g_watched;
static sigset_t         g_previous_mask;
static volatile uint32_t g_stopping;

static void *watch_main(void *arg)
{
    (void)arg;
    fl_thread_set_name("flow-signals");
    for (;;) {
        int sig = 0;
        if (sigwait(&g_watched, &sig) != 0) continue;
        if (sig == STOP_SIGNAL) {
            if (fl_atomic_load_u32(&g_stopping)) break;
            continue;
        }
        deliver(sig);
    }
    return NULL;
}

bool fl_termination_watch_start(Fl_TerminationFn fn, void *user_data)
{
    if (!fn || g_watcher.started) return false;
    g_watcher = (Watcher){ .fn = fn, .user_data = user_data };
    fl_atomic_store_u32(&g_stopping, 0u);

    sigemptyset(&g_watched);
    sigaddset(&g_watched, SIGINT);
    sigaddset(&g_watched, SIGTERM);
    sigaddset(&g_watched, SIGHUP);
    sigaddset(&g_watched, STOP_SIGNAL);
    if (pthread_sigmask(SIG_BLOCK, &g_watched, &g_previous_mask) != 0) return false;
    if (pthread_create(&g_thread, NULL, watch_main, NULL) != 0) {
        pthread_sigmask(SIG_SETMASK, &g_previous_mask, NULL);
        return false;
    }
    g_watcher.started = true;
    return true;
}

void fl_termination_watch_stop(void)
{
    if (!g_watcher.started) return;
    fl_atomic_store_u32(&g_stopping, 1u);
    pthread_kill(g_thread, STOP_SIGNAL);
    pthread_join(g_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &g_previous_mask, NULL);
    g_watcher.started = false;
}

#endif
