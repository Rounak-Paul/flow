#include "flow_test.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "media/fl_png.h"
#include "schema/fl_widgets.h"
typedef struct GLFWwindow GLFWwindow;
#include "ca_internal.h"
#include "../../src/editor/fl_timeline_panel.c"

#define SECOND ((Fl_Tick)FL_TICKS_PER_SECOND)

/* Mount the actual timeline at a stable editor size. */
static void seed(Ca_DockLayout *layout, void *data)
{
    (void)data; int group = ca_dock_layout_new_group(layout);
    CHECK(ca_dock_layout_add_tab(layout,group,ca_dock_layout_find(layout,PANEL_ID),-1));
    CHECK(ca_dock_layout_set_root(layout,CA_DOCK_MAIN_WINDOW,group));
}

/* Save the composed timeline frame for focused visual inspection. */
static void captured(const uint8_t *rgba, uint32_t width, uint32_t height, void *data)
{
    size_t size = 0; unsigned char *png = fl_png_encode_region(rgba,width,height,0,0,width,height,1100,&size,NULL,NULL);
    CHECK(png && fl_fs_write_atomic(data,png,size)); free(png);
}

/* Resolve a retained select by the binding control's stable CSS ID. */
static Ca_Select *select_by_id(Ca_Window *window, const char *id)
{
    for (uint32_t i = 0; i < ca_pool_slot_count(&window->select_pool); ++i) {
        Ca_Select *select = CA_POOL_AT(window->select_pool,Ca_Select,i);
        if (select->in_use && !strcmp(select->node->id,id)) return select;
    }
    return NULL;
}

