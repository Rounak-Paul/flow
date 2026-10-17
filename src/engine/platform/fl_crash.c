#include "fl_crash.h"

#include "core/fl_atomic.h"
#include "core/fl_log.h"
#include "core/fl_thread.h"
#include "fl_fs.h"
#include "fl_paths.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MARKER_NAME     "last-crash"
#define FIELD_MAX       64u
#define MAX_FRAMES      64

/* ---------------------------------------------------------------------------
   Async-signal-safe text building. Everything below writes into a fixed
   buffer that is flushed to every output target; no allocation, no stdio.
   --------------------------------------------------------------------------- */

typedef struct Writer {
    char   buffer[512];
    size_t used;
    void (*emit)(const char *data, size_t len);
} Writer;

static void writer_flush(Writer *w)
{
    if (w->used) w->emit(w->buffer, w->used);
    w->used = 0;
}

static void put_bytes(Writer *w, const char *data, size_t len)
{
    while (len) {
        size_t room = sizeof(w->buffer) - w->used;
        size_t chunk = len < room ? len : room;
        memcpy(w->buffer + w->used, data, chunk);
        w->used += chunk;
        data += chunk;
        len -= chunk;
        if (w->used == sizeof(w->buffer)) writer_flush(w);
    }
}

static void put_str(Writer *w, const char *s)
{
    if (s) put_bytes(w, s, strlen(s));
}

