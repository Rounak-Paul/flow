#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Crash capture.
 *
 * On a fatal fault (segmentation fault, bus error, illegal instruction,
 * arithmetic exception, abort/panic, trap; unhandled SEH exceptions on
 * Windows) the handler writes a plain-text report to the report directory and
 * stderr: application and version, UTC time, process and thread, fault kind
 * and address, a backtrace, and the logger's recent output. On Windows a
 * minidump (.dmp) is written alongside. The handler then lets the default
 * action run, so the OS crash reporter and core dumps still work.
 *
 * The report path is also recorded in a marker file so the next launch can
 * surface it (fl_crash_take_previous_report).
 *
 * Every thread that may crash should call fl_crash_thread_attach so stack
 * overflows are reported too (POSIX: alternate signal stack; Windows: stack
 * guarantee for the handler). fl_crash_install attaches the calling thread.
 */

typedef struct Fl_CrashDesc {
    const char *report_dir;    /* required; created if missing   */
    const char *app_name;      /* default "Flow"                 */
    const char *app_version;   /* default "unknown"              */
} Fl_CrashDesc;

/*
 * Install process-wide crash handlers. Call once, early, on the main thread.
 *
 * Returns  false on invalid arguments, an unusable report directory, or when
 *          already installed.
 */
bool fl_crash_install(const Fl_CrashDesc *desc);

/*
 * Restore the handlers that were active before fl_crash_install and detach
 * the calling thread. No-op when not installed.
 */
void fl_crash_uninstall(void);

/*
 * Prepare the calling thread for crash reporting, including stack overflow.
 *
 * Returns  false when per-thread resources cannot be allocated.
 */
bool fl_crash_thread_attach(void);

/*
 * Release the calling thread's crash-reporting resources. Call before a
 * thread that attached exits.
 */
void fl_crash_thread_detach(void);

/*
 * Retrieve and clear the report left by a crash in a previous run.
 *
 * report_dir  Directory passed to fl_crash_install (may be called before it).
 * out         Receives the report path.
 * Returns     true when a previous report exists and its path fits in cap.
 */
bool fl_crash_take_previous_report(const char *report_dir, char *out, size_t cap);
