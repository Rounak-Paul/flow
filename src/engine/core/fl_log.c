#include "fl_log.h"

#include "fl_atomic.h"
#include "fl_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MESSAGE_STACK_BYTES 2048u
#define HEADER_BYTES        192u

typedef struct SinkEntry {
    uint32_t       id;
    Fl_LogSinkDesc desc;
} SinkEntry;

typedef struct LogState {
    Fl_Mutex          lock;
    volatile uint32_t level;
    SinkEntry        *sinks;
    uint32_t          sink_count;
    uint32_t          sink_cap;
    uint32_t          next_id;
    uint32_t          console_id;
    bool              console_color;
    uint64_t          sequence;
    volatile uint64_t ring_head;      /* total bytes ever written to ring */
    char              ring[FL_LOG_RECENT_BYTES];
} LogState;

static LogState g_log = {
    .lock  = FL_MUTEX_INITIALIZER,
    .level = FL_LOG_INFO,
};

static _Thread_local bool t_in_sink;

static const char *const k_level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF",
};

static const char *const k_level_colors[] = {
    "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[1;41;97m", "",
};

const char *fl_log_level_name(Fl_LogLevel level)
{
    return (unsigned)level <= (unsigned)FL_LOG_OFF ? k_level_names[level] : "?";
}

void fl_log_set_level(Fl_LogLevel level)
{
    if ((unsigned)level > (unsigned)FL_LOG_OFF) level = FL_LOG_OFF;
    fl_atomic_store_u32(&g_log.level, (uint32_t)level);
}

Fl_LogLevel fl_log_level(void)
{
    return (Fl_LogLevel)fl_atomic_load_u32(&g_log.level);
}

bool fl_log_enabled(Fl_LogLevel level)
{
    return (unsigned)level < (unsigned)FL_LOG_OFF &&
           (uint32_t)level >= fl_atomic_load_u32(&g_log.level);
}

/* Append bytes to the crash-report ring; caller holds the lock. */
static void ring_append(const char *data, size_t len)
{
    uint64_t head = fl_atomic_load_u64(&g_log.ring_head);
    if (len > FL_LOG_RECENT_BYTES) {
        data += len - FL_LOG_RECENT_BYTES;
        head += len - FL_LOG_RECENT_BYTES;
        len = FL_LOG_RECENT_BYTES;
    }
    size_t at = (size_t)(head % FL_LOG_RECENT_BYTES);
    size_t first = FL_LOG_RECENT_BYTES - at;
    if (first > len) first = len;
    memcpy(g_log.ring + at, data, first);
    memcpy(g_log.ring, data + first, len - first);
    fl_atomic_store_u64(&g_log.ring_head, head + len);
}

size_t fl_log_copy_recent(char *out, size_t cap)
{
    if (!out || !cap) return 0;
    const uint64_t head = fl_atomic_load_u64(&g_log.ring_head);
    size_t available = head < FL_LOG_RECENT_BYTES ? (size_t)head : FL_LOG_RECENT_BYTES;
    size_t len = available < cap ? available : cap;
    size_t start = (size_t)((head - len) % FL_LOG_RECENT_BYTES);
    size_t first = FL_LOG_RECENT_BYTES - start;
    if (first > len) first = len;
    memcpy(out, g_log.ring + start, first);
    memcpy(out + first, g_log.ring, len - first);
    return len;
}

int fl_log_format_prefix(const Fl_LogRecord *record, char *out, size_t cap)
{
    const Fl_WallTime *t = &record->time;
    if (record->thread_name[0])
        return snprintf(out, cap, "%04d-%02u-%02u %02u:%02u:%02u.%03u %-5s [%s] %s: ",
                        t->year, t->month, t->day, t->hour, t->minute, t->second,
                        t->millisecond, fl_log_level_name(record->level),
                        record->thread_name, record->tag);
    return snprintf(out, cap, "%04d-%02u-%02u %02u:%02u:%02u.%03u %-5s [%llx] %s: ",
                    t->year, t->month, t->day, t->hour, t->minute, t->second,
                    t->millisecond, fl_log_level_name(record->level),
                    (unsigned long long)record->thread_id, record->tag);
}

static int format_location(char *out, size_t cap, const Fl_LogRecord *record)
{
    out[0] = '\0';
    if (record->level < FL_LOG_WARN) return 0;
    return snprintf(out, cap, " (%s:%d)", record->file, record->line);
}

