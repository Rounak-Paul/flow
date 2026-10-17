#include "core/fl_job.h"
#include "core/fl_log.h"
#include "core/fl_thread.h"
#include "platform/fl_crash.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"

#include "flow_test.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

int main(void)
{
    printf("fl_crash_tests: skipped on Windows (fork-based harness)\n");
    return 0;
}

#else

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void (*CrashFn)(void);

static volatile int *volatile g_null;

static void crash_null_write(void)
{
    *g_null = 1;
}

static int recurse(volatile int depth)
{
    if (g_null == (volatile int *)1) return 0;
    volatile char pad[1024];
    pad[0] = (char)depth;
    return recurse(depth + 1) + pad[0];
}

static void crash_stack_overflow(void)
{
    (void)recurse(0);
}

static void crash_abort(void)
{
    abort();
}

static void crash_assert(void)
{
    int value = 3;
    FL_ASSERT(value == 4);
}

static void crash_in_job_body(void *data)
{
    (void)data;
    *g_null = 2;
}

static void attach_worker(uint32_t index, void *data)
{
    (void)index;
    (void)data;
    fl_crash_thread_attach();
}

static void crash_on_worker(void)
{
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){ .worker_count = 1, .on_thread_start = attach_worker });
    fl_job_submit(jobs, &(Fl_JobDesc){ .fn = crash_in_job_body });
    for (;;) pause();
}

/* Run crash_fn in a child with handlers installed; returns the terminating signal or -1. */
static int run_crashing_child(const char *dir, CrashFn crash_fn)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        alarm(20);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        fl_thread_set_name("crash-child");
        fl_log_init(&(Fl_LogDesc){ .level = FL_LOG_INFO, .console = false });
        if (!fl_crash_install(&(Fl_CrashDesc){ .report_dir = dir, .app_name = "FlowTest", .app_version = "9.9.9" }))
            _exit(90);
        FL_LOG_INFO("test", "breadcrumb-before-crash");
        crash_fn();
        _exit(91);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFSIGNALED(status) ? WTERMSIG(status) : -100 - (WIFEXITED(status) ? WEXITSTATUS(status) : 0);
}

static void read_report(const char *path, char *out, size_t cap)
{
    out[0] = '\0';
    FILE *file = fl_fs_fopen(path, "rb");
    if (!file) return;
    size_t n = fread(out, 1, cap - 1, file);
    fclose(file);
    out[n] = '\0';
}

/* Crash in a child and verify the report. alt_signal (or 0) is an accepted
   alternative terminating signal where platforms differ. */
static void expect_report(const char *dir, CrashFn crash_fn, int expected_signal, int alt_signal,
                          const char *expected_fault, const char *expected_thread,
                          const char *expected_text)
{
    char ignored[FL_PATH_MAX];
    fl_crash_take_previous_report(dir, ignored, sizeof(ignored));

    int sig = run_crashing_child(dir, crash_fn);
    const bool signal_ok = sig == expected_signal || (alt_signal && sig == alt_signal);
    CHECK(signal_ok);

    char path[FL_PATH_MAX];
    CHECK(fl_crash_take_previous_report(dir, path, sizeof(path)));
    CHECK(!fl_crash_take_previous_report(dir, ignored, sizeof(ignored)));

    static char report[1 << 18];
    read_report(path, report, sizeof(report));
    CHECK(strstr(report, "FlowTest crash report") != NULL);
    CHECK(strstr(report, "app: FlowTest 9.9.9") != NULL);
    CHECK(strstr(report, expected_fault) != NULL);
    CHECK(strstr(report, expected_thread) != NULL);
    CHECK(strstr(report, "backtrace:") != NULL);
    CHECK(strstr(report, "breadcrumb-before-crash") != NULL);
    CHECK(strstr(report, "---- end of report ----") != NULL);
    if (expected_text) CHECK(strstr(report, expected_text) != NULL);
    if (!signal_ok) fprintf(stderr, "  child ended with %d for %s\n", sig, expected_fault);
}

static void test_install_validation(const char *dir)
{
    CHECK(!fl_crash_install(NULL));
    CHECK(!fl_crash_install(&(Fl_CrashDesc){0}));
    CHECK(fl_crash_install(&(Fl_CrashDesc){ .report_dir = dir }));
    CHECK(!fl_crash_install(&(Fl_CrashDesc){ .report_dir = dir }));
    fl_crash_uninstall();
    fl_crash_uninstall();
    char path[FL_PATH_MAX];
    CHECK(!fl_crash_take_previous_report(NULL, path, sizeof(path)));
}

int main(int argc, char **argv)
{
    const char *scratch = argc > 1 ? argv[1] : ".";
    char dir[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), scratch, "crashes"));
    CHECK(fl_fs_mkdir_p(dir));

    test_install_validation(dir);
    expect_report(dir, crash_null_write, SIGSEGV, SIGBUS, "fault: SIG", "thread: crash-child",
                  "crash_null_write");
    expect_report(dir, crash_stack_overflow, SIGSEGV, SIGBUS, "fault: SIG", "thread: crash-child", NULL);
    expect_report(dir, crash_abort, SIGABRT, 0, "fault: SIGABRT", "thread: crash-child", NULL);
    expect_report(dir, crash_assert, SIGABRT, 0, "fault: SIGABRT", "thread: crash-child",
                  "assertion failed: value == 4");
    expect_report(dir, crash_on_worker, SIGSEGV, SIGBUS, "fault: SIG", "thread: flow-job-0", NULL);
    return FLOW_TEST_RESULT("fl_crash_tests");
}

#endif