static void put_uint(Writer *w, uint64_t value, unsigned base, unsigned min_digits)
{
    char digits[32];
    unsigned n = 0;
    do {
        unsigned d = (unsigned)(value % base);
        digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= base;
    } while (value && n < sizeof(digits));
    while (n < min_digits && n < sizeof(digits)) digits[n++] = '0';
    char out[32];
    for (unsigned i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    put_bytes(w, out, n);
}

static void put_hex(Writer *w, uint64_t value)
{
    put_str(w, "0x");
    put_uint(w, value, 16, 1);
}

typedef struct CivilTime {
    int64_t  year;
    unsigned month, day, hour, minute, second;
} CivilTime;

/* UTC civil time from Unix seconds (Hinnant's days-from-civil inverse). */
static CivilTime civil_from_unix(int64_t seconds)
{
    int64_t days = seconds / 86400;
    int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    return (CivilTime){
        .year = (int64_t)yoe + era * 400 + (month <= 2),
        .month = month, .day = day,
        .hour = (unsigned)(rem / 3600), .minute = (unsigned)(rem % 3600 / 60),
        .second = (unsigned)(rem % 60),
    };
}

static void put_utc(Writer *w, const CivilTime *t)
{
    put_uint(w, (uint64_t)t->year, 10, 4);
    put_str(w, "-");
    put_uint(w, t->month, 10, 2);
    put_str(w, "-");
    put_uint(w, t->day, 10, 2);
    put_str(w, " ");
    put_uint(w, t->hour, 10, 2);
    put_str(w, ":");
    put_uint(w, t->minute, 10, 2);
    put_str(w, ":");
    put_uint(w, t->second, 10, 2);
    put_str(w, " UTC");
}

/* Compact timestamp for file names: YYYYMMDDTHHMMSSZ. */
static size_t format_stamp(char *out, size_t cap, const CivilTime *t)
{
    const uint64_t parts[] = { (uint64_t)t->year, t->month, t->day, t->hour, t->minute, t->second };
    const unsigned widths[] = { 4, 2, 2, 2, 2, 2 };
    size_t n = 0;
    for (size_t i = 0; i < 6; ++i) {
        if (i == 3 && n < cap) out[n++] = 'T';
        uint64_t v = parts[i];
        for (unsigned d = widths[i]; d > 0; --d) {
            uint64_t div = 1;
            for (unsigned k = 1; k < d; ++k) div *= 10;
            if (n < cap) out[n++] = (char)('0' + (v / div) % 10);
        }
    }
    if (n < cap) out[n++] = 'Z';
    return n;
}

typedef struct CrashState {
    bool              installed;
    char              dir[FL_PATH_MAX];
    char              marker[FL_PATH_MAX];
    char              app_name[FIELD_MAX];
    char              app_version[FIELD_MAX];
    volatile uint32_t crashing;
    char              report_path[FL_PATH_MAX];
    char              log_tail[FL_LOG_RECENT_BYTES];
} CrashState;

static CrashState g_crash;

static void copy_field(char *out, size_t cap, const char *value, const char *fallback)
{
    const char *src = value && value[0] ? value : fallback;
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1u;
    memcpy(out, src, len);
    out[len] = '\0';
}

static bool prepare_state(const Fl_CrashDesc *desc)
{
    if (!desc || !desc->report_dir || !desc->report_dir[0]) return false;
    if (!fl_fs_mkdir_p(desc->report_dir)) return false;
    if (strlen(desc->report_dir) >= sizeof(g_crash.dir)) return false;
    memcpy(g_crash.dir, desc->report_dir, strlen(desc->report_dir) + 1u);
    if (!fl_fs_join(g_crash.marker, sizeof(g_crash.marker), g_crash.dir, MARKER_NAME)) return false;
    copy_field(g_crash.app_name, sizeof(g_crash.app_name), desc->app_name, "Flow");
    copy_field(g_crash.app_version, sizeof(g_crash.app_version), desc->app_version, "unknown");
    g_crash.crashing = 0;
    return true;
}

/* Build "<dir>/crash-<stamp>-<pid>.<ext>" into out without allocating. */
static bool build_report_path(char *out, size_t cap, const CivilTime *t, uint64_t pid, const char *ext)
{
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t n = strlen(g_crash.dir);
    if (n + 64u >= cap) return false;
    memcpy(out, g_crash.dir, n);
    if (n && out[n - 1] != sep && out[n - 1] != '/') out[n++] = sep;
    memcpy(out + n, "crash-", 6);
    n += 6;
    n += format_stamp(out + n, cap - n, t);
    out[n++] = '-';
    char digits[24];
    unsigned d = 0;
    do {
        digits[d++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid && d < sizeof(digits));
    while (d) out[n++] = digits[--d];
    size_t ext_len = strlen(ext);
    if (n + ext_len + 1u >= cap) return false;
    memcpy(out + n, ext, ext_len + 1u);
    return true;
}

static void put_header(Writer *w, const CivilTime *t, uint64_t pid)
{
    put_str(w, "==== ");
    put_str(w, g_crash.app_name);
    put_str(w, " crash report ====\napp: ");
    put_str(w, g_crash.app_name);
    put_str(w, " ");
    put_str(w, g_crash.app_version);
    put_str(w, "\ntime: ");
    put_utc(w, t);
    put_str(w, "\npid: ");
    put_uint(w, pid, 10, 1);
    put_str(w, "\nthread: ");
    const char *name = fl_thread_current_name();
    put_str(w, name[0] ? name : "(unnamed)");
    put_str(w, " id ");
    put_hex(w, fl_thread_current_id());
    put_str(w, "\n");
}

static void put_log_tail(Writer *w)
{
    put_str(w, "\n---- recent log ----\n");
    size_t len = fl_log_copy_recent(g_crash.log_tail, sizeof(g_crash.log_tail));
    /* Start at a line boundary when the ring has wrapped mid-line. */
    size_t start = 0;
    if (len == sizeof(g_crash.log_tail)) {
        while (start < len && g_crash.log_tail[start] != '\n') ++start;
        if (start < len) ++start;
    }
    put_bytes(w, g_crash.log_tail + start, len - start);
    put_str(w, "---- end of report ----\n");
}

bool fl_crash_take_previous_report(const char *report_dir, char *out, size_t cap)
{
    if (!report_dir || !out || !cap) return false;
    out[0] = '\0';
    char marker[FL_PATH_MAX];
    if (!fl_fs_join(marker, sizeof(marker), report_dir, MARKER_NAME)) return false;
    FILE *file = fl_fs_fopen(marker, "rb");
    if (!file) return false;
    char path[FL_PATH_MAX];
    size_t len = fread(path, 1, sizeof(path) - 1u, file);
    fclose(file);
    fl_fs_remove(marker);
    while (len && (path[len - 1] == '\n' || path[len - 1] == '\r' || path[len - 1] == '\0')) --len;
    path[len] = '\0';
    if (!len || len >= cap || !fl_fs_exists(path)) return false;
    memcpy(out, path, len + 1u);
    return true;
}

#if defined(_WIN32)

/* ---------------------------------------------------------------------------
   Windows: unhandled-exception filter, abort and invalid-parameter handlers,
   minidump via dbghelp loaded at install.
   --------------------------------------------------------------------------- */

#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <stdlib.h>

typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

static LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter;
static _crt_signal_t g_previous_abort;
static _invalid_parameter_handler g_previous_invalid;
static MiniDumpWriteDumpFn g_minidump;
static HANDLE g_report_file = INVALID_HANDLE_VALUE;

static void write_handle(HANDLE handle, const char *data, size_t len)
{
    while (len && handle != INVALID_HANDLE_VALUE) {
        DWORD chunk = len > 0x10000000u ? 0x10000000u : (DWORD)len;
        DWORD written = 0;
        if (!WriteFile(handle, data, chunk, &written, NULL) || written == 0) return;
        data += written;
        len -= written;
    }
}

static void emit_all(const char *data, size_t len)
{
    write_handle(g_report_file, data, len);
    write_handle(GetStdHandle(STD_ERROR_HANDLE), data, len);
}

static HANDLE create_utf8_file(const char *path)
{
    wchar_t wide[FL_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, FL_PATH_MAX) <= 0) return INVALID_HANDLE_VALUE;
    return CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_BREAKPOINT:            return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION";
    default:                              return "EXCEPTION";
    }
}

