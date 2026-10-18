#include "flow_test.h"
#include "project/fl_project.h"
#include "doc/fl_binary.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Count mutations and ensure synchronous observers cannot mutate or save reentrantly. */
static bool observe(const Fl_Event *event, void *data)
{
    size_t *count = data; ++*count;
    const Fl_ProjectChange *c = event->payload;
    CHECK(!fl_project_apply(c->project, &(Fl_Op){.kind = FL_OP_SCENE_ADD, .name = "Reentrant"}, NULL));
    CHECK(!fl_project_save(c->project));
    return false;
}

/* Construct checksummed scene fixtures containing semantically invalid hierarchies. */
static void fixture(const char *dir, Fl_Id parent, bool duplicate)
{
    char path[FL_PATH_MAX]; Fl_Binary b = {0};
    unsigned char transform[56]; fl_transform_encode(&(Fl_Transform){.width = 320, .height = 180, .scale_x = 1, .scale_y = 1}, transform);
    fl_binary_string(&b, "Fixture"); fl_binary_u64(&b, 9); fl_binary_u64(&b, 1);
    fl_binary_u64(&b, 1); fl_binary_u64(&b, 1); fl_binary_u64(&b, 0);
    CHECK(fl_fs_join(path, sizeof(path), dir, "project.flow"));
    CHECK(fl_binary_save(&b, path, "FLPROJ\0\0", 3)); free(b.data); b = (Fl_Binary){0};
    fl_binary_u64(&b, 1); fl_binary_u64(&b, 1); fl_binary_string(&b, "Scene");
    fl_binary_u64(&b, 1920); fl_binary_u64(&b, 1080); fl_binary_u64(&b, 600000); fl_binary_u64(&b, 2);
    for (size_t i = 0; i < 2; ++i) {
        fl_binary_u64(&b, duplicate ? 2 : i + 2); fl_binary_u64(&b, i ? 2 : parent);
        fl_binary_string(&b, "Entity"); fl_binary_u64(&b, 1); fl_binary_u64(&b, 1);
        fl_binary_u64(&b, FL_COMPONENT_TRANSFORM); fl_binary_u64(&b, 1); fl_binary_u64(&b, sizeof(transform));
        fl_binary_write(&b, transform, sizeof(transform));
    }
    for (int field = 0; field < 6; ++field) fl_binary_u64(&b, 0);
    CHECK(fl_fs_join(path, sizeof(path), dir, "scenes/0000000000000001-0000000000000001.flscene"));
    CHECK(fl_binary_save(&b, path, "FLSCENE\0", 6)); free(b.data);
}

