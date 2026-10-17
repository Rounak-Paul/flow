#pragma once

#include "fl_clock.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Process-wide logger.
 *
 * Every record goes to the registered sinks (observers). Console output is a
 * built-in sink configured by fl_log_init; file output (engine/platform/
 * fl_log_file.h), event-bus bridges and anything else attach with
 * fl_log_add_sink. All functions are thread-safe. Logging works before
 * fl_log_init and after fl_log_shutdown, falling back to stderr.
 *
 * Sinks run serialized, on the logging thread, while the logger lock is held:
 * they must be fast and must not add or remove sinks. A log call made from
 * inside a sink bypasses the sinks and writes straight to stderr.
 */

typedef enum Fl_LogLevel {
    FL_LOG_TRACE = 0,
    FL_LOG_DEBUG,
    FL_LOG_INFO,
    FL_LOG_WARN,
    FL_LOG_ERROR,
    FL_LOG_FATAL,
    FL_LOG_OFF,
} Fl_LogLevel;

/* One log record. Pointers are valid only for the duration of the sink call. */
typedef struct Fl_LogRecord {
    Fl_LogLevel level;
    const char *tag;            /* subsystem, e.g. "jobs"; never NULL        */
    const char *message;        /* formatted, NUL-terminated, no newline     */
    size_t      message_length;
    const char *file;           /* source file; never NULL                   */
    int         line;
    uint64_t    thread_id;
    const char *thread_name;    /* "" when unnamed                           */
    Fl_WallTime time;
    uint64_t    monotonic_ns;
    uint64_t    sequence;       /* process-wide, starting at 1               */
} Fl_LogRecord;

/*
 * Sink callback.
 *
 * record     Record to output.
 * user_data  Pointer supplied in Fl_LogSinkDesc.
 */
typedef void (*Fl_LogSinkFn)(const Fl_LogRecord *record, void *user_data);

/*
 * Optional sink flush callback, invoked by fl_log_flush and before a panic.
 *
 * user_data  Pointer supplied in Fl_LogSinkDesc.
 */
typedef void (*Fl_LogFlushFn)(void *user_data);

typedef struct Fl_LogSinkDesc {
    Fl_LogSinkFn  write;        /* required                                  */
    Fl_LogFlushFn flush;        /* optional                                  */
    void         *user_data;
    Fl_LogLevel   min_level;    /* per-sink threshold, combined with global  */
} Fl_LogSinkDesc;

/* Sink handle. Zero-initialised handles are always invalid. */
typedef struct Fl_LogSink {
    uint32_t id;
} Fl_LogSink;

typedef struct Fl_LogDesc {
    Fl_LogLevel level;          /* global threshold                          */
    bool        console;        /* stderr output, coloured on terminals      */
} Fl_LogDesc;

/* Bytes of recent formatted output retained in memory for crash reports. */
#define FL_LOG_RECENT_BYTES 65536u

/*
 * Configure the global level and the built-in console sink. Re-initialising
 * replaces the previous built-in configuration; other sinks are kept.
 *
 * Returns  false when the console sink could not be attached.
 */
bool fl_log_init(const Fl_LogDesc *desc);

/*
 * Flush every sink and remove all of them. Owners of sinks registered with
 * fl_log_add_sink must not rely on them after this call.
 */
void fl_log_shutdown(void);

/* Set the global threshold; records below it are discarded cheaply. */
void fl_log_set_level(Fl_LogLevel level);

/* Current global threshold. */
Fl_LogLevel fl_log_level(void);

/* Report whether a record at level would currently be emitted. */
bool fl_log_enabled(Fl_LogLevel level);

/*
 * Attach a sink receiving records at or above both its min_level and the
 * global level.
 *
 * Returns  Handle, or a zero handle on invalid arguments, allocation failure,
 *          or when called from inside a sink.
 */
Fl_LogSink fl_log_add_sink(const Fl_LogSinkDesc *desc);

/*
 * Detach a sink. Returns true when the handle was live.
 */
bool fl_log_remove_sink(Fl_LogSink sink);

#if defined(__GNUC__) || defined(__clang__)
#define FL_PRINTF_FORMAT(fmt_index, args_index) \
    __attribute__((format(printf, fmt_index, args_index)))
#else
#define FL_PRINTF_FORMAT(fmt_index, args_index)
#endif

/*
 * Emit a printf-formatted record. Prefer the FL_LOG_* macros.
 *
 * level  Severity; FL_LOG_OFF is ignored.
 * tag    Subsystem tag; NULL becomes "flow".
 * file   Source file; NULL becomes "?".
 * line   Source line.
 */
void fl_log_write(Fl_LogLevel level, const char *tag, const char *file, int line,
                  const char *fmt, ...) FL_PRINTF_FORMAT(5, 6);

/* va_list variant of fl_log_write. */
void fl_log_writev(Fl_LogLevel level, const char *tag, const char *file, int line,
                   const char *fmt, va_list args);

/* Invoke every sink's flush callback. */
void fl_log_flush(void);

/*
 * Copy the most recent formatted output, oldest first, without taking locks.
 * Intended for crash handlers: async-signal-safe, and may observe a
 * partially written final line.
 *
 * Returns  Bytes written to out (not NUL-terminated).
 */
size_t fl_log_copy_recent(char *out, size_t cap);

/* Upper-case level name, e.g. "WARN". */
const char *fl_log_level_name(Fl_LogLevel level);

/*
 * Format the canonical line prefix used by text sinks:
 * "YYYY-MM-DD HH:MM:SS.mmm LEVEL [thread] tag: ".
 *
 * Returns  snprintf semantics: characters that would have been written, or
 *          a negative value on error.
 */
int fl_log_format_prefix(const Fl_LogRecord *record, char *out, size_t cap);

#if defined(__GNUC__) || defined(__clang__)
#define FL_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define FL_NORETURN __declspec(noreturn)
#else
#define FL_NORETURN
#endif

/*
 * Log a fatal record, flush, and abort the process (the crash handler then
 * writes a report). Use for unrecoverable invariant violations.
 */
FL_NORETURN void fl_panic_at(const char *file, int line, const char *fmt, ...)
    FL_PRINTF_FORMAT(3, 4);

#define FL_LOG(level, tag, ...)                                           \
    do {                                                                  \
        if (fl_log_enabled(level))                                        \
            fl_log_write((level), (tag), __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#define FL_LOG_TRACE(tag, ...) FL_LOG(FL_LOG_TRACE, tag, __VA_ARGS__)
#define FL_LOG_DEBUG(tag, ...) FL_LOG(FL_LOG_DEBUG, tag, __VA_ARGS__)
#define FL_LOG_INFO(tag, ...)  FL_LOG(FL_LOG_INFO, tag, __VA_ARGS__)
#define FL_LOG_WARN(tag, ...)  FL_LOG(FL_LOG_WARN, tag, __VA_ARGS__)
#define FL_LOG_ERROR(tag, ...) FL_LOG(FL_LOG_ERROR, tag, __VA_ARGS__)
#define FL_LOG_FATAL(tag, ...) FL_LOG(FL_LOG_FATAL, tag, __VA_ARGS__)

#define FL_PANIC(...) fl_panic_at(__FILE__, __LINE__, __VA_ARGS__)

/* Always-on invariant check; failure panics with the expression text. */
#define FL_ASSERT(cond)                                                       \
    do {                                                                      \
        if (!(cond)) fl_panic_at(__FILE__, __LINE__, "assertion failed: %s", #cond); \
    } while (0)