static void put_backtrace(Writer *w)
{
    put_str(w, "\nbacktrace:\n");
    void *frames[MAX_FRAMES];
    USHORT count = CaptureStackBackTrace(0, MAX_FRAMES, frames, NULL);
    for (USHORT i = 0; i < count; ++i) {
        put_str(w, "  #");
        put_uint(w, i, 10, 2);
        put_str(w, " ");
        HMODULE module = NULL;
        char module_path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &module) && module &&
            GetModuleFileNameA(module, module_path, MAX_PATH)) {
            const char *base = strrchr(module_path, '\\');
            put_str(w, base ? base + 1 : module_path);
            put_str(w, "+");
            put_hex(w, (uint64_t)((uintptr_t)frames[i] - (uintptr_t)module));
        } else {
            put_hex(w, (uint64_t)(uintptr_t)frames[i]);
        }
        put_str(w, "\n");
    }
}

static void write_minidump(const CivilTime *t, uint64_t pid, EXCEPTION_POINTERS *pointers)
{
    if (!g_minidump) return;
    char path[FL_PATH_MAX];
    if (!build_report_path(path, sizeof(path), t, pid, ".dmp")) return;
    HANDLE file = create_utf8_file(path);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info = {
        .ThreadId = GetCurrentThreadId(), .ExceptionPointers = pointers, .ClientPointers = FALSE,
    };
    g_minidump(GetCurrentProcess(), GetCurrentProcessId(), file,
               (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
               pointers ? &info : NULL, NULL, NULL);
    CloseHandle(file);
}

/* Write the full report once per process; returns false if another thread already is. */
static bool report_crash(const char *kind, DWORD code, uint64_t address, EXCEPTION_POINTERS *pointers)
{
    uint32_t expected = 0;
    if (!fl_atomic_cas_u32(&g_crash.crashing, &expected, 1u)) return false;

    SYSTEMTIME st;
    GetSystemTime(&st);
    const CivilTime t = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond };
    const uint64_t pid = GetCurrentProcessId();

    if (build_report_path(g_crash.report_path, sizeof(g_crash.report_path), &t, pid, ".txt"))
        g_report_file = create_utf8_file(g_crash.report_path);

    Writer w = { .emit = emit_all };
    put_header(&w, &t, pid);
    put_str(&w, "fault: ");
    put_str(&w, kind);
    put_str(&w, " code ");
    put_hex(&w, code);
    put_str(&w, " address ");
    put_hex(&w, address);
    put_str(&w, "\n");
    put_backtrace(&w);
    put_log_tail(&w);
    writer_flush(&w);

    if (g_report_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_report_file);
        CloseHandle(g_report_file);
        g_report_file = INVALID_HANDLE_VALUE;
        HANDLE marker = create_utf8_file(g_crash.marker);
        write_handle(marker, g_crash.report_path, strlen(g_crash.report_path));
        if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
    }
    write_minidump(&t, pid, pointers);
    return true;
}