/* Native preview/export sampling must follow a cue-bound opacity reveal. */
static void check_reveal(const Fl_Scene *scene, Fl_Tick tick, double opacity)
{
    Fl_EntityState state;
    CHECK(fl_entity_state_sample(scene,0,tick,&state)); CHECK(fabs(state.opacity - opacity) < 0.0001);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 2;
    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){.app_name = "Speech bindings"}); CHECK(instance); if (!instance) return 1;
    ca_instance_set_continuous(instance,true);
    Ca_Window *window = ca_window_create(instance,&(Ca_WindowDesc){.title = "Speech cue bindings", .width = 1100, .height = 440, .native_frame = true});
    CHECK(window); if (!window) return 1;
    Fl_EventBus *bus = fl_event_bus_create(NULL); Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.worker_count = 1, .bus = bus});
    Fl_Project *project = fl_project_create(argv[1],"Cue binding",bus); Fl_SceneEdit *edit = fl_scene_edit_create(project,bus,jobs);
    Fl_Playback *playback = fl_playback_create(&(Fl_PlaybackDesc){.instance = instance, .edit = edit, .bus = bus, .silent = true});
    Fl_TimelinePanel *panel = fl_timeline_panel_create(edit,playback,bus); CHECK(panel); if (!panel) return 1;
    Fl_Id scene = fl_project_scene_at(project,0)->id, phase, asset, clip, entity, key;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "Introduction", .source = "Reveal the diagram."},&phase));
    Fl_Id beat = fl_scene_phase(fl_project_scene(project,scene),phase)->beat;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = beat, .name = "Introduction", .beat_duration = 10 * SECOND},NULL));
    Fl_Asset record = {.name = "Narration", .kind = FL_ASSET_AUDIO, .hash = {39}, .extension = "wav", .size = 8, .duration = 10 * SECOND, .sample_rate = 24000, .channels = 1};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record},&asset));
    Fl_AudioClip audio = {.asset = asset, .start = {.kind = FL_ANCHOR_BEAT, .beat = beat}, .gain = 1};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene, .clip_record = &audio},&clip));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene, .phase = phase, .name = "Introduction", .source = "Reveal the diagram.", .beat = beat, .clip = clip},NULL));
    Fl_SpeechCue measured[] = {{.text = "diagram", .start = 3 * SECOND, .end = 4 * SECOND, .resolved = true}};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_CUES, .scene = scene, .phase = phase, .asset = asset, .cues = measured, .cue_count = 1},NULL));
    Fl_Id cue = fl_scene_phase(fl_project_scene(project,scene),phase)->cues[0].id;
    CHECK(fl_widget_create(project,scene,0,FL_WIDGET_RECTANGLE,0,0,&entity)); CHECK(fl_scene_edit_select(edit,scene,entity,false));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = entity, .property = FL_PROP_OPACITY, .anchor = {0}, .value = {0}, .ease = FL_EASE_HOLD},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = entity, .property = FL_PROP_OPACITY,
        .anchor = {.offset = 3 * SECOND}, .value = {1}, .ease = FL_EASE_LINEAR},&key));
    panel->selected_key = key; panel->property = FL_PROP_OPACITY;
    const char *parts[] = {FL_STYLE_CSS_PALETTE,FL_STYLE_CSS_BUTTONS,FL_STYLE_CSS_SYSTEM,FL_STYLE_CSS_COMPONENTS,FL_STYLE_CSS_AUTHORING,FL_STYLE_CSS_TIMELINE,FL_STYLE_CSS_TIMELINE_BEATS};
    size_t length = 1; for (size_t i = 0; i < sizeof(parts)/sizeof(*parts); ++i) length += strlen(parts[i]);
    char *css = calloc(length,1); for (size_t i = 0; i < sizeof(parts)/sizeof(*parts); ++i) strcat(css,parts[i]);
    Ca_Stylesheet *sheet = ca_css_parse(css); free(css); ca_instance_set_stylesheet(instance,sheet);
    Ca_Dock *dock = ca_dock_create(window,&(Ca_DockDesc){.default_layout = seed});
    CHECK(ca_dock_register_panel(dock,&(Ca_DockPanelDesc){.id = PANEL_ID, .title = "Timeline", .build = fl_timeline_panel_build, .teardown = fl_timeline_panel_teardown, .user_data = panel}));
    ca_ui_begin(window,&(Ca_DivDesc){.direction = CA_VERTICAL, .width = 1100, .height = 440}); CHECK(ca_dock_mount(dock)); ca_ui_end();
    for (int i = 0; i < 5; ++i) CHECK(ca_instance_tick(instance));
    CHECK(panel->cue_count == 2 && panel->cue_ids[1] == cue);
    Ca_Select *choice = select_by_id(window,"timeline-speech-cue"); CHECK(choice);
    if (choice) {ca_select_set(choice,1); binding_cue_changed(choice,panel);}
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    const Fl_Keyframe *bound = fl_scene_key(fl_project_scene(project,scene),key,NULL); CHECK(bound && bound->at.cue == cue && bound->value[0] == 1);
    check_reveal(fl_project_scene(project,scene),2 * SECOND,0); check_reveal(fl_project_scene(project,scene),4 * SECOND,1);
    measured[0].start = 5 * SECOND; measured[0].end = 6 * SECOND;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_CUES, .scene = scene, .phase = phase, .asset = asset, .cues = measured, .cue_count = 1},NULL));
    check_reveal(fl_project_scene(project,scene),4 * SECOND,0); check_reveal(fl_project_scene(project,scene),6 * SECOND,1);
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    char path[FL_PATH_MAX]; CHECK(fl_fs_join(path,sizeof(path),argv[1],"binding.png")); CHECK(ca_window_capture(window,captured,path));
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    measured[0].text = "replacement";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_CUES, .scene = scene, .phase = phase, .asset = asset, .cues = measured, .cue_count = 1},NULL));
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    CHECK(panel->unresolved_count == 2 && select_by_id(window,"timeline-unresolved-keys"));
    Fl_Id replacement = fl_scene_phase(fl_project_scene(project,scene),phase)->cues[0].id;
    choice = select_by_id(window,"timeline-speech-cue"); int repair = 0;
    for (size_t i = 1; i < panel->cue_count; ++i) if (panel->cue_ids[i] == replacement) repair = (int)i;
    CHECK(choice && repair);
    if (choice && repair) {ca_select_set(choice,repair); binding_cue_changed(choice,panel);}
    for (int i = 0; i < 4; ++i) CHECK(ca_instance_tick(instance));
    CHECK(panel->unresolved_count == 0);
    bound = fl_scene_key(fl_project_scene(project,scene),key,NULL); CHECK(bound && bound->at.cue == replacement && bound->value[0] == 1);
    ca_dock_destroy(dock); fl_timeline_panel_destroy(panel); fl_playback_destroy(playback); fl_scene_edit_destroy(edit);
    fl_jobs_destroy(jobs); fl_project_destroy(project); fl_event_bus_destroy(bus); ca_instance_destroy(instance); ca_css_destroy(sheet);
    return FLOW_TEST_RESULT("fl_speech_binding_tests");
}
