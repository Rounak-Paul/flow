#include "flow_test.h"
#include "media/fl_png.h"
#include "project/fl_history.h"
#include "platform/fl_fs.h"
typedef struct GLFWwindow GLFWwindow;
#include "ca_internal.h"
#include "widget.h"
#include "../../src/editor/fl_manuscript_panel.c"

/* Mount only the manuscript so its actual dock layout can be checked at a known size. */
static void seed(Ca_DockLayout *layout, void *data)
{
    (void)data; int group = ca_dock_layout_new_group(layout);
    CHECK(ca_dock_layout_add_tab(layout, group, ca_dock_layout_find(layout, FL_MANUSCRIPT_PANEL_ID), -1));
    CHECK(ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, group));
}

/* Find the retained text input identified by id in window. */
static Ca_TextInput *input_by_id(Ca_Window *window, const char *id)
{
    for (uint32_t i = 0; i < ca_pool_slot_count(&window->input_pool); ++i) {
        Ca_TextInput *input = CA_POOL_AT(window->input_pool, Ca_TextInput, i);
        if (input->in_use && !strcmp(input->node->id, id)) return input;
    }
    return NULL;
}

/* Route one shortcut through the real widget keyboard path without OS timing dependencies. */
static void key(Ca_Window *window, Ca_Key code, int modifiers)
{
    int value = code; int *keys = window->key_buf, *mods = window->key_mods_buf;
    uint32_t count = window->key_count;
    window->key_buf = &value; window->key_mods_buf = &modifiers; window->key_count = 1;
    ca_widget_input_pass(window);
    window->key_buf = keys; window->key_mods_buf = mods; window->key_count = count;
}