static LONG WINAPI exception_filter(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers ? pointers->ExceptionRecord : NULL;
    const DWORD code = record ? record->ExceptionCode : 0;
    uint64_t address = record ? (uint64_t)(uintptr_t)record->ExceptionAddress : 0;
    if (record && code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        address = (uint64_t)record->ExceptionInformation[1];
    report_crash(exception_name(code), code, address, pointers);
    return g_previous_filter ? g_previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

static void abort_handler(int sig)
{
    report_crash("SIGABRT", (DWORD)sig, 0, NULL);
    signal(SIGABRT, SIG_DFL);
}

static void invalid_parameter_handler(const wchar_t *expression, const wchar_t *function,
                                      const wchar_t *file, unsigned int line, uintptr_t reserved)
{
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
    report_crash("CRT_INVALID_PARAMETER", 0, 0, NULL);
    abort();
}

bool fl_crash_thread_attach(void)
{
    ULONG guarantee = 128u * 1024u;
    return SetThreadStackGuarantee(&guarantee) != 0;
}

void fl_crash_thread_detach(void)
{
}

bool fl_crash_install(const Fl_CrashDesc *desc)
{
    if (g_crash.installed || !prepare_state(desc)) return false;
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    g_minidump = dbghelp ? (MiniDumpWriteDumpFn)(void (*)(void))GetProcAddress(dbghelp, "MiniDumpWriteDump")
                         : NULL;
    g_previous_filter = SetUnhandledExceptionFilter(exception_filter);
    g_previous_abort = signal(SIGABRT, abort_handler);
    g_previous_invalid = _set_invalid_parameter_handler(invalid_parameter_handler);
    fl_crash_thread_attach();
    g_crash.installed = true;
    return true;
}

void fl_crash_uninstall(void)
{
    if (!g_crash.installed) return;
    SetUnhandledExceptionFilter(g_previous_filter);
    signal(SIGABRT, g_previous_abort ? g_previous_abort : SIG_DFL);
    _set_invalid_parameter_handler(g_previous_invalid);
    g_crash.installed = false;
}

#else

/* ---------------------------------------------------------------------------
   POSIX: sigaction handlers on an alternate stack, backtrace(3).
   --------------------------------------------------------------------------- */

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#define ALT_STACK_BYTES (256u * 1024u)

static const int k_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
#define SIGNAL_COUNT (sizeof(k_signals) / sizeof(k_signals[0]))

static struct sigaction g_previous[SIGNAL_COUNT];
static int g_report_fd = -1;
static _Thread_local void *t_alt_stack;
static _Thread_local volatile sig_atomic_t t_in_handler;

static void write_fd(int fd, const char *data, size_t len)
{
    while (len && fd >= 0) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) return;
        data += n;
        len -= (size_t)n;
    }
}

static void emit_all(const char *data, size_t len)
{
    write_fd(g_report_fd, data, len);
    write_fd(STDERR_FILENO, data, len);
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    default:      return "SIGNAL";
    }
}

