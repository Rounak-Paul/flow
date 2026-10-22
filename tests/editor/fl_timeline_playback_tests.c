#include "flow_test.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "core/fl_clock.h"
#include "core/fl_thread.h"
#include "components/fl_script.h"
#include <ca_gpu.h>
#include "../../src/editor/fl_timeline_panel.c"

/* Seed one dock group so playback runs through the editor's actual panel lifecycle. */
static void seed_layout(Ca_DockLayout *layout, void *data)
{
    (void)data;
    int group = ca_dock_layout_new_group(layout);
    CHECK(ca_dock_layout_add_tab(layout, group, ca_dock_layout_find(layout, PANEL_ID), -1));
    CHECK(ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, group));
}

/* Count document-time notifications delivered synchronously by the playback frame effect. */
static bool count_time(const Fl_Event *event, void *data)
{
    const Fl_TimeChange *change = event->payload;
    if (change->kind == FL_TIME_POSITION) ++*(unsigned *)data;
    return false;
}

/* The needle is a viewport overlay, independent of track count and vertical scroll. */
static void check_needle_area(Fl_TimelinePanel *panel)
{
    float viewport_y = 0, viewport_height = 0, needle_y = 0, needle_height = 0;
    ca_div_content_screen_rect(panel->scroll, NULL, &viewport_y, NULL, &viewport_height);
    ca_div_screen_rect(panel->playhead, NULL, &needle_y, NULL, &needle_height);
    CHECK(viewport_height > 0);
    CHECK(fabsf(viewport_y - needle_y) < 0.1f);
    CHECK(fabsf(viewport_height - needle_height) < 0.1f);
}

/* Let clocks advance without UI ticks; tolerate spurious condition wakeups. */
static void wait_for_clock(Fl_Mutex *mutex, Fl_CondVar *condition, uint32_t milliseconds)
{
    uint64_t until = fl_clock_monotonic_ns() + (uint64_t)milliseconds * UINT64_C(1000000);
    fl_mutex_lock(mutex);
    for (uint64_t now = fl_clock_monotonic_ns(); now < until; now = fl_clock_monotonic_ns())
        fl_cond_wait_ms(condition, mutex, (uint32_t)((until-now+999999)/1000000));
    fl_mutex_unlock(mutex);
}

