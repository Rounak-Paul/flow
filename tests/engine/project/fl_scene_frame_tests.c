#include "flow_test.h"
#include "doc/fl_scene_frame.h"
#include "doc/fl_binary.h"
#include "schema/fl_widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>

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
    Fl_Transform before;
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene, entity), FL_COMPONENT_TRANSFORM), &before));
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
    Fl_Transform after;
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene, entity), FL_COMPONENT_TRANSFORM), &after));
    CHECK(before.x == after.x && before.y == after.y && before.width == after.width && before.height == after.height);
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
    char relative[80], path[FL_PATH_MAX];
    snprintf(relative, sizeof(relative), "scenes/%016llx-%016llx.flscene", (unsigned long long)id,
        (unsigned long long)scene->revision);
    CHECK(fl_fs_join(path, sizeof(path), directory, relative));
    fl_project_destroy(project);
    Fl_Binary binary; uint64_t version;
    CHECK(fl_binary_load(&binary, path, "FLSCENE\0", 6, &version));
    CHECK(version == 6 && binary.size >= 32);
    if (version != 6 || binary.size < 32) {free(binary.data); return 1;}
    Fl_Binary legacy = {0};
    size_t end = binary.size - 32;
    Fl_Binary scan = {.data=binary.data,.size=end,.cursor=binary.cursor};
    for (int field=0;field<2;++field) fl_binary_get_u64(&scan);
    char *name=fl_binary_get_string(&scan);free(name);
    for (int field=0;field<3;++field) fl_binary_get_u64(&scan);
    uint64_t entities=fl_binary_get_u64(&scan);
    for (uint64_t entity_index=0;entity_index<entities;++entity_index) {
        fl_binary_get_u64(&scan);fl_binary_get_u64(&scan);name=fl_binary_get_string(&scan);free(name);
        fl_binary_get_u64(&scan);uint64_t components=fl_binary_get_u64(&scan);
        for (uint64_t component=0;component<components;++component) {
            fl_binary_get_u64(&scan);fl_binary_get_u64(&scan);uint64_t bytes=fl_binary_get_u64(&scan);scan.cursor+=(size_t)bytes;
        }
    }
    uint64_t beats=fl_binary_get_u64(&scan);
    for (uint64_t beat=0;beat<beats;++beat) {
        fl_binary_get_u64(&scan);name=fl_binary_get_string(&scan);free(name);fl_binary_get_u64(&scan);fl_binary_get_u64(&scan);
    }
    fl_binary_write(&legacy,binary.data+binary.cursor,scan.cursor-binary.cursor);
    fl_binary_u64(&legacy,0);
    fl_binary_write(&legacy,binary.data+scan.cursor,end-scan.cursor);
    CHECK(fl_binary_save(&legacy, path, "FLSCENE\0", 2));
    free(legacy.data);free(binary.data);
    project = fl_project_open(directory, NULL); CHECK(project);
    scene = fl_project_scene(project, id);
    CHECK(scene && scene->aspect_width == 0 && scene->aspect_height == 0);
    CHECK(scene && scene->camera_x==0 && scene->camera_y==0);
    CHECK(fl_scene_frame(scene).width == 1920);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = id,
        .aspect_width = 16, .aspect_height = 9}, NULL));
    CHECK(fl_project_save(project));
    fl_project_destroy(project);
    return FLOW_TEST_RESULT("fl_scene_frame_tests");
}