/* Program counter of the faulting instruction, or NULL when unavailable. */
static void *fault_pc(const void *context)
{
    const ucontext_t *uc = context;
    if (!uc) return NULL;
#if defined(__APPLE__) && defined(__aarch64__)
    return (void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return (void *)(uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__x86_64__)
    return (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    return (void *)(uintptr_t)uc->uc_mcontext.pc;
#else
    return NULL;
#endif
}

static void put_faulting_frame(Writer *w, const void *context)
{
    void *pc = fault_pc(context);
    if (!pc) return;
    put_str(w, "\nfaulting frame:\n");
    writer_flush(w);
    void *frames[1] = { pc };
    if (g_report_fd >= 0) backtrace_symbols_fd(frames, 1, g_report_fd);
    backtrace_symbols_fd(frames, 1, STDERR_FILENO);
}

static void put_backtrace(Writer *w)
{
    put_str(w, "\nbacktrace:\n");
    writer_flush(w);
    void *frames[MAX_FRAMES];
    int count = backtrace(frames, MAX_FRAMES);
    if (g_report_fd >= 0) backtrace_symbols_fd(frames, count, g_report_fd);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

static void restore_default_and_reraise(int sig)
{
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

static void crash_handler(int sig, siginfo_t *info, void *context)
{
    if (t_in_handler) {
        restore_default_and_reraise(sig);
        return;
    }
    t_in_handler = 1;

    uint32_t expected = 0;
    if (!fl_atomic_cas_u32(&g_crash.crashing, &expected, 1u)) {
        /* Another thread is writing the report and will terminate the process. */
        for (;;) pause();
    }

    const CivilTime t = civil_from_unix((int64_t)time(NULL));
    const uint64_t pid = (uint64_t)getpid();
    if (build_report_path(g_crash.report_path, sizeof(g_crash.report_path), &t, pid, ".txt"))
        g_report_fd = open(g_crash.report_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    Writer w = { .emit = emit_all };
    put_header(&w, &t, pid);
    put_str(&w, "fault: ");
    put_str(&w, signal_name(sig));
    put_str(&w, " (");
    put_uint(&w, (uint64_t)sig, 10, 1);
    put_str(&w, ") code ");
    put_uint(&w, info ? (uint64_t)(uint32_t)info->si_code : 0u, 10, 1);
    put_str(&w, " address ");
    put_hex(&w, info ? (uint64_t)(uintptr_t)info->si_addr : 0u);
    put_str(&w, "\n");
    if (sig != SIGABRT) put_faulting_frame(&w, context);
    put_backtrace(&w);
    put_log_tail(&w);
    writer_flush(&w);

    if (g_report_fd >= 0) {
        fsync(g_report_fd);
        close(g_report_fd);
        g_report_fd = -1;
        int marker = open(g_crash.marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (marker >= 0) {
            write_fd(marker, g_crash.report_path, strlen(g_crash.report_path));
            fsync(marker);
            close(marker);
        }
    }
    restore_default_and_reraise(sig);
}

bool fl_crash_thread_attach(void)
{
    if (t_alt_stack) return true;
    void *stack = malloc(ALT_STACK_BYTES);
    if (!stack) return false;
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = stack;
    ss.ss_size = ALT_STACK_BYTES;
    if (sigaltstack(&ss, NULL) != 0) {
        free(stack);
        return false;
    }
    t_alt_stack = stack;
    return true;
}

void fl_crash_thread_detach(void)
{
    if (!t_alt_stack) return;
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
    free(t_alt_stack);
    t_alt_stack = NULL;
}

bool fl_crash_install(const Fl_CrashDesc *desc)
{
    if (g_crash.installed || !prepare_state(desc)) return false;

    /* backtrace() may lazily load its unwinder (and allocate) on first use;
       do that now rather than inside the signal handler. */
    void *warmup[4];
    (void)backtrace(warmup, 4);

    if (!fl_crash_thread_attach()) return false;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        if (sigaction(k_signals[i], &action, &g_previous[i]) != 0) {
            for (size_t j = 0; j < i; ++j) sigaction(k_signals[j], &g_previous[j], NULL);
            fl_crash_thread_detach();
            return false;
        }
    }
    g_crash.installed = true;
    return true;
}

void fl_crash_uninstall(void)
{
    if (!g_crash.installed) return;
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) sigaction(k_signals[i], &g_previous[i], NULL);
    fl_crash_thread_detach();
    g_crash.installed = false;
}

#endif
