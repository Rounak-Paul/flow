#include "flow_test.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "../../src/editor/fl_timeline_panel.c"

/* Deterministic transport for controller tests without a GPU or audio device. */
struct Fl_Playback {Fl_SceneEdit *edit; bool playing;};
bool fl_playback_playing(const Fl_Playback *playback) {return playback->playing;}
void fl_playback_pause(Fl_Playback *playback) {playback->playing = false;}
void fl_playback_toggle(Fl_Playback *playback) {playback->playing = !playback->playing;}
void fl_playback_seek(Fl_Playback *playback, Fl_Tick tick) {fl_scene_edit_set_time(playback->edit, tick);}

/* Exercise property-scoped authoring, selection cleanup, upsert and navigation. */
int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 1;
    char path[FL_PATH_MAX];
    CHECK(fl_fs_mkdir_p(argv[1]));
    CHECK(fl_fs_join(path, sizeof(path), argv[1], "project.flow")); fl_fs_remove(path);
    Fl_EventBus *bus = fl_event_bus_create(NULL);
    Fl_Project *project = fl_project_create(argv[1], "Animation", bus); CHECK(project); if (!project) return 1;
    Fl_SceneEdit *edit = fl_scene_edit_create(project, bus); CHECK(edit);
    Fl_Playback playback = {.edit = edit};
    CHECK(fl_event_type_register(bus, FL_EVENT_PLAYBACK_CHANGED, sizeof(Fl_PlaybackChange)));
    Fl_TimelinePanel *panel = fl_timeline_panel_create(edit, &playback, bus); CHECK(panel);
    Fl_Id scene = fl_project_scene_at(project, 0)->id, rectangle, group, text;
    CHECK(fl_widget_create(project, scene, 0, FL_WIDGET_RECTANGLE, 10, 20, &rectangle));
    CHECK(fl_widget_create(project, scene, 0, FL_WIDGET_GROUP, 0, 0, &group));
    CHECK(fl_widget_create(project, scene, 0, FL_WIDGET_TEXT, 0, 0, &text));
    CHECK(property_supported(fl_scene_entity(scene_of(panel), rectangle), FL_PROP_FILL));
    CHECK(!property_supported(fl_scene_entity(scene_of(panel), group), FL_PROP_FILL));
    CHECK(!property_supported(fl_scene_entity(scene_of(panel), rectangle), FL_PROP_FONT_SIZE));
    CHECK(property_supported(fl_scene_entity(scene_of(panel), text), FL_PROP_FONT_SIZE));
    CHECK(fl_scene_edit_select(edit, scene, rectangle, false));
    key_selection(NULL, panel);
    CHECK(scene_of(panel)->track_count == 1);
    const Fl_Track *track = fl_scene_track(scene_of(panel), rectangle, FL_PROP_X); CHECK(track && track->key_count == 1);
    Fl_Id first = panel->selected_key, track_id = track->id;
    CHECK(first != 0);
    key_selection(NULL, panel);
    CHECK(fl_scene_track(scene_of(panel), rectangle, FL_PROP_X)->key_count == 1 && panel->selected_key == first);
    fl_scene_edit_set_time(edit, FL_TICKS_PER_SECOND * 2);
    Fl_TimelineItem row = {panel, ITEM_TRACK, track_id};
    key_track(NULL, &row);
    CHECK(fl_scene_track(scene_of(panel), rectangle, FL_PROP_X)->key_count == 2);
    Fl_Id last = panel->selected_key;
    previous_key(NULL, &row); CHECK(fl_scene_edit_time(edit) == 0 && panel->selected_key == first);
    next_key(NULL, &row); CHECK(fl_scene_edit_time(edit) == FL_TICKS_PER_SECOND * 2 && panel->selected_key == last);
    next_key(NULL, &row); CHECK(panel->selected_key == last);
    CHECK(fl_scene_edit_select(edit, scene, group, true));
    panel->property = FL_PROP_Y; key_selection(NULL, panel); CHECK(scene_of(panel)->track_count == 1);
    CHECK(fl_scene_edit_select(edit, scene, group, false)); CHECK(panel->selected_key == 0);
    key_selection(NULL, panel); CHECK(scene_of(panel)->track_count == 2);
    playback.playing = true; panel->property = FL_PROP_OPACITY; key_selection(NULL, panel);
    CHECK(scene_of(panel)->track_count == 2); playback.playing = false;
    delete_key(NULL, panel); CHECK(!fl_scene_track(scene_of(panel), group, FL_PROP_Y));
    CHECK(fl_scene_edit_select(edit, scene, rectangle, false)); CHECK(panel->property == FL_PROP_X);
    const Fl_Scene *scene_record = scene_of(panel);
    Fl_Op late = {.kind = FL_OP_KEY_SET, .scene = scene, .entity = rectangle, .property = FL_PROP_X,
        .anchor = fl_anchor_at(scene_record, FL_TICKS_PER_SECOND * 3), .value = {80}, .ease = FL_EASE_OUT};
    CHECK(fl_project_apply(project, &late, NULL));
    fl_scene_edit_set_time(edit, FL_TICKS_PER_SECOND);
    key_track(NULL, &row);
    fl_scene_edit_set_time(edit, FL_TICKS_PER_SECOND * 4);
    key_track(NULL, &row);
    CHECK(fl_scene_key(scene_of(panel), panel->selected_key, NULL)->ease == FL_EASE_OUT);
    CHECK(fl_scene_edit_select(edit, scene, rectangle, false));
    panel->property = FL_PROP_FILL; key_selection(NULL, panel);
    CHECK(fl_scene_track(scene_of(panel), rectangle, FL_PROP_FILL)->key_count == 1);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = scene, .entity = rectangle}, NULL));
    CHECK(panel->selected_key == 0);
    fl_timeline_panel_destroy(panel); fl_scene_edit_destroy(edit); fl_project_destroy(project);
    CHECK(fl_event_bus_destroy(bus)); fl_fs_remove(path);
    return FLOW_TEST_RESULT("fl_timeline_panel_tests");
}
