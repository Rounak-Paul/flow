#include "core/fl_log.h"
#include "core/fl_thread.h"
#include "platform/fl_fs.h"
#include "platform/fl_log_file.h"
#include "platform/fl_paths.h"

#include "flow_test.h"

#include <stdlib.h>
#include <string.h>

typedef struct Capture {
    Fl_Mutex lock;
    int      count;
    int      levels[FL_LOG_OFF];
    uint64_t last_sequence;
    bool     sequences_increasing;
    char     last_message[4096];
    char     last_tag[64];
    int      last_line;
    size_t   last_length;
    int      flushes;
} Capture;

static void capture_write(const Fl_LogRecord *record, void *user_data)
{
    Capture *capture = user_data;
    fl_mutex_lock(&capture->lock);
    capture->count++;
    capture->levels[record->level]++;
    if (record->sequence <= capture->last_sequence) capture->sequences_increasing = false;
    capture->last_sequence = record->sequence;
    snprintf(capture->last_message, sizeof(capture->last_message), "%s", record->message);
    snprintf(capture->last_tag, sizeof(capture->last_tag), "%s", record->tag);
    capture->last_line = record->line;
    capture->last_length = record->message_length;
    fl_mutex_unlock(&capture->lock);
}

static void capture_flush(void *user_data)
{
    Capture *capture = user_data;
    capture->flushes++;
}

static void capture_init(Capture *capture)
{
    memset(capture, 0, sizeof(*capture));
    capture->sequences_increasing = true;
    fl_mutex_init(&capture->lock);
}

static void test_levels_and_fields(void)
{
    Capture capture;
    capture_init(&capture);
    fl_log_set_level(FL_LOG_DEBUG);
    Fl_LogSink sink = fl_log_add_sink(&(Fl_LogSinkDesc){
        .write = capture_write, .flush = capture_flush, .user_data = &capture,
        .min_level = FL_LOG_INFO });
    CHECK(sink.id != 0);

    FL_LOG_TRACE("t", "dropped by global level");
    FL_LOG_DEBUG("t", "dropped by sink level");
    FL_LOG_INFO("render", "value %d", 42);
    const int line = __LINE__ - 1;
    CHECK(capture.count == 1);
    CHECK(strcmp(capture.last_message, "value 42") == 0);
    CHECK(strcmp(capture.last_tag, "render") == 0);
    CHECK(capture.last_line == line);

    FL_LOG_WARN(NULL, "trailing newline\n\n");
    CHECK(strcmp(capture.last_message, "trailing newline") == 0);
    CHECK(strcmp(capture.last_tag, "flow") == 0);

    fl_log_set_level(FL_LOG_OFF);
    CHECK(!fl_log_enabled(FL_LOG_FATAL));
    FL_LOG_ERROR("t", "suppressed");
    CHECK(capture.count == 2);
    fl_log_set_level(FL_LOG_TRACE);
    CHECK(fl_log_enabled(FL_LOG_TRACE));

    FL_LOG_ERROR("t", "errors flush sinks");
    CHECK(capture.flushes >= 1);
    fl_log_flush();
    CHECK(capture.flushes >= 2);

    CHECK(capture.sequences_increasing);
    CHECK(fl_log_remove_sink(sink));
    CHECK(!fl_log_remove_sink(sink));
    FL_LOG_INFO("t", "after removal");
    CHECK(capture.count == 3);
    fl_mutex_destroy(&capture.lock);
}

