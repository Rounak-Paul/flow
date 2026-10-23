#include "flow_test.h"
#include "fl_speech.h"
#include "media/fl_voice.h"
#include "media/fl_audio.h"
#include "core/fl_atomic.h"
#include "core/fl_clock.h"
#include "core/fl_thread.h"
#include "platform/fl_fs.h"
#include "project/fl_history.h"
#include "project/fl_manuscript.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wait on a condition variable to avoid busy-spinning the test's owner thread. */
static void wait_ms(uint32_t milliseconds)
{
    Fl_Mutex mutex; Fl_CondVar condition;
    fl_mutex_init(&mutex); fl_cond_init(&condition); fl_mutex_lock(&mutex);
    fl_cond_wait_ms(&condition, &mutex, milliseconds);
    fl_mutex_unlock(&mutex); fl_cond_destroy(&condition); fl_mutex_destroy(&mutex);
}

/* Act as a deterministic child TTS executable, writing valid short WAVs without downloading a model. */
static int runner(int argc, char **argv)
{
    const char *out = NULL, *text = "", *alignment = NULL;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "-of")) alignment = argv[i + 1];
        if (!strcmp(argv[i], "-o")) out = argv[i + 1];
        if (!strcmp(argv[i], "-p")) text = argv[i + 1];
    }
    if (alignment) {
        char path[FL_PATH_MAX]; snprintf(path,sizeof(path),"%s.json",alignment);
        const char *json = "{\"transcription\":[{\"text\":\"First paragraph. A second café paragraph.\",\"offsets\":{\"from\":0,\"to\":90},\"tokens\":[]}]}";
        return fl_fs_write_atomic(path,json,strlen(json)) ? 0 : 5;
    }
    if (!out) return 3;
    if (strstr(text, "fail_late") || !strcmp(text, "fail")) {fputs("Expected runner failure\n", stderr); return 7;}
    if (!strcmp(text, "slow")) wait_ms(10000);
    unsigned char wav[1644] = {0};
    memcpy(wav, "RIFF", 4); wav[4] = 0x64; wav[5] = 6; memcpy(wav + 8, "WAVEfmt ", 8);
    wav[16] = 16; wav[20] = 1; wav[22] = 1; wav[24] = 0x40; wav[25] = 0x1f;
    wav[28] = 0x80; wav[29] = 0x3e; wav[32] = 2; wav[34] = 16;
    memcpy(wav + 36, "data", 4); wav[40] = 0x40; wav[41] = 6;
    unsigned char value = 0;
    for (const char *p = text; *p; ++p) value = (unsigned char)(value * 31 + (unsigned char)*p);
    wav[44] = value;
    const char *cap = getenv("FLOW_TEST_VOICE_CAP");
    bool limited = cap && cap[0] && strlen(text) > 80;
    fprintf(stderr, "generated %u frames, 1644 bytes of WAV audio (8000 Hz)\n", limited ? 2048u : 2u);
    const char *capture = getenv("FLOW_TEST_VOICE_CAPTURE");
    if (!limited && capture && capture[0]) {
        FILE *file = fl_fs_fopen(capture, "ab");
        if (!file || fwrite(text, 1, strlen(text), file) != strlen(text)) {if (file) fclose(file); return 8;}
        fclose(file);
    }
    return fl_fs_write_atomic(out, wav, sizeof(wav)) ? 0 : 4;
}

/* Pump the real background service until it finishes or a bounded test deadline expires. */
static void finish(Fl_Speech *speech)
{
    uint64_t end = fl_clock_monotonic_ns() + UINT64_C(5000000000);
    while (fl_speech_running(speech) && fl_clock_monotonic_ns() < end) {fl_speech_pump(speech); wait_ms(5);}
    CHECK(!fl_speech_running(speech));
}

/* Undo exactly one accepted take. */
static bool undo(Fl_Project *project)
{
    char label[FL_HISTORY_LABEL_MAX]; bool conflict = false;
    return fl_history_undo(fl_project_history(project), project, label, &conflict) && !conflict;
}

