#include "flow_test.h"
#include "doc/fl_scene_frame.h"
#include "components/fl_spatial.h"
#include "doc/fl_binary.h"
#include "schema/fl_widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <string.h>

/* Check centered crop geometry, bounded validation, no-op revisions and save/reopen persistence. */
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    CHECK(fl_fs_mkdir_p(argv[1]));
    char directory[FL_PATH_MAX];
    CHECK(fl_fs_join(directory, sizeof(directory), argv[1], "aspect-project"));
    Fl_Project *project = fl_project_create(directory, "Aspect", NULL);
    if (!project) project = fl_project_open(directory, NULL);
    CHECK(project);
    if (!project) return 1;
    const Fl_Scene *scene = fl_project_scene_at(project, 0);
    Fl_Id id = scene->id;
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_SCENE_CAMERA,.scene=id}, NULL));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_SETTINGS, .scene = id,
        .width = 1920, .height = 1080, .duration = scene->duration}, NULL));
    Fl_Id entity;
    CHECK(fl_widget_create(project, id, 0, FL_WIDGET_RECTANGLE, 123, 456, &entity));
    Fl_Pose before; Fl_Bounds before_box;
    CHECK(fl_pose_decode(fl_entity_component(fl_scene_entity(scene, entity), FL_COMPONENT_POSE), &before));
    before_box = fl_entity_bounds(fl_scene_entity(scene, entity));
    const uint64_t ratios[][2] = {{0, 0}, {16, 9}, {9, 16}, {1, 1}, {4, 3}, {3, 4}, {21, 9}, {1, 65536}, {65536, 1}};
    for (size_t i = 0; i < sizeof(ratios) / sizeof(*ratios); ++i) {
        CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = id,
            .aspect_width = ratios[i][0], .aspect_height = ratios[i][1]}, NULL));
        Fl_SceneFrame frame = fl_scene_frame(scene);
        CHECK(frame.width > 0 && frame.height > 0);
        CHECK(frame.x >= 0 && frame.y >= 0);
        CHECK(fabs(frame.x * 2 + frame.width - 1920) < 1e-8);
        CHECK(fabs(frame.y * 2 + frame.height - 1080) < 1e-8);
        double expected = ratios[i][0] ? (double)ratios[i][0] / ratios[i][1] : 1920.0 / 1080;
        CHECK(fabs(frame.width / frame.height - expected) < 1e-8);
    }
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = id,
        .aspect_width = 9, .aspect_height = 16}, NULL));
    uint64_t revision = scene->revision;
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = id,
        .aspect_width = 9, .aspect_height = 16}, NULL));
    CHECK(scene->revision == revision);
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = id, .aspect_width = 9}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = id, .aspect_height = 16}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = id,
        .aspect_width = 65537, .aspect_height = 1}, NULL));
    CHECK(scene->revision == revision && scene->width == 1920 && scene->height == 1080);
    Fl_Pose after; Fl_Bounds after_box;
    CHECK(fl_pose_decode(fl_entity_component(fl_scene_entity(scene, entity), FL_COMPONENT_POSE), &after));
    after_box = fl_entity_bounds(fl_scene_entity(scene, entity));
    CHECK(before.x == after.x && before.y == after.y && before_box.width == after_box.width && before_box.height == after_box.height);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_SCENE_CAMERA,.scene=id,.camera_x=125,.camera_y=-75}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind=FL_OP_SCENE_CAMERA,.scene=id,.camera_x=NAN}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind=FL_OP_SCENE_CAMERA,.scene=id,.camera_y=INFINITY}, NULL));
    CHECK(fl_project_save(project));
    fl_project_destroy(project);
    project = fl_project_open(directory, NULL); CHECK(project);
    scene = fl_project_scene(project, id);
    CHECK(scene && scene->aspect_width == 9 && scene->aspect_height == 16);
    Fl_SceneFrame frame = fl_scene_frame(scene);
    CHECK(fabs(frame.x - 781.25) < 1e-8 && frame.y == -75 && frame.width == 607.5 && frame.height == 1080);
    CHECK(scene->camera_x==125 && scene->camera_y==-75);
    uint64_t camera_revision=scene->revision;
    CHECK(fl_project_apply(project, &(Fl_Op){.kind=FL_OP_SCENE_CAMERA,.scene=id,.camera_x=125,.camera_y=-75}, NULL));
    CHECK(scene->revision==camera_revision);
    CHECK(fl_scene_frame(NULL).width == 0);
    CHECK(fl_scene_frame(&(Fl_Scene){0}).height == 0);
    const unsigned char default_background[4] = FL_SCENE_BACKGROUND_DEFAULT;
    CHECK(!memcmp(scene->background, default_background, 4));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_BACKGROUND, .scene = id, .background = {10, 20, 30, 255}}, NULL));
    uint64_t background_revision = scene->revision;
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_BACKGROUND, .scene = id, .background = {10, 20, 30, 255}}, NULL) && scene->revision == background_revision);
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_BACKGROUND, .scene = id, .background = {1, 2, 3, 128}}, NULL));
    CHECK(fl_project_save(project));
    fl_project_destroy(project);
    project = fl_project_open(directory, NULL); CHECK(project);
    scene = fl_project_scene(project, id);
    CHECK(scene && scene->background[0] == 10 && scene->background[1] == 20 && scene->background[2] == 30 && scene->background[3] == 255);
    Fl_Id camera_id = 0;
    CHECK(fl_spatial_create(project, id, 0, "camera", 0, 0, 0, &camera_id));
    scene = fl_project_scene(project, id);
    Fl_Camera camera;
    const Fl_Component *camera_component = fl_entity_component(fl_scene_entity(scene, camera_id), FL_COMPONENT_CAMERA);
    CHECK(fl_camera_decode(camera_component, &camera) && camera.aspect_w == 9 && camera.aspect_h == 16);
    CHECK(fabs(fl_scene_frame(scene).width / fl_scene_frame(scene).height - 9.0 / 16.0) < 1e-8);
    camera.aspect_w = 1; camera.aspect_h = 1;
    unsigned char camera_bytes[FL_CAMERA_BYTES];
    fl_camera_encode(&camera, camera_bytes);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = id, .entity = camera_id,
        .component = {.type = FL_COMPONENT_CAMERA, .version = 1, .size = FL_CAMERA_BYTES, .data = camera_bytes, .id = camera_component->id}}, NULL));
    scene = fl_project_scene(project, id);
    Fl_SceneFrame square = fl_scene_frame(scene);
    CHECK(fabs(square.width - 1080) < 1e-6 && fabs(square.height - 1080) < 1e-6);
    CHECK(fabs(square.x - 545) < 1e-6 && fabs(square.y + 75) < 1e-6);
    fl_project_destroy(project);
    return FLOW_TEST_RESULT("fl_scene_frame_tests");
}