static void test_long_message(void)
{
    Capture capture;
    capture_init(&capture);
    Fl_LogSink sink = fl_log_add_sink(&(Fl_LogSinkDesc){ .write = capture_write, .user_data = &capture });
    char big[3500];
    memset(big, 'z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    FL_LOG_INFO("t", "%s", big);
    CHECK(capture.last_length == sizeof(big) - 1);
    CHECK(strlen(capture.last_message) == sizeof(big) - 1);
    fl_log_remove_sink(sink);
    fl_mutex_destroy(&capture.lock);
}

typedef struct Reentrant {
    int  calls;
    bool add_rejected;
} Reentrant;

static void reentrant_write(const Fl_LogRecord *record, void *user_data)
{
    Reentrant *r = user_data;
    r->calls++;
    if (strcmp(record->tag, "outer") != 0) return;
    FL_LOG_INFO("inner", "logged from inside a sink");
    r->add_rejected = fl_log_add_sink(&(Fl_LogSinkDesc){ .write = reentrant_write }).id == 0;
}

static void test_reentrancy(void)
{
    Reentrant r = {0};
    Fl_LogSink sink = fl_log_add_sink(&(Fl_LogSinkDesc){ .write = reentrant_write, .user_data = &r });
    FL_LOG_INFO("outer", "trigger");
    CHECK(r.calls == 1);
    CHECK(r.add_rejected);
    fl_log_remove_sink(sink);
}

static void test_recent_ring(void)
{
    FL_LOG_INFO("ring", "unique-ring-marker-%d", 7);
    char recent[FL_LOG_RECENT_BYTES];
    size_t len = fl_log_copy_recent(recent, sizeof(recent) - 1);
    recent[len] = '\0';
    CHECK(strstr(recent, "unique-ring-marker-7") != NULL);

    for (int i = 0; i < 3000; ++i) FL_LOG_DEBUG("ring", "filler line %d with some padding text", i);
    FL_LOG_INFO("ring", "final-marker");
    len = fl_log_copy_recent(recent, sizeof(recent) - 1);
    recent[len] = '\0';
    CHECK(len == FL_LOG_RECENT_BYTES - 1);
    CHECK(strstr(recent, "final-marker") != NULL);
    CHECK(strstr(recent, "unique-ring-marker-7") == NULL);

    char small[16];
    CHECK(fl_log_copy_recent(small, sizeof(small)) == sizeof(small));
    CHECK(memcmp(small, recent + len - sizeof(small), sizeof(small)) == 0);
}

typedef struct Spammer {
    int id;
} Spammer;

static void spam(void *user_data)
{
    Spammer *s = user_data;
    char name[16];
    snprintf(name, sizeof(name), "spam-%d", s->id);
    fl_thread_set_name(name);
    for (int i = 0; i < 2000; ++i) FL_LOG_INFO("spam", "thread %d record %d", s->id, i);
}

static void test_concurrent_logging(void)
{
    Capture capture;
    capture_init(&capture);
    Fl_LogSink sink = fl_log_add_sink(&(Fl_LogSinkDesc){ .write = capture_write, .user_data = &capture });
    Spammer spammers[6];
    Fl_Thread *threads[6];
    for (int i = 0; i < 6; ++i) {
        spammers[i].id = i;
        threads[i] = fl_thread_create(spam, &spammers[i]);
        CHECK(threads[i] != NULL);
    }
    for (int i = 0; i < 6; ++i) CHECK(fl_thread_join(threads[i]));
    CHECK(capture.count == 6 * 2000);
    CHECK(capture.sequences_increasing);
    fl_log_remove_sink(sink);
    fl_mutex_destroy(&capture.lock);
}

static size_t read_file(const char *path, char *out, size_t cap)
{
    FILE *file = fl_fs_fopen(path, "rb");
    if (!file) return 0;
    size_t n = fread(out, 1, cap - 1, file);
    fclose(file);
    out[n] = '\0';
    return n;
}

static void test_file_sink(const char *scratch)
{
    char dir[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), scratch, "logs"));
    char path[FL_PATH_MAX];
    CHECK(fl_fs_join(path, sizeof(path), dir, "test.log"));
    char rotated[FL_PATH_MAX];
    for (int i = 1; i <= 3; ++i) {
        snprintf(rotated, sizeof(rotated), "%s.%d", path, i);
        fl_fs_remove(rotated);
    }
    fl_fs_remove(path);

    CHECK(fl_log_file_open(&(Fl_LogFileDesc){
        .dir = dir, .name = "test.log", .rotate_bytes = 4096, .keep_files = 2,
        .min_level = FL_LOG_DEBUG }));
    char active[FL_PATH_MAX];
    CHECK(fl_log_file_path(active, sizeof(active)));
    CHECK(strcmp(active, path) == 0);

    fl_thread_set_name("file-test");
    FL_LOG_WARN("file", "file-sink-marker");
    fl_log_flush();
    static char contents[1 << 16];
    read_file(path, contents, sizeof(contents));
    CHECK(strstr(contents, "file-sink-marker") != NULL);
    CHECK(strstr(contents, "[file-test]") != NULL);
    CHECK(strstr(contents, "WARN") != NULL);

    for (int i = 0; i < 400; ++i) FL_LOG_INFO("file", "rotation filler %d .........................", i);
    fl_log_flush();
    snprintf(rotated, sizeof(rotated), "%s.1", path);
    CHECK(fl_fs_exists(rotated));
    snprintf(rotated, sizeof(rotated), "%s.2", path);
    CHECK(fl_fs_exists(rotated));
    snprintf(rotated, sizeof(rotated), "%s.3", path);
    CHECK(!fl_fs_exists(rotated));
    CHECK(fl_fs_file_size(path) <= 4096 + 512);

    fl_log_file_close();
    CHECK(!fl_log_file_path(active, sizeof(active)));
    fl_thread_set_name(NULL);
}

static void test_shutdown_falls_back(void)
{
    Capture capture;
    capture_init(&capture);
    fl_log_add_sink(&(Fl_LogSinkDesc){ .write = capture_write, .user_data = &capture });
    fl_log_shutdown();
    FL_LOG_INFO("t", "goes to stderr after shutdown");
    CHECK(capture.count == 0);
    CHECK(fl_log_init(&(Fl_LogDesc){ .level = FL_LOG_INFO, .console = false }));
    fl_mutex_destroy(&capture.lock);
}

int main(int argc, char **argv)
{
    const char *scratch = argc > 1 ? argv[1] : ".";
    CHECK(fl_fs_mkdir_p(scratch));
    CHECK(fl_log_init(&(Fl_LogDesc){ .level = FL_LOG_TRACE, .console = false }));

    test_levels_and_fields();
    test_long_message();
    test_reentrancy();
    test_recent_ring();
    test_concurrent_logging();
    test_file_sink(scratch);
    test_shutdown_falls_back();

    fl_log_shutdown();
    return FLOW_TEST_RESULT("fl_log_tests");
}