static void write_plain(FILE *stream, const Fl_LogRecord *record)
{
    char header[HEADER_BYTES], location[HEADER_BYTES];
    fl_log_format_prefix(record, header, sizeof(header));
    format_location(location, sizeof(location), record);
    fprintf(stream, "%s%s%s\n", header, record->message, location);
}

/* Built-in console sink. */
static void console_write(const Fl_LogRecord *record, void *user_data)
{
    (void)user_data;
    if (!g_log.console_color) {
        write_plain(stderr, record);
        return;
    }
    const Fl_WallTime *t = &record->time;
    char location[HEADER_BYTES];
    format_location(location, sizeof(location), record);
    fprintf(stderr, "\x1b[90m%02u:%02u:%02u.%03u\x1b[0m %s%-5s\x1b[0m \x1b[90m[%s]\x1b[0m \x1b[1m%s\x1b[0m: %s\x1b[90m%s\x1b[0m\n",
            t->hour, t->minute, t->second, t->millisecond,
            k_level_colors[record->level], fl_log_level_name(record->level),
            record->thread_name[0] ? record->thread_name : "-", record->tag,
            record->message, location);
}

static void console_flush(void *user_data)
{
    (void)user_data;
    fflush(stderr);
}

static bool stderr_supports_color(void)
{
#if defined(_WIN32)
    if (!_isatty(_fileno(stderr))) return false;
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) return false;
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    const char *term = getenv("TERM");
    return isatty(fileno(stderr)) && !(term && strcmp(term, "dumb") == 0) && !getenv("NO_COLOR");
#endif
}

static uint32_t add_sink_locked(const Fl_LogSinkDesc *desc)
{
    if (g_log.sink_count == g_log.sink_cap) {
        uint32_t cap = g_log.sink_cap ? g_log.sink_cap * 2u : 8u;
        if (cap < g_log.sink_cap) return 0;
        SinkEntry *sinks = realloc(g_log.sinks, (size_t)cap * sizeof(SinkEntry));
        if (!sinks) return 0;
        g_log.sinks = sinks;
        g_log.sink_cap = cap;
    }
    uint32_t id = ++g_log.next_id;
    if (id == 0) id = ++g_log.next_id;
    g_log.sinks[g_log.sink_count++] = (SinkEntry){ .id = id, .desc = *desc };
    return id;
}

static bool remove_sink_locked(uint32_t id)
{
    for (uint32_t i = 0; i < g_log.sink_count; ++i) {
        if (g_log.sinks[i].id != id) continue;
        memmove(&g_log.sinks[i], &g_log.sinks[i + 1u],
                (size_t)(g_log.sink_count - i - 1u) * sizeof(SinkEntry));
        g_log.sink_count--;
        return true;
    }
    return false;
}

bool fl_log_init(const Fl_LogDesc *desc)
{
    const Fl_LogDesc defaults = { .level = FL_LOG_INFO, .console = true };
    if (!desc) desc = &defaults;
    fl_log_set_level(desc->level);
    if (t_in_sink) return false;

    bool ok = true;
    fl_mutex_lock(&g_log.lock);
    if (desc->console && !g_log.console_id) {
        g_log.console_color = stderr_supports_color();
        g_log.console_id = add_sink_locked(&(Fl_LogSinkDesc){
            .write = console_write, .flush = console_flush, .min_level = FL_LOG_TRACE,
        });
        ok = g_log.console_id != 0;
    } else if (!desc->console && g_log.console_id) {
        remove_sink_locked(g_log.console_id);
        g_log.console_id = 0;
    }
    fl_mutex_unlock(&g_log.lock);
    return ok;
}

static void flush_locked(void)
{
    t_in_sink = true;
    for (uint32_t i = 0; i < g_log.sink_count; ++i)
        if (g_log.sinks[i].desc.flush) g_log.sinks[i].desc.flush(g_log.sinks[i].desc.user_data);
    t_in_sink = false;
    fflush(stderr);
}

void fl_log_shutdown(void)
{
    if (t_in_sink) return;
    fl_mutex_lock(&g_log.lock);
    flush_locked();
    free(g_log.sinks);
    g_log.sinks = NULL;
    g_log.sink_count = g_log.sink_cap = 0;
    g_log.console_id = 0;
    fl_mutex_unlock(&g_log.lock);
}

