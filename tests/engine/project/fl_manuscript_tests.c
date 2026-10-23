#include "flow_test.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "project/fl_history.h"
#include "project/fl_manuscript.h"
#include "project/fl_project.h"
#include <stdlib.h>
#include <string.h>

#define SECOND ((uint64_t)FL_TICKS_PER_SECOND)

static bool near(double value, double expected)
{
    double gap = value - expected;
    if (gap < 0) gap = -gap;
    return gap < 0.001;
}

static void check_words(void)
{
    char clock[32];
    CHECK(fl_manuscript_words("hello world") == 2 && near(fl_manuscript_seconds("hello world"), 0.8));
    CHECK(fl_manuscript_words("") == 0 && fl_manuscript_seconds("") == 0);
    CHECK(fl_manuscript_words("  a \n b  ") == 2);
    CHECK(fl_manuscript_words("café words") == 2);
    fl_manuscript_format(42, clock, sizeof(clock));
    CHECK(!strcmp(clock, "0:42"));
    fl_manuscript_format(90, clock, sizeof(clock));
    CHECK(!strcmp(clock, "1.5 min"));
}

static bool undo(Fl_Project *p)
{
    char label[FL_HISTORY_LABEL_MAX];
    bool conflict = false;
    return fl_history_undo(fl_project_history(p), p, label, &conflict) && !conflict;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (argc != 2) return 1;
    check_words();
    char manifest[FL_PATH_MAX];
    CHECK(fl_fs_mkdir_p(argv[1]) && fl_fs_join(manifest, sizeof(manifest), argv[1], "project.flow.json"));
    fl_fs_remove(manifest);
    Fl_Project *p = fl_project_create(argv[1], "Manuscript", NULL);
    CHECK(p);
    if (!p) return FLOW_TEST_RESULT("fl_manuscript_tests");
    const Fl_Scene *s = fl_project_scene_at(p, 0);
    Fl_Id scene = s->id, beat = s->beats[0].id;
    CHECK(s->voice && !strcmp(s->voice, "Ryan") && s->language && !strcmp(s->language, "English"));
    CHECK(!s->voice_reference && !s->voice_transcript && s->phase_count == 0 && s->beat_count == 1);

    Fl_Id phase = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "Cold open", .source = "", .position = 0}, &phase));
    s = fl_project_scene(p, scene);
    CHECK(s->phase_count == 1 && s->beat_count == 1 && s->phases[0].beat == beat && s->beats[0].duration == 600000);
    CHECK(!strcmp(s->beats[0].name, "Cold open") && !strcmp(s->phases[0].text, ""));
    CHECK(undo(p));
    s = fl_project_scene(p, scene);
    CHECK(s->phase_count == 0 && s->beat_count == 1 && !strcmp(s->beats[0].name, "Beat 1") && s->beats[0].duration == 600000);

    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "Intro", .source = "one two three four", .position = 0}, &phase));
    s = fl_project_scene(p, scene);
    CHECK(s->phase_count == 1 && s->beat_count == 1 && s->phases[0].id == phase && s->phases[0].beat == s->beats[0].id);
    CHECK(s->beats[0].duration == 192000 && !strcmp(s->beats[0].name, "Intro"));

    Fl_Id later = 0, second = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene, .name = "Spare", .position = 1, .beat_duration = 2 * SECOND}, &later));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "Linked", .source = "hello", .position = 1, .beat = later}, &second));
    s = fl_project_scene(p, scene);
    CHECK(s->beat_count == 2 && s->phase_count == 2 && fl_scene_phase(s, second)->beat == later);

    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene, .phase = second, .name = "Second", .source = "hello there", .beat = later, .clip = 0}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_MOVE, .scene = scene, .phase = second, .position = 0}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(s->phases[0].id == second && !strcmp(s->phases[0].title, "Second") && fl_manuscript_words(s->phases[0].text) == 2);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_REMOVE, .scene = scene, .phase = second}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(s->phase_count == 1 && s->beat_count == 2);
    CHECK(undo(p));
    s = fl_project_scene(p, scene);
    CHECK(s->phase_count == 2);

    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "", .source = "x", .position = s->phase_count}, NULL));
    char *long_text = malloc(FL_PHASE_TEXT_MAX + 2);
    CHECK(long_text);
    if (long_text) {
        memset(long_text, 'a', FL_PHASE_TEXT_MAX + 1);
        long_text[FL_PHASE_TEXT_MAX + 1] = 0;
        CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "Too long", .source = long_text, .position = s->phase_count}, NULL));
        free(long_text);
    }
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene, .name = "123", .language = "English"}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene, .name = "Ryan", .language = "Klingon"}, NULL));

    Fl_Asset image = {.name = "Picture", .kind = FL_ASSET_IMAGE, .hash = {2}, .extension = "png", .size = 8, .width = 4, .height = 4};
    Fl_Asset audio = {.name = "Reference", .kind = FL_ASSET_AUDIO, .hash = {7}, .extension = "wav", .size = 16, .duration = 2 * SECOND, .sample_rate = 48000, .channels = 1};
    Fl_Id picture = 0, reference = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &image}, &picture));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &audio}, &reference));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene, .name = "Vivian", .language = "French", .asset = picture, .source = "bonjour"}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene, .name = "Vivian", .language = "French", .asset = reference, .source = "bonjour"}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(!strcmp(s->voice, "Vivian") && !strcmp(s->language, "French") && s->voice_reference == reference && !strcmp(s->voice_transcript, "bonjour"));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = reference}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene, .name = "Vivian", .language = "French", .asset = 0, .source = "bonjour"}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = reference}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &audio}, &reference));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_VOICE_SET, .scene = scene, .name = "Vivian", .language = "French", .asset = reference, .source = "bonjour"}, NULL));

    s = fl_project_scene(p, scene);
    Fl_Id linked = fl_scene_phase(s, second)->beat;
    Fl_AudioClip clip = {.asset = reference, .start = {.kind = FL_ANCHOR_BEAT, .beat = linked}, .gain = 1};
    Fl_Id clip_id = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene, .clip_record = &clip}, &clip_id));
    s = fl_project_scene(p, scene);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene, .phase = second, .name = fl_scene_phase(s, second)->title,
        .source = fl_scene_phase(s, second)->text, .beat = linked, .clip = clip_id}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_REMOVE, .scene = scene, .clip = clip_id}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(fl_scene_phase(s, second)->clip == 0 && fl_scene_phase(s, second)->beat == linked);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_REMOVE, .scene = scene, .beat = linked}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(fl_scene_phase(s, second)->beat == 0 && s->beat_count >= 1);

    CHECK(fl_project_save(p));
    fl_project_destroy(p);
    p = fl_project_open(argv[1], NULL);
    CHECK(p);
    if (!p) return FLOW_TEST_RESULT("fl_manuscript_tests");
    s = fl_project_scene_at(p, 0);
    CHECK(s->phase_count == 2 && fl_scene_phase(s, second) && !strcmp(fl_scene_phase(s, second)->text, "hello there"));
    CHECK(!strcmp(s->voice, "Vivian") && !strcmp(s->language, "French") && s->voice_reference == reference);
    CHECK(s->voice_transcript && !strcmp(s->voice_transcript, "bonjour"));
    fl_project_destroy(p);
    return FLOW_TEST_RESULT("fl_manuscript_tests");
}