/* Write a composed GPU frame into the test scratch directory for focused visual review. */
static void captured(const uint8_t *rgba, uint32_t width, uint32_t height, void *data)
{
    size_t size = 0;
    unsigned char *png = fl_png_encode_region(rgba, width, height, 0, 0, width, height, 1000, &size, NULL, NULL);
    CHECK(png && fl_fs_write_atomic(data, png, size)); free(png);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 2;
    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){.app_name = "Flow manuscript regression"});
    CHECK(instance); if (!instance) return 1;
    Ca_Window *window = ca_window_create(instance, &(Ca_WindowDesc){.title = "Manuscript", .width = 960, .height = 720, .native_frame = true});
    CHECK(window); if (!window) return 1;
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.worker_count = 1, .bus = bus});
    Fl_Project *project = fl_project_create(argv[1], "Manuscript UI", bus);
    Fl_SceneEdit *edit = fl_scene_edit_create(project, bus, jobs);
    Fl_Speech *speech = fl_speech_create(project, jobs, bus);
    Fl_AudioPlayer *player = fl_audio_player_create(&(Fl_AudioPlayerDesc){.bus = bus, .silent = true});
    Fl_AssetImporter *importer = fl_asset_importer_create(project, jobs, bus);
    Fl_ManuscriptPanel *panel = fl_manuscript_panel_create(edit, speech, player, importer, bus);
    CHECK(panel); if (!panel) return 1;
    const char *parts[] = {FL_STYLE_CSS_PALETTE, FL_STYLE_CSS_BUTTONS, FL_STYLE_CSS_SYSTEM,
        FL_STYLE_CSS_COMPONENTS, FL_STYLE_CSS_AUTHORING, FL_STYLE_CSS_SCRIPTS};
    size_t length = 1;
    for (size_t i = 0; i < sizeof(parts) / sizeof(*parts); ++i) length += strlen(parts[i]);
    char *css = calloc(length, 1);
    for (size_t i = 0; i < sizeof(parts) / sizeof(*parts); ++i) strcat(css, parts[i]);
    Ca_Stylesheet *sheet = ca_css_parse(css); free(css); ca_instance_set_stylesheet(instance, sheet);
    Ca_Dock *dock = ca_dock_create(window, &(Ca_DockDesc){.default_layout = seed});
    CHECK(ca_dock_register_panel(dock, &(Ca_DockPanelDesc){.id = FL_MANUSCRIPT_PANEL_ID, .title = "Manuscript",
        .build = fl_manuscript_panel_build, .teardown = fl_manuscript_panel_teardown, .user_data = panel}));
    ca_ui_begin(window, &(Ca_DivDesc){.direction = CA_VERTICAL, .width = 960, .height = 720});
    CHECK(ca_dock_mount(dock)); ca_ui_end();
    for (int i = 0; i < 5; ++i) CHECK(ca_instance_tick(instance));
    Ca_TextInput *input = input_by_id(window, "manuscript-narration");
    CHECK(input && input->multiline && panel->draft);
    if (!input || !panel->draft) return 1;
    CHECK(input->node->w > 800 && input->node->h >= 190 && input->node->h <= 210);
    const char *previous_clipboard = ca_clipboard_get_text(window);
    char *clipboard = strdup(previous_clipboard ? previous_clipboard : "");
    window->focused_node = input->node;
    const char *narration = "Welcome to Flow.\n\nWrite your narration, generate a take, and listen before adding it to the timeline. Café voices and Unicode text stay intact.";
    ca_clipboard_set_text(window, narration); key(window, CA_KEY_V, CA_MOD_SUPER);
    CHECK(!strcmp(ca_input_text(input), narration) && !strcmp(panel->draft->text, narration));
    CHECK(window->key_consumed[CA_KEY_V]);
    key(window, CA_KEY_A, CA_MOD_CONTROL); ca_clipboard_set_text(window, "One\r\nTwo"); key(window, CA_KEY_V, CA_MOD_CONTROL);
    CHECK(!strcmp(ca_input_text(input), "One\nTwo"));
    key(window, CA_KEY_ENTER, 0); CHECK(!strcmp(ca_input_text(input), "One\nTwo\n"));
    key(window, CA_KEY_A, CA_MOD_SUPER); key(window, CA_KEY_X, CA_MOD_SUPER);
    CHECK(!ca_input_text(input)[0]); key(window, CA_KEY_V, CA_MOD_SUPER); CHECK(!strcmp(ca_input_text(input), "One\nTwo\n"));
    char *large = malloc(FL_PHASE_TEXT_MAX + 2); memset(large, 'x', FL_PHASE_TEXT_MAX + 1); large[FL_PHASE_TEXT_MAX + 1] = 0;
    key(window, CA_KEY_A, CA_MOD_SUPER); ca_clipboard_set_text(window, large); key(window, CA_KEY_V, CA_MOD_SUPER);
    CHECK(!strcmp(ca_input_text(input), "One\nTwo\n"));
    large[1200] = 0; ca_clipboard_set_text(window, large); key(window, CA_KEY_V, CA_MOD_SUPER);
    CHECK(strlen(ca_input_text(input)) == 1200 && strlen(panel->draft->text) == 1200); free(large);
    key(window, CA_KEY_A, CA_MOD_SUPER); ca_clipboard_set_text(window, narration); key(window, CA_KEY_V, CA_MOD_SUPER);
    CHECK(input->first_line == 0);
    char path[FL_PATH_MAX]; CHECK(fl_fs_join(path, sizeof(path), argv[1], "manuscript.png"));
    CHECK(ca_window_capture(window, captured, path));
    for (int i = 0; i < 5; ++i) CHECK(ca_instance_tick(instance));
    CHECK(fl_fs_exists(path));
    panel->draft->reference_mode = true; invalidate(panel);
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    CHECK(panel->well && !strcmp(panel->draft->text, narration));
    CHECK(!input_by_id(window, "manuscript-reference")); /* Current runner clones from audio without a transcript. */
    CHECK(fl_fs_join(path, sizeof(path), argv[1], "recording.png"));
    CHECK(ca_window_capture(window, captured, path));
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    char reference[FL_PATH_MAX];
    CHECK(fl_voice_preset_path("Ryan", reference, sizeof(reference)));
    use_recording(panel, reference);
    uint64_t deadline = fl_clock_monotonic_ns() + UINT64_C(3000000000);
    while (panel->import_request && fl_clock_monotonic_ns() < deadline) {fl_event_bus_dispatch(bus); CHECK(ca_instance_tick(instance));}
    CHECK(!panel->import_request && panel->draft->reference);
    CHECK(fl_project_asset(fl_scene_edit_project(edit), panel->draft->reference)->kind == FL_ASSET_AUDIO);
    ca_clipboard_set_text(window, clipboard ? clipboard : ""); free(clipboard);
    ca_dock_destroy(dock);
    Ca_Window *narrow = ca_window_create(instance, &(Ca_WindowDesc){.title = "Narrow manuscript", .width = 420, .height = 720, .native_frame = true});
    CHECK(narrow);
    dock = ca_dock_create(narrow, &(Ca_DockDesc){.default_layout = seed});
    CHECK(ca_dock_register_panel(dock, &(Ca_DockPanelDesc){.id = FL_MANUSCRIPT_PANEL_ID, .title = "Manuscript",
        .build = fl_manuscript_panel_build, .teardown = fl_manuscript_panel_teardown, .user_data = panel}));
    ca_ui_begin(narrow, &(Ca_DivDesc){.direction = CA_VERTICAL, .width = 420, .height = 720});
    CHECK(ca_dock_mount(dock)); ca_ui_end();
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    input = input_by_id(narrow, "manuscript-narration");
    CHECK(input && input->node->w > 300 && input->node->w < 420 && input->node->h >= 190);
    CHECK(!strcmp(panel->draft->text, narration));
    CHECK(fl_fs_join(path, sizeof(path), argv[1], "narrow.png"));
    CHECK(ca_window_capture(narrow, captured, path));
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    Fl_Id phase, clip; Fl_Id scene = fl_scene_edit_scene(edit), asset = panel->draft->reference;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "Greeting", .source = "Hello."},&phase));
    Fl_Id beat = fl_scene_phase(fl_project_scene(project,scene),phase)->beat;
    Fl_AudioClip audio = {.asset = asset, .start = {.kind = FL_ANCHOR_BEAT, .beat = beat}, .gain = 1};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene, .clip_record = &audio},&clip));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene, .phase = phase,
        .name = "Greeting", .source = "Hello.", .beat = beat, .clip = clip},NULL));
    Fl_SpeechCue cue = {.text = "Hello", .start = 12000, .end = 60000, .resolved = true};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_CUES, .scene = scene, .phase = phase, .asset = asset, .cues = &cue, .cue_count = 1},NULL));
    panel->expanded_phase = phase; invalidate(panel);
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    Ca_Node *cue_list = NULL;
    for (uint32_t i = 0; i < ca_pool_slot_count(&narrow->node_pool); ++i) {
        Ca_Node *node = CA_POOL_AT(narrow->node_pool,Ca_Node,i);
        if (!strcmp(node->id,"manuscript-cue-list")) cue_list = node;
    }
    CHECK(cue_list && cue_list->h >= 320);
    CHECK(panel->cue_rows[0].cue == fl_scene_phase(fl_project_scene(project,scene),phase)->cues[0].id);
    select_cue(NULL,&panel->cue_rows[0]);
    snprintf(panel->cue_start,sizeof(panel->cue_start),"0.2"); snprintf(panel->cue_end,sizeof(panel->cue_end),"0.6"); repair_cue(NULL,panel);
    CHECK(fl_scene_phase(fl_project_scene(project,scene),phase)->cues[0].start == 24000);
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    ca_scroll_to_bottom(narrow,"manuscript-body");
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    CHECK(input_by_id(narrow,"manuscript-cue-start"));
    CHECK(fl_fs_join(path,sizeof(path),argv[1],"cues.png")); CHECK(ca_window_capture(narrow,captured,path));
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    edit_phase(NULL,&panel->rows[0]);
    CHECK(panel->draft->phase == phase);
    Fl_ManuscriptRow removal = {panel,phase};
    remove_phase(NULL,&removal);
    const Fl_Scene *removed = fl_project_scene(project,scene);
    CHECK(!fl_scene_phase(removed,phase) && removed->clip_count == 0);
    CHECK(removed->beat_count == 1 && fl_project_asset(project,asset));
    CHECK(panel->draft->phase == 0 && panel->expanded_phase == 0);
    CHECK(fl_history_undo(fl_project_history(project),project,NULL,NULL));
    removed = fl_project_scene(project,scene);
    CHECK(fl_scene_phase(removed,phase) && removed->clip_count == 1 && removed->clips[0].id == clip);
    ca_dock_destroy(dock); fl_manuscript_panel_destroy(panel); fl_speech_destroy(speech);
    fl_asset_importer_destroy(importer); fl_audio_player_destroy(player); fl_scene_edit_destroy(edit);
    fl_jobs_destroy(jobs); fl_project_destroy(project); fl_event_bus_destroy(bus);
    ca_instance_destroy(instance); ca_css_destroy(sheet);
    return FLOW_TEST_RESULT("fl_manuscript_panel_tests");
}