Fl_LogSink fl_log_add_sink(const Fl_LogSinkDesc *desc)
{
    if (!desc || !desc->write || t_in_sink) return (Fl_LogSink){0};
    fl_mutex_lock(&g_log.lock);
    uint32_t id = add_sink_locked(desc);
    fl_mutex_unlock(&g_log.lock);
    return (Fl_LogSink){ .id = id };
}

bool fl_log_remove_sink(Fl_LogSink sink)
{
    if (!sink.id || t_in_sink) return false;
    fl_mutex_lock(&g_log.lock);
    bool removed = remove_sink_locked(sink.id);
    if (removed && sink.id == g_log.console_id) g_log.console_id = 0;
    fl_mutex_unlock(&g_log.lock);
    return removed;
}

void fl_log_flush(void)
{
    if (t_in_sink) {
        fflush(stderr);
        return;
    }
    fl_mutex_lock(&g_log.lock);
    flush_locked();
    fl_mutex_unlock(&g_log.lock);
}

static void emit(Fl_LogLevel level, const char *tag, const char *file, int line,
                 const char *fmt, va_list args)
{
    char stack[MESSAGE_STACK_BYTES];
    char *heap = NULL;
    const char *message = stack;
    size_t length = 0;

    va_list measure;
    va_copy(measure, args);
    int needed = fmt ? vsnprintf(stack, sizeof(stack), fmt, measure) : -1;
    va_end(measure);
    if (needed < 0) {
        message = "<invalid log format>";
        length = strlen(message);
    } else if ((size_t)needed < sizeof(stack)) {
        length = (size_t)needed;
    } else if ((heap = malloc((size_t)needed + 1u)) != NULL) {
        vsnprintf(heap, (size_t)needed + 1u, fmt, args);
        message = heap;
        length = (size_t)needed;
    } else {
        length = sizeof(stack) - 1u;
    }
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        length--;
    if (message == stack || message == heap) ((char *)message)[length] = '\0';

    Fl_LogRecord record = {
        .level          = level,
        .tag            = tag ? tag : "flow",
        .message        = message,
        .message_length = length,
        .file           = file ? file : "?",
        .line           = line,
        .thread_id      = fl_thread_current_id(),
        .thread_name    = fl_thread_current_name(),
        .time           = fl_clock_wall_now(),
        .monotonic_ns   = fl_clock_monotonic_ns(),
    };

    if (t_in_sink) {
        write_plain(stderr, &record);
        free(heap);
        return;
    }

    char header[HEADER_BYTES], location[HEADER_BYTES];
    int header_len = fl_log_format_prefix(&record, header, sizeof(header));
    int location_len = format_location(location, sizeof(location), &record);

    fl_mutex_lock(&g_log.lock);
    record.sequence = ++g_log.sequence;
    if (header_len > 0) ring_append(header, (size_t)header_len < sizeof(header) ? (size_t)header_len : sizeof(header) - 1u);
    ring_append(message, length);
    if (location_len > 0) ring_append(location, (size_t)location_len < sizeof(location) ? (size_t)location_len : sizeof(location) - 1u);
    ring_append("\n", 1u);

    if (g_log.sink_count == 0) {
        write_plain(stderr, &record);
    } else {
        t_in_sink = true;
        for (uint32_t i = 0; i < g_log.sink_count; ++i) {
            const SinkEntry *entry = &g_log.sinks[i];
            if (level >= entry->desc.min_level) entry->desc.write(&record, entry->desc.user_data);
        }
        t_in_sink = false;
    }
    if (level >= FL_LOG_ERROR) flush_locked();
    fl_mutex_unlock(&g_log.lock);
    free(heap);
}

void fl_log_writev(Fl_LogLevel level, const char *tag, const char *file, int line,
                   const char *fmt, va_list args)
{
    if (!fl_log_enabled(level)) return;
    emit(level, tag, file, line, fmt, args);
}

void fl_log_write(Fl_LogLevel level, const char *tag, const char *file, int line,
                  const char *fmt, ...)
{
    if (!fl_log_enabled(level)) return;
    va_list args;
    va_start(args, fmt);
    emit(level, tag, file, line, fmt, args);
    va_end(args);
}

void fl_panic_at(const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    emit(FL_LOG_FATAL, "panic", file, line, fmt, args);
    va_end(args);
    fl_log_flush();
    abort();
}
