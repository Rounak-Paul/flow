#include "core/fl_atomic.h"
#include "core/fl_thread.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_signal.h"

#include "flow_test.h"

#include <string.h>

static void test_join(void)
{
    char out[16];
    CHECK(fl_fs_join(out, sizeof(out), "a", "b"));
#if defined(_WIN32)
    CHECK(strcmp(out, "a\\b") == 0);
#else
    CHECK(strcmp(out, "a/b") == 0);
#endif
    CHECK(fl_fs_join(out, sizeof(out), "a/", "b"));
    CHECK(strcmp(out, "a/b") == 0);
    CHECK(!fl_fs_join(out, sizeof(out), "0123456789", "abcdef"));
    CHECK(out[0] == '\0');
    CHECK(!fl_fs_join(out, sizeof(out), NULL, "b"));
}

static void test_files(const char *scratch)
{
    char dir[FL_PATH_MAX], nested[FL_PATH_MAX], file[FL_PATH_MAX], moved[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), scratch, "fs"));
    CHECK(fl_fs_join(nested, sizeof(nested), dir, "a/b/c"));
    CHECK(fl_fs_mkdir_p(nested));
    CHECK(fl_fs_mkdir_p(nested));
    CHECK(fl_fs_exists(nested));
    CHECK(fl_fs_file_size(nested) == -1);

    CHECK(fl_fs_join(file, sizeof(file), nested, "data.bin"));
    CHECK(fl_fs_join(moved, sizeof(moved), nested, "moved.bin"));
    fl_fs_remove(moved);
    FILE *f = fl_fs_fopen(file, "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite("hello", 1, 5, f);
        fclose(f);
    }
    CHECK(fl_fs_file_size(file) == 5);
    CHECK(fl_fs_rename(file, moved));
    CHECK(!fl_fs_exists(file));
    CHECK(fl_fs_file_size(moved) == 5);
    CHECK(fl_fs_remove(moved));
    CHECK(fl_fs_remove(moved));
    CHECK(fl_fs_file_size(moved) == -1);
    CHECK(!fl_fs_mkdir_p(""));
}

static void test_paths(void)
{
    char dir[FL_PATH_MAX];
    CHECK(fl_paths_executable_dir(dir, sizeof(dir)));
    CHECK(dir[0] != '\0');
    CHECK(fl_fs_exists(dir));
    char tiny[2];
    CHECK(!fl_paths_executable_dir(tiny, sizeof(tiny)));
    CHECK(!fl_paths_is_portable());
    CHECK(!fl_paths_user_dir(FL_PATH_KIND_COUNT, dir, sizeof(dir)));
}

static void test_thread_names(void)
{
    fl_thread_set_name("a-very-long-thread-name-that-exceeds-the-limit-by-far");
    CHECK(strlen(fl_thread_current_name()) == FL_THREAD_NAME_MAX);
    fl_thread_set_name("short");
    CHECK(strcmp(fl_thread_current_name(), "short") == 0);
    fl_thread_set_name(NULL);
    CHECK(fl_thread_current_name()[0] == '\0');
    CHECK(fl_thread_hardware_concurrency() >= 1u);
}

#if !defined(_WIN32)

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Requests {
    Fl_Mutex   lock;
    Fl_CondVar cond;
    int        signal_number;
    int        count;
} Requests;

static void on_request(int signal_number, void *user_data)
{
    Requests *r = user_data;
    fl_mutex_lock(&r->lock);
    r->signal_number = signal_number;
    r->count++;
    fl_cond_broadcast(&r->cond);
    fl_mutex_unlock(&r->lock);
}

static volatile uint32_t g_first_request_seen;

static void note_request(int signal_number, void *user_data)
{
    (void)signal_number;
    (void)user_data;
    fl_atomic_store_u32(&g_first_request_seen, 1u);
}

static void test_termination_watcher(void)
{
    Requests r = {0};
    fl_mutex_init(&r.lock);
    fl_cond_init(&r.cond);
    CHECK(!fl_termination_watch_start(NULL, NULL));
    CHECK(fl_termination_watch_start(on_request, &r));
    CHECK(!fl_termination_watch_start(on_request, &r));

    kill(getpid(), SIGTERM);
    fl_mutex_lock(&r.lock);
    while (r.count == 0) fl_cond_wait(&r.cond, &r.lock);
    fl_mutex_unlock(&r.lock);
    CHECK(r.signal_number == SIGTERM);
    CHECK(r.count == 1);
    fl_termination_watch_stop();
    fl_termination_watch_stop();

    pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        if (!fl_termination_watch_start(note_request, NULL)) _exit(90);
        kill(getpid(), SIGINT);
        /* Standard signals do not queue: send the second only after the
           first has been consumed, or both may coalesce into one. */
        while (!fl_atomic_load_u32(&g_first_request_seen)) {
        }
        kill(getpid(), SIGINT);
        for (;;) pause();
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGINT);
    fl_cond_destroy(&r.cond);
    fl_mutex_destroy(&r.lock);
}

#endif

int main(int argc, char **argv)
{
    const char *scratch = argc > 1 ? argv[1] : ".";
    CHECK(fl_fs_mkdir_p(scratch));
    test_join();
    test_files(scratch);
    test_paths();
    test_thread_names();
#if !defined(_WIN32)
    test_termination_watcher();
#endif
    return FLOW_TEST_RESULT("fl_platform_tests");
}