/* Clear one real viewport target while resolution changes retire its in-flight images. */
static void clear_viewport(Ca_Viewport *viewport, void *data)
{
    (void)data;
    VkRenderingAttachmentInfo color = {.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=ca_viewport_image_view(viewport), .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo info = {.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea.extent={ca_viewport_width(viewport), ca_viewport_height(viewport)}, .layerCount=1,
        .colorAttachmentCount=1, .pColorAttachments=&color};
    vkCmdBeginRendering(ca_viewport_cmd(viewport), &info);
    vkCmdEndRendering(ca_viewport_cmd(viewport));
}

/* Draft playback changes image density, preserving layout and restoring native pixels on pause. */
static void check_preview_resolution(Ca_Instance *instance)
{
    Ca_Window *window = ca_window_create(instance, &(Ca_WindowDesc){.title="Preview resolution", .width=640, .height=360});
    CHECK(window); if (!window) return;
    ca_ui_begin(window, &(Ca_DivDesc){.direction=CA_VERTICAL, .width=640, .height=360});
    Ca_Viewport *viewport = ca_viewport(&(Ca_ViewportDesc){.on_render=clear_viewport});
    ca_ui_end();
    CHECK(viewport); if (!viewport) {ca_window_close(window); return;}
    for (unsigned i=0; i<3; ++i) CHECK(ca_instance_tick(instance));
    uint32_t width=ca_viewport_width(viewport), height=ca_viewport_height(viewport);
    float logical=0; ca_viewport_screen_rect(viewport, NULL, NULL, &logical, NULL);
    CHECK(width>160 && height>0);
    ca_viewport_set_max_dimension(viewport, 160);
    for (unsigned i=0; i<3; ++i) CHECK(ca_instance_tick(instance));
    CHECK(ca_viewport_width(viewport)==160);
    CHECK(abs((int)ca_viewport_height(viewport)-(int)((uint64_t)height*160/width))<=1);
    float draft=0; ca_viewport_screen_rect(viewport, NULL, NULL, &draft, NULL);
    CHECK(fabsf(draft-logical)<.1f);
    ca_viewport_set_max_dimension(viewport, 0);
    for (unsigned i=0; i<3; ++i) CHECK(ca_instance_tick(instance));
    CHECK(ca_viewport_width(viewport)==width && ca_viewport_height(viewport)==height);
    ca_window_close(window);
}

/* Run real GPU frames: crossing the edge must not schedule extra playback ticks. */
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){.app_name = "Flow playback regression"});
    if (!instance) { puts("fl_timeline_playback_tests: skipped (no GPU/display)"); return 0; }
    ca_instance_set_continuous(instance, true);
    Ca_Window *window = ca_window_create(instance, &(Ca_WindowDesc){
        .title = "Flow playback regression", .width = 800, .height = 240, .native_frame = true});
    if (!window) { ca_instance_destroy(instance); puts("fl_timeline_playback_tests: skipped (no window)"); return 0; }
    CHECK(fl_fs_mkdir_p(argv[1]));
    char manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(manifest, sizeof(manifest), argv[1], "project.flow.json"));
    fl_fs_remove(manifest);
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_Project *project = fl_project_create(argv[1], "Playback", bus);
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.worker_count = 1, .bus = bus});
    Fl_SceneEdit *edit = fl_scene_edit_create(project, bus, jobs);
    Fl_Playback *playback = fl_playback_create(&(Fl_PlaybackDesc){
        .instance = instance, .edit = edit, .bus = bus, .silent = true});
    Fl_TimelinePanel *panel = fl_timeline_panel_create(edit, playback, bus);
    CHECK(bus && project && edit && playback && panel);
    if (!bus || !project || !edit || !playback || !panel) return 1;
    Fl_Id scene = fl_project_scene_at(project, 0)->id, entity;
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_SETTINGS, .scene = scene,
        .width = 1920, .height = 1080, .duration = 2 * FL_TICKS_PER_SECOND}, NULL));
    CHECK(fl_widget_create(project, scene, 0, FL_WIDGET_RECTANGLE, 0, 0, &entity));
    CHECK(fl_scene_edit_select(edit, scene, entity, false));
    key_selection(NULL, panel);
    panel->selected_key = 0;
    panel->zoom = FL_STYLE_TL_ZOOM_MAX;
    const char *parts[] = {FL_STYLE_CSS_PALETTE, FL_STYLE_CSS_BUTTONS, FL_STYLE_CSS_TIMELINE, FL_STYLE_CSS_TIMELINE_BEATS};
    size_t length = 1;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) length += strlen(parts[i]);
    char *css = malloc(length);
    CHECK(css);
    if (!css) return 1;
    css[0] = 0;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) strcat(css, parts[i]);
    Ca_Stylesheet *sheet = ca_css_parse(css);
    free(css);
    CHECK(sheet);
    ca_instance_set_stylesheet(instance, sheet);
    Ca_Dock *dock = ca_dock_create(window, &(Ca_DockDesc){.default_layout = seed_layout});
    CHECK(dock && ca_dock_register_panel(dock, &(Ca_DockPanelDesc){.id = PANEL_ID, .title = "Timeline",
        .build = fl_timeline_panel_build, .teardown = fl_timeline_panel_teardown, .user_data = panel}));
    ca_ui_begin(window, &(Ca_DivDesc){.direction = CA_VERTICAL, .width = 800, .height = 240});
    CHECK(ca_dock_mount(dock));
    ca_ui_end();
    for (int i = 0; i < 6; ++i) CHECK(ca_instance_tick(instance));
    CHECK(panel->selected_beat == 0 && panel->split_button != NULL);
    unsigned notifications = 0;
    CHECK(fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_TIME_CHANGED),
        .fn = count_time, .user_data = &notifications, .owner = &notifications}).generation);
    float viewport_width=0;
    ca_div_content_screen_rect(panel->scroll,NULL,NULL,&viewport_width,NULL);
    CHECK(viewport_width>0);
    Fl_Tick near_edge=(Fl_Tick)(viewport_width*0.8/(panel->zoom*panel_scale(panel))*FL_TICKS_PER_SECOND);
    fl_playback_seek(playback,near_edge);
    fl_playback_play(playback);
    uint64_t start = fl_clock_monotonic_ns();
    while (fl_clock_monotonic_ns() - start < 600000000ull) {
        unsigned before = notifications;
        CHECK(ca_instance_tick(instance));
        CHECK(notifications - before <= 1);
        float header_x = 0, lane_x = 0;
        ca_div_screen_rect(panel->header_playhead, &header_x, NULL, NULL, NULL);
        ca_div_screen_rect(panel->playhead, &lane_x, NULL, NULL, NULL);
        CHECK(fabsf(header_x - lane_x) < 0.1f);
        if (notifications - before > 1) break;
    }
    CHECK(fl_playback_playing(playback));
    CHECK(ca_signal_get_float(ca_get_scroll_x_signal(window, "timeline-scroll")) > 0);
    fl_playback_pause(playback);
    ca_set_scroll_x(window, "timeline-scroll", 0);
    for (int i = 0; i < 3; ++i) CHECK(ca_instance_tick(instance));
    CHECK(ca_signal_get_float(ca_get_scroll_x_signal(window, "timeline-scroll")) == 0);
    Fl_Id beat = scene_of(panel)->beats[0].id;
    Fl_TimelineItem beat_item = {panel, ITEM_BEAT, beat};
    Ca_DragEvent click = {.window = window, .x = 300, .y = 60};
    item_drag_start(&click, &beat_item);
    item_drag_end(&click, &beat_item);
    CHECK(!panel->rename_window);
    panel->click_ns = fl_clock_monotonic_ns();
    item_drag_start(&click, &beat_item);
    item_drag_end(&click, &beat_item);
    CHECK(panel->rename_window && panel->rename_beat == beat);
    CHECK(panel->rename_input && !strcmp(ca_input_text(panel->rename_input), scene_of(panel)->beats[0].name));
    if (panel->rename_window) ca_window_close(panel->rename_window);
    check_needle_area(panel);
    ca_set_scroll_y(window, "timeline-scroll", 1000000);
    CHECK(ca_get_scroll_y(window, "timeline-scroll") == 0);
    CHECK(fl_scene_edit_select(edit, scene, 0, false));
    for (int i = 0; i < 6; ++i) CHECK(ca_instance_tick(instance));
    check_needle_area(panel);
    ca_set_scroll_y(window, "timeline-scroll", 1000000);
    CHECK(ca_get_scroll_y(window, "timeline-scroll") == 0);
    CHECK(fl_scene_edit_select(edit, scene, entity, false));
    for (int prop = FL_PROP_Y; prop <= FL_PROP_OPACITY; ++prop) {
        panel->property = (Fl_AnimProperty)prop;
        key_selection(NULL, panel);
    }
    for (int i = 0; i < 6; ++i) CHECK(ca_instance_tick(instance));
    ca_set_scroll_y(window, "timeline-scroll", 1000000);
    for (int i = 0; i < 3; ++i) CHECK(ca_instance_tick(instance));
    CHECK(ca_get_scroll_y(window, "timeline-scroll") > 0);
    check_needle_area(panel);
    ca_instance_set_scale(instance, 1.5f);
    for (int i = 0; i < 6; ++i) CHECK(ca_instance_tick(instance));
    check_needle_area(panel);
    /* Worker readiness must preserve elapsed clock time; a real seek must still reset it. */
    Fl_Id script = 0;
    const char *source = "return {sample=function(ctx) ctx.entity.pose.x=ctx.time.seconds end}";
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,
        .name="Playback script", .source=source, .source_size=strlen(source)}, &script));
    Fl_Component component = {0};
    CHECK(fl_script_encode(&(Fl_Script){.script=script, .enabled=true}, &component));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_COMPONENT_SET, .scene=scene, .entity=entity, .component=component}, NULL));
    fl_component_clear(&component);
    fl_playback_seek(playback, FL_TICKS_PER_SECOND / 5);
    fl_playback_play(playback);
    Fl_Mutex mutex; Fl_CondVar condition;
    CHECK(fl_mutex_init(&mutex)); CHECK(fl_cond_init(&condition));
    wait_for_clock(&mutex, &condition, 120);
    fl_scene_edit_state(edit);
    CHECK(fl_scene_edit_evaluating(edit));
    uint64_t deadline = fl_clock_monotonic_ns() + UINT64_C(2000000000);
    while (fl_scene_edit_evaluating(edit) && fl_clock_monotonic_ns() < deadline) {
        fl_event_bus_dispatch(bus);
        wait_for_clock(&mutex, &condition, 1);
    }
    CHECK(!fl_scene_edit_evaluating(edit));
    CHECK(ca_instance_tick(instance));
    CHECK(fl_scene_edit_time(edit) >= FL_TICKS_PER_SECOND * 3 / 10);
    fl_playback_seek(playback, FL_TICKS_PER_SECOND * 3 / 5);
    wait_for_clock(&mutex, &condition, 100);
    CHECK(ca_instance_tick(instance));
    CHECK(fl_scene_edit_time(edit) >= FL_TICKS_PER_SECOND * 68 / 100);
    CHECK(fl_scene_edit_time(edit) < FL_TICKS_PER_SECOND);
    fl_playback_pause(playback);
    fl_cond_destroy(&condition); fl_mutex_destroy(&mutex);
    fl_event_unsubscribe_owner(bus, &notifications);
    check_preview_resolution(instance);
    ca_dock_destroy(dock);
    ca_window_close(window);
    fl_timeline_panel_destroy(panel);
    fl_playback_destroy(playback);
    ca_instance_destroy(instance);
    ca_css_destroy(sheet);
    fl_scene_edit_destroy(edit);
    CHECK(fl_jobs_destroy(jobs)); fl_event_bus_dispatch(bus);
    fl_project_destroy(project);
    CHECK(fl_event_bus_destroy(bus));
    fl_fs_remove(manifest);
    return FLOW_TEST_RESULT("fl_timeline_playback_tests");
}