/* Set a child-runner test control without changing platform-specific environment ownership. */
static void environment(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* Verify every UTF-8 input byte reaches successful parts, and all decoded samples reach the final WAV. */
static void check_chunking(const char *directory)
{
    Fl_VoiceItem item = {0}; char capture[FL_PATH_MAX], staging[FL_PATH_MAX];
    CHECK(fl_fs_join(item.text_path, sizeof(item.text_path), directory, "chunk-text.txt"));
    CHECK(fl_fs_join(item.out_path, sizeof(item.out_path), directory, "chunk-audio.wav"));
    CHECK(fl_fs_join(capture, sizeof(capture), directory, "chunk-capture.txt"));
    snprintf(item.speaker, sizeof(item.speaker), "Ryan"); snprintf(item.language, sizeof(item.language), "English");
    environment("FLOW_TEST_VOICE_CAPTURE", capture);
    char text[8192], error[512];
    const char *cases[] = {"A complete sentence with a café and a number 3.14. ", "日本語。", "abcdefghijklmnopqrstuvwxyz"};
    for (size_t test = 0; test < sizeof(cases) / sizeof(*cases); ++test) {
        text[0] = 0;
        for (int i = 0; i < 100; ++i) strcat(text, cases[test]);
        strcat(text, "End.");
        CHECK(fl_fs_write_atomic(item.text_path, text, strlen(text))); fl_fs_remove(capture);
        atomic_bool cancel = ATOMIC_VAR_INIT(false); volatile uint32_t done = 0; Fl_VoiceProgress progress = {0};
        CHECK(fl_voice_run(&item, 1, &cancel, &done, &progress, error, sizeof(error)) == FL_VOICE_OK);
        CHECK(done == 1 && progress.done == progress.total && progress.done > 1);
        char *captured = fl_fs_read_file(capture, sizeof(text), NULL);
        CHECK(captured && !strcmp(captured, text)); free(captured);
        Fl_AudioInfo info; CHECK(fl_audio_probe(item.out_path, &info));
        CHECK(info.frames == (uint64_t)progress.done * 800 && info.sample_rate == 8000 && info.channels == 1);
    }
    char *maximum = malloc(FL_PHASE_TEXT_MAX + 1);
    CHECK(maximum);
    if (maximum) {
        memset(maximum, 'x', FL_PHASE_TEXT_MAX); maximum[FL_PHASE_TEXT_MAX] = 0;
        CHECK(fl_fs_write_atomic(item.text_path, maximum, FL_PHASE_TEXT_MAX)); fl_fs_remove(capture);
        atomic_bool cancel = ATOMIC_VAR_INIT(false); volatile uint32_t done = 0; Fl_VoiceProgress progress = {0};
        CHECK(fl_voice_run(&item, 1, &cancel, &done, &progress, error, sizeof(error)) == FL_VOICE_OK);
        char *captured = fl_fs_read_file(capture, FL_PHASE_TEXT_MAX, NULL);
        CHECK(captured && !strcmp(captured, maximum)); free(captured);
        Fl_AudioInfo info; CHECK(fl_audio_probe(item.out_path, &info));
        CHECK(info.frames == (uint64_t)progress.done * 800 && progress.done == progress.total);
        free(maximum);
    }
    environment("FLOW_TEST_VOICE_CAP", "1"); fl_fs_remove(capture);
    CHECK(fl_fs_write_atomic(item.text_path, text, strlen(text)));
    atomic_bool cancel = ATOMIC_VAR_INIT(false); volatile uint32_t done = 0; Fl_VoiceProgress progress = {0};
    CHECK(fl_voice_run(&item, 1, &cancel, &done, &progress, error, sizeof(error)) == FL_VOICE_OK);
    CHECK(progress.done == progress.total && progress.done > 10);
    char *captured = fl_fs_read_file(capture, sizeof(text), NULL);
    CHECK(captured && !strcmp(captured, text)); free(captured);
    Fl_AudioInfo info; CHECK(fl_audio_probe(item.out_path, &info)); CHECK(info.frames == (uint64_t)progress.done * 800);
    environment("FLOW_TEST_VOICE_CAP", "");
    strcat(text, " fail_late"); CHECK(fl_fs_write_atomic(item.text_path, text, strlen(text)));
    CHECK(fl_fs_write_atomic(item.out_path, "previous", 8));
    CHECK(fl_voice_run(&item, 1, &cancel, &done, &progress, error, sizeof(error)) == FL_VOICE_FAILED);
    captured = fl_fs_read_file(item.out_path, 16, NULL); CHECK(captured && !strcmp(captured, "previous")); free(captured);
    snprintf(staging, sizeof(staging), "%s.parts", item.out_path); CHECK(!fl_fs_exists(staging));
    CHECK(fl_fs_write_atomic(item.text_path, "words\0lost", 10));
    CHECK(fl_voice_run(&item, 1, &cancel, &done, &progress, error, sizeof(error)) == FL_VOICE_FAILED);
    environment("FLOW_TEST_VOICE_CAPTURE", "");
}

int main(int argc, char **argv)
{
    if (argc > 2) return runner(argc, argv);
    CHECK(argc == 2); if (argc != 2) return 2;
#ifdef _WIN32
    _putenv_s("FLOW_LLAMA_TTS", argv[0]);
#else
    setenv("FLOW_LLAMA_TTS", argv[0], 1);
#endif
    environment("FLOW_WHISPER",argv[0]); environment("FLOW_WHISPER_MODEL",argv[0]);
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.worker_count = 1, .bus = bus});
    Fl_Project *project = fl_project_create(argv[1], "Speech", bus);
    Fl_Speech *speech = fl_speech_create(project, jobs, bus);
    CHECK(bus && jobs && project && speech); if (!speech) return 1;
    Fl_Id scene = fl_project_scene_at(project, 0)->id;
    const char *text = "First paragraph.\nA second café paragraph.";
    CHECK(!fl_speech_preview(speech, scene, "", "Ryan", "English", 0, ""));
    CHECK(fl_speech_preview(speech, scene, text, "Ryan", "English", 0, ""));
    CHECK(!fl_speech_preview(speech, scene, text, "Ryan", "English", 0, ""));
    finish(speech);
    CHECK(!strcmp(fl_speech_state(speech), "ready") && fl_speech_preview_path(speech));
    CHECK(fl_project_asset_count(project) == 0 && fl_project_scene(project, scene)->phase_count == 0);
    CHECK(fl_speech_accept(speech, scene, 0, "Opening"));
    const Fl_Scene *current = fl_project_scene(project, scene);
    CHECK(current->phase_count == 1 && current->clip_count == 1 && fl_project_asset_count(project) == 1);
    CHECK(!strcmp(current->phases[0].text, text));
    Fl_Id phase = current->phases[0].id, clip = current->phases[0].clip, beat = current->phases[0].beat;
    Fl_Id previous = current->clips[0].asset;
    CHECK(clip && beat && current->clips[0].start.beat == beat);
    CHECK(!fl_speech_preview_path(speech) && !fl_speech_accept(speech, scene, 0, "Duplicate"));
    CHECK(undo(project));
    CHECK(fl_project_scene(project, scene)->phase_count == 0 && fl_project_asset_count(project) == 0);
    CHECK(fl_speech_preview(speech, scene, text, "Aiden", "English", 0, "")); finish(speech);
    CHECK(fl_speech_accept(speech, scene, 0, "Opening"));
    current = fl_project_scene(project, scene); phase = current->phases[0].id; clip = current->phases[0].clip;
    beat = current->phases[0].beat; previous = current->clips[0].asset;
    CHECK(fl_speech_preview(speech, scene, "Updated narration", "Aiden", "English", 0, "")); finish(speech);
    CHECK(fl_project_scene(project, scene)->clips[0].asset == previous);
    CHECK(fl_speech_accept(speech, scene, phase, "Revised opening"));
    current = fl_project_scene(project, scene);
    CHECK(current->phase_count == 1 && current->clip_count == 1 && current->phases[0].clip == clip && current->phases[0].beat == beat);
    CHECK(current->clips[0].asset != previous && fl_project_asset_count(project) == 2);
    CHECK(!strcmp(current->phases[0].text, "Updated narration"));
    CHECK(undo(project));
    current = fl_project_scene(project, scene);
    CHECK(current->clips[0].asset == previous && !strcmp(current->phases[0].text, text));
    CHECK(fl_speech_preview(speech, scene, "fail", "Ryan", "English", 0, "")); finish(speech);
    CHECK(!strcmp(fl_speech_state(speech), "failed") && !fl_speech_preview_path(speech));
    CHECK(fl_speech_preview(speech, scene, "slow", "Ryan", "English", 0, ""));
    wait_ms(100); uint64_t cancelled = fl_clock_monotonic_ns(); fl_speech_cancel(speech); finish(speech);
    CHECK(fl_clock_monotonic_ns() - cancelled < UINT64_C(2000000000));
    CHECK(!strcmp(fl_speech_state(speech), "cancelled") && !fl_speech_preview_path(speech));
    CHECK(fl_speech_preview(speech, scene, "Removed phase", "Ryan", "English", 0, "")); finish(speech);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_PHASE_REMOVE, .scene = scene, .phase = phase}, NULL));
    size_t assets = fl_project_asset_count(project);
    CHECK(!fl_speech_accept(speech, scene, phase, "Missing"));
    CHECK(fl_project_scene(project, scene)->phase_count == 0 && fl_project_asset_count(project) == assets);
    fl_speech_discard(speech); CHECK(!fl_speech_preview_path(speech));
    check_chunking(argv[1]);
    fl_speech_destroy(speech); fl_project_destroy(project); fl_jobs_destroy(jobs); fl_event_bus_destroy(bus);
    return FLOW_TEST_RESULT("fl_speech_tests");
}