/* Exercise model ownership, mutation validation, transaction persistence and corrupt-input rejection. */
int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 1;
    CHECK(fl_fs_mkdir_p(argv[1]));
    char dir[FL_PATH_MAX], path[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), argv[1], "project"));
    CHECK(fl_fs_join(path, sizeof(path), dir, "project.flow")); fl_fs_remove(path);
    Fl_EventBus *bus = fl_event_bus_create(NULL); CHECK(bus);
    Fl_Project *p = fl_project_create(dir, "Motion", bus); CHECK(p); if (!p) return 1;
    CHECK(!fl_project_open(dir, bus)); CHECK(!fl_project_create(dir, "Overwrite", bus));
    CHECK(!fl_project_dirty(p)); CHECK(!strcmp(fl_project_name(p), "Motion"));
    CHECK(fl_project_scene_count(p) == 1);
    size_t changes = 0;
    Fl_EventType changed = fl_event_type_find(bus, FL_EVENT_PROJECT_CHANGED);
    CHECK(fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = changed, .fn = observe, .user_data = &changes, .owner = &changes}).generation);
    Fl_Id first = fl_project_scene_at(p, 0)->id, second = 0, root = 0, child = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_ADD, .name = "Second"}, &second));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_MOVE, .scene = second, .position = 0}, NULL));
    CHECK(fl_project_scene_at(p, 0)->id == second);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = first, .name = "Parent"}, &root));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = first, .parent = root, .name = "Child"}, &child));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_PARENT, .scene = first, .entity = root, .parent = child}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_PARENT, .scene = first, .entity = root, .parent = root}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = first, .parent = 99999, .name = "Invalid"}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_MOVE, .scene = first, .entity = child, .position = 0}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_RENAME, .scene = first, .entity = root, .name = "Renamed"}, NULL));
    Fl_Transform t = {12, 23, 450, 240, .5, 2, 3}, decoded;
    unsigned char transform[56]; fl_transform_encode(&t, transform);
    Fl_Component c = {FL_COMPONENT_TRANSFORM, 1, sizeof(transform), transform};
    CHECK(fl_transform_decode(&c, &decoded)); CHECK(decoded.x == 12 && decoded.scale_y == 3);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = first, .entity = child, .component = c}, NULL));
    t.x = NAN; fl_transform_encode(&t, transform);
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = first, .entity = child, .component = c}, NULL));
    unsigned char custom[] = {0, 255, 4, 128};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = first, .entity = root,
                                   .component = {900, 7, sizeof(custom), custom}}, NULL));
    custom[0] = 13;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_VISIBLE, .scene = first, .entity = root, .visible = false}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_SETTINGS, .scene = first, .width = 1280, .height = 720, .duration = 240000}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_SETTINGS, .scene = first, .width = 0, .height = 720, .duration = 1}, NULL));
    CHECK(changes == 10); CHECK(fl_project_save(p)); CHECK(!fl_project_dirty(p));
    fl_project_destroy(p); p = fl_project_open(dir, bus); CHECK(p); if (!p) return 1;
    const Fl_Scene *s = fl_project_scene(p, first); CHECK(s && s->width == 1280 && s->duration == 240000);
    const Fl_Entity *e = fl_scene_entity(s, root); CHECK(e && !e->visible && !strcmp(e->name, "Renamed"));
    const Fl_Component *unknown = fl_entity_component(e, 900); CHECK(unknown && unknown->data[0] == 0 && unknown->version == 7);
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(s, child), FL_COMPONENT_TRANSFORM), &decoded));
    CHECK(decoded.x == 12 && decoded.rotation == .5);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_COMPONENT_REMOVE, .scene = first, .entity = root, .component.type = 900}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = first, .entity = root}, NULL));
    CHECK(fl_project_scene(p, first)->entity_count == 0);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_REMOVE, .scene = second}, NULL));
    CHECK(fl_project_save(p)); fl_project_destroy(p);
    p = fl_project_open(dir, NULL); CHECK(p && fl_project_scene_count(p) == 1); fl_project_destroy(p);
    /* A failed manifest commit must leave all previously referenced scene files readable. */
    p = fl_project_open(dir, NULL); CHECK(p);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_RENAME, .scene = first, .name = "Uncommitted"}, NULL));
    char blocked[FL_PATH_MAX], moved[FL_PATH_MAX];
    CHECK(fl_fs_join(blocked, sizeof(blocked), dir, "project.flow.tmp")); CHECK(fl_fs_mkdir_p(blocked));
    CHECK(!fl_project_save(p)); CHECK(fl_project_dirty(p)); fl_project_destroy(p);
    p = fl_project_open(dir, NULL); CHECK(p);
    CHECK(p && strcmp(fl_project_scene(p, first)->name, "Uncommitted")); fl_project_destroy(p);
    CHECK(fl_fs_join(moved, sizeof(moved), dir, "blocked-temp")); CHECK(fl_fs_rename(blocked, moved));
    /* Every prefix of a real manifest must fail; restoring it must still work. */
    size_t size = 0; char *bytes = fl_fs_read_file(path, SIZE_MAX - 1, &size); CHECK(bytes);
    for (size_t i = 0; i < size; ++i) {
        CHECK(fl_fs_write_atomic(path, bytes, i)); CHECK(!fl_project_open(dir, NULL));
    }
    CHECK(fl_fs_write_atomic(path, bytes, size));
    bytes[size - 1] ^= 1; CHECK(fl_fs_write_atomic(path, bytes, size)); CHECK(!fl_project_open(dir, NULL));
    free(bytes);
    fixture(dir, 3, false); CHECK(!fl_project_open(dir, NULL));
    fixture(dir, 99, false); CHECK(!fl_project_open(dir, NULL));
    fixture(dir, 0, true); CHECK(!fl_project_open(dir, NULL));
    fixture(dir, 0, false); p = fl_project_open(dir, NULL); CHECK(p); fl_project_destroy(p);
    CHECK(fl_fs_remove(path));
    /* Deep documents must load, validate and delete without recursion or quadratic ancestor walks. */
    CHECK(fl_fs_join(dir, sizeof(dir), argv[1], "deep"));
    CHECK(fl_fs_join(path, sizeof(path), dir, "project.flow")); fl_fs_remove(path);
    p = fl_project_create(dir, "Deep hierarchy", NULL); CHECK(p);
    Fl_Id deep_scene = fl_project_scene_at(p, 0)->id, parent = 0, top = 0;
    for (size_t i = 0; i < 20000; ++i) {
        Fl_Id id = 0;
        CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = deep_scene, .parent = parent, .name = "Node"}, &id));
        if (!i) top = id;
        parent = id;
    }
    CHECK(fl_project_save(p)); fl_project_destroy(p);
    p = fl_project_open(dir, NULL); CHECK(p);
    CHECK(p && fl_project_scene(p, deep_scene)->entity_count == 20000);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = deep_scene, .entity = top}, NULL));
    CHECK(fl_project_scene(p, deep_scene)->entity_count == 0); fl_project_destroy(p);
    CHECK(fl_fs_remove(path));
    fl_event_unsubscribe_owner(bus, &changes); CHECK(fl_event_bus_destroy(bus));
    return FLOW_TEST_RESULT("fl_project_tests");
}
