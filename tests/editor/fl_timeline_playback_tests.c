#include "flow_test.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "core/fl_clock.h"
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
    (void)event;
    ++*(unsigned *)data;
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
    CHECK(fl_fs_join(manifest, sizeof(manifest), argv[1], "project.flow"));
    fl_fs_remove(manifest);
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_Project *project = fl_project_create(argv[1], "Playback", bus);
    Fl_SceneEdit *edit = fl_scene_edit_create(project, bus);
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
    fl_event_unsubscribe_owner(bus, &notifications);
    ca_dock_destroy(dock);
    ca_window_close(window);
    fl_timeline_panel_destroy(panel);
    fl_playback_destroy(playback);
    ca_instance_destroy(instance);
    ca_css_destroy(sheet);
    fl_scene_edit_destroy(edit);
    fl_project_destroy(project);
    CHECK(fl_event_bus_destroy(bus));
    fl_fs_remove(manifest);
    return FLOW_TEST_RESULT("fl_timeline_playback_tests");
}
