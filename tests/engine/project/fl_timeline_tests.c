#include "flow_test.h"
#include "doc/fl_scene_geometry.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "schema/fl_widgets.h"
#include "time/fl_scene_state.h"
#include <math.h>
#include <string.h>

#define SECOND ((Fl_Tick)FL_TICKS_PER_SECOND)

/* Set a scalar key on entity at a beat-relative anchor. */
static bool key(Fl_Project *p, Fl_Id scene, Fl_Id entity, Fl_AnimProperty prop, Fl_TimeAnchor at, double value, Fl_Ease ease, Fl_Id *id)
{
    return fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = entity, .property = prop,
                                        .anchor = at, .value = {value, 0, 0, 0}, .ease = ease}, id);
}

/* Sample a scalar track. */
static double sample(const Fl_Project *p, Fl_Id scene, Fl_Id entity, Fl_AnimProperty prop, Fl_Tick tick)
{
    const Fl_Scene *s = fl_project_scene(p, scene);
    double v[4] = {NAN};
    fl_track_sample(s, fl_scene_track(s, entity, prop), tick, v);
    return v[0];
}

/* Shrink/expand in-beat offsets while retaining absolute and later-beat anchors. */
static void check_beat_scaling(Fl_Project *p, Fl_Id scene)
{
    const Fl_Scene *s = fl_project_scene(p, scene);
    Fl_Id beat = s->beats[0].id, later, entity, first, last, ending, absolute, following, boundary;
    CHECK(fl_widget_create(p, scene, 0, FL_WIDGET_RECTANGLE, 0, 0, &entity));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene, .name = "Following", .position = 1,
        .beat_duration = 4 * SECOND}, &later));
    CHECK(key(p, scene, entity, FL_PROP_X, (Fl_TimeAnchor){FL_ANCHOR_BEAT, beat, false, SECOND}, 10, FL_EASE_IN, &first));
    CHECK(key(p, scene, entity, FL_PROP_X, (Fl_TimeAnchor){FL_ANCHOR_BEAT, beat, false, 3 * SECOND}, 30, FL_EASE_OUT, &last));
    CHECK(key(p, scene, entity, FL_PROP_Y, (Fl_TimeAnchor){FL_ANCHOR_BEAT, beat, true, -SECOND}, 20, FL_EASE_HOLD, &ending));
    CHECK(key(p, scene, entity, FL_PROP_WIDTH, (Fl_TimeAnchor){FL_ANCHOR_ABSOLUTE, 0, false, 2 * SECOND}, 50, FL_EASE_LINEAR, &absolute));
    CHECK(key(p, scene, entity, FL_PROP_Y, (Fl_TimeAnchor){FL_ANCHOR_BEAT, later, false, SECOND}, 40, FL_EASE_LINEAR, &following));
    CHECK(key(p, scene, entity, FL_PROP_X, (Fl_TimeAnchor){FL_ANCHOR_BEAT, beat, false, 4 * SECOND}, 40, FL_EASE_LINEAR, &boundary));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = beat, .name = "Scaled",
        .beat_duration = 2 * SECOND}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, first, NULL)->at) == SECOND / 2);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, last, NULL)->at) == 3 * SECOND / 2);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, boundary, NULL)->at) == 2 * SECOND);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, ending, NULL)->at) == 3 * SECOND / 2);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, absolute, NULL)->at) == SECOND);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, following, NULL)->at) == 3 * SECOND);
    CHECK(fl_scene_key(s, first, NULL)->ease == FL_EASE_IN && fl_scene_key(s, last, NULL)->value[0] == 30);
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = beat, .name = "Invalid",
        .beat_duration = 0}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = beat, .name = "Scaled",
        .beat_duration = 8 * SECOND}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, first, NULL)->at) == 2 * SECOND);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, ending, NULL)->at) == 6 * SECOND);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, following, NULL)->at) == 9 * SECOND);
    CHECK(fl_scene_key(s, ending, NULL)->at.end && fl_scene_key(s, ending, NULL)->at.offset == -2 * SECOND);
}

/* Splitting is atomic and preserves keys/clips on either side and at the boundary. */
static void check_beat_splitting(Fl_Project *p)
{
    Fl_Id scene, entity, right, asset;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_ADD, .name = "Splitting"}, &scene));
    const Fl_Scene *s = fl_project_scene(p, scene);
    Fl_Id beat = s->beats[0].id;
    CHECK(fl_widget_create(p, scene, 0, FL_WIDGET_RECTANGLE, 0, 0, &entity));
    Fl_TimeAnchor anchors[] = {
        {FL_ANCHOR_BEAT, beat, false, SECOND}, {FL_ANCHOR_BEAT, beat, false, 2 * SECOND},
        {FL_ANCHOR_BEAT, beat, false, 4 * SECOND}, {FL_ANCHOR_BEAT, beat, true, -4 * SECOND},
        {FL_ANCHOR_BEAT, beat, true, -SECOND}, {FL_ANCHOR_BEAT, beat, true, 0},
        {FL_ANCHOR_ABSOLUTE, 0, false, 3 * SECOND}};
    Fl_Id keys[7]; Fl_Tick times[7];
    for (size_t i = 0; i < 7; ++i) {
        times[i] = fl_anchor_resolve(s, &anchors[i]);
        CHECK(key(p, scene, entity, (Fl_AnimProperty)i, anchors[i], 1, FL_EASE_IN_OUT, &keys[i]));
    }
    Fl_Asset audio = {.name = "Split audio", .kind = FL_ASSET_AUDIO, .hash = {19}, .extension = "wav", .size = 10,
        .duration = SECOND, .sample_rate = 48000, .channels = 1};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &audio}, &asset));
    Fl_AudioClip clip = {.asset = asset, .start = {FL_ANCHOR_BEAT, beat, true, -SECOND}, .gain = 1};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene, .clip_record = &clip}, NULL));
    uint64_t revision = s->revision;
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_SPLIT, .scene = scene, .beat = beat, .name = "Right", .tick = 0}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_SPLIT, .scene = scene, .beat = beat, .name = "Right", .tick = 5 * SECOND}, NULL));
    CHECK(s->revision == revision && s->beat_count == 1);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_SPLIT, .scene = scene, .beat = beat, .name = "Right", .tick = 2 * SECOND}, &right));
    CHECK(s->revision == revision + 1 && s->beat_count == 2 && fl_scene_length(s) == 5 * SECOND);
    for (size_t i = 0; i < 7; ++i) CHECK(fl_anchor_resolve(s, &fl_scene_key(s, keys[i], NULL)->at) == times[i]);
    CHECK(fl_scene_key(s, keys[0], NULL)->at.beat == beat);
    CHECK(fl_scene_key(s, keys[1], NULL)->at.beat == right && fl_scene_key(s, keys[1], NULL)->at.offset == 0);
    CHECK(fl_scene_key(s, keys[2], NULL)->at.beat == right && fl_scene_key(s, keys[2], NULL)->at.offset == 2 * SECOND);
    CHECK(fl_scene_key(s, keys[3], NULL)->at.beat == beat && fl_scene_key(s, keys[3], NULL)->at.offset == -SECOND);
    CHECK(s->clips[0].start.beat == right && fl_anchor_resolve(s, &s->clips[0].start) == 4 * SECOND);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = right, .name = "Right",
        .beat_duration = 6 * SECOND}, NULL));
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, keys[2], NULL)->at) == 6 * SECOND);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, keys[0], NULL)->at) == SECOND);
    revision = s->revision;
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = right, .name = "Overflow",
        .beat_duration = UINT64_MAX - 1}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_SETTINGS, .scene = scene, .width = 1280, .height = 720,
        .duration = INT64_MAX}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene, .name = "Overflow",
        .position = s->beat_count, .beat_duration = UINT64_MAX}, NULL));
    CHECK(s->revision == revision);
}

/* Recover malformed old timing without dropping keys, then verify idempotent persistence. */
static void check_key_ownership_repair(const char *root)
{
    char directory[FL_PATH_MAX], manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(directory, sizeof(directory), root, "repair"));
    CHECK(fl_fs_mkdir_p(directory) && fl_fs_join(manifest, sizeof(manifest), directory, "project.flow"));
    fl_fs_remove(manifest);
    Fl_Project *p = fl_project_create(directory, "Repair", NULL); CHECK(p); if (!p) return;
    Fl_Id scene = fl_project_scene_at(p, 0)->id, first = fl_project_scene_at(p, 0)->beats[0].id, second, entity;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene, .name = "Second", .position = 1,
        .beat_duration = 3 * SECOND}, &second));
    CHECK(fl_widget_create(p, scene, 0, FL_WIDGET_RECTANGLE, 0, 0, &entity));
    Fl_Id keys[3];
    CHECK(key(p, scene, entity, FL_PROP_X, (Fl_TimeAnchor){FL_ANCHOR_BEAT, first, false, SECOND}, 10, FL_EASE_IN, &keys[0]));
    CHECK(key(p, scene, entity, FL_PROP_Y, (Fl_TimeAnchor){FL_ANCHOR_BEAT, second, false, SECOND}, 20, FL_EASE_OUT, &keys[1]));
    CHECK(key(p, scene, entity, FL_PROP_WIDTH, (Fl_TimeAnchor){FL_ANCHOR_BEAT, second, true, -SECOND}, 30, FL_EASE_HOLD, &keys[2]));
    const Fl_Scene *s = fl_project_scene(p, scene);
    ((Fl_Keyframe *)fl_scene_key(s, keys[0], NULL))->at.offset = 6 * SECOND;
    ((Fl_Keyframe *)fl_scene_key(s, keys[1], NULL))->at = (Fl_TimeAnchor){FL_ANCHOR_ABSOLUTE, 0, false, 9 * SECOND};
    uint64_t revision = s->revision;
    CHECK(fl_project_save(p)); fl_project_destroy(p);
    p = fl_project_open(directory, NULL); CHECK(p); if (!p) return;
    s = fl_project_scene(p, scene);
    CHECK(s->revision == revision + 1 && fl_scene_length(s) == 9 * SECOND);
    const Fl_Tick expected[] = {6 * SECOND, 9 * SECOND, 7 * SECOND};
    for (size_t i = 0; i < 3; ++i) {
        const Fl_Keyframe *k = fl_scene_key(s, keys[i], NULL); CHECK(k);
        CHECK(k->at.beat == second && fl_anchor_resolve(s, &k->at) == expected[i] && k->value[0] == (double)(i + 1) * 10);
    }
    CHECK(!key(p, scene, entity, FL_PROP_X, (Fl_TimeAnchor){FL_ANCHOR_BEAT, first, false, 6 * SECOND}, 3, FL_EASE_LINEAR, NULL));
    CHECK(!key(p, scene, entity, FL_PROP_X, (Fl_TimeAnchor){FL_ANCHOR_ABSOLUTE, 0, false, 10 * SECOND}, 3, FL_EASE_LINEAR, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = second,
        .name = "Second", .beat_duration = 2 * SECOND}, NULL));
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, keys[0], NULL)->at) == 5 * SECOND + SECOND / 2);
    CHECK(fl_anchor_resolve(s, &fl_scene_key(s, keys[1], NULL)->at) == 7 * SECOND);
    Fl_SceneState evaluated = {0};
    CHECK(fl_scene_state_update(&evaluated, s, 6 * SECOND));
    const Fl_Entity *record = fl_scene_entity(s, entity);
    Fl_Transform transform = evaluated.entities[record - s->entities].transform;
    unsigned char bytes[56]; fl_transform_encode(&transform, bytes);
    Fl_Component component = {FL_COMPONENT_TRANSFORM, 1, 56, bytes};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = scene, .entity = entity,
        .name = record->name, .visible = true, .components = &component, .component_count = 1, .keyed = true, .tick = 6 * SECOND}, NULL));
    CHECK(fl_scene_track(s, entity, FL_PROP_X)->key_count == 1 && fl_scene_track(s, entity, FL_PROP_Y)->key_count == 1);
    transform.x += 1; fl_transform_encode(&transform, bytes);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = scene, .entity = entity,
        .name = fl_scene_entity(s, entity)->name, .visible = true, .components = &component, .component_count = 1,
        .keyed = true, .tick = 6 * SECOND}, NULL));
    CHECK(fl_scene_track(s, entity, FL_PROP_X)->key_count == 2 && fl_scene_track(s, entity, FL_PROP_Y)->key_count == 1);
    fl_scene_state_clear(&evaluated);
    CHECK(fl_project_save(p)); revision = s->revision; fl_project_destroy(p);
    p = fl_project_open(directory, NULL); CHECK(p); if (!p) return;
    s = fl_project_scene(p, scene); CHECK(s->revision == revision && fl_scene_track_count(s) == 3);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_REMOVE, .scene = scene, .beat = second}, NULL));
    CHECK(fl_scene_track_count(s) == 0);
    fl_project_destroy(p);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 1;
    char manifest[FL_PATH_MAX];
    CHECK(fl_fs_mkdir_p(argv[1]) && fl_fs_join(manifest, sizeof(manifest), argv[1], "project.flow")); fl_fs_remove(manifest);
    Fl_Project *p = fl_project_create(argv[1], "Timeline", NULL);
    CHECK(p != NULL); if (!p) return 1;
    const Fl_Scene *s = fl_project_scene_at(p, 0);
    Fl_Id scene = s->id, beat1 = s->beats[0].id, beat2 = 0, box = 0, group = 0, k = 0, k2 = 0;
    CHECK(s->beat_count == 1 && fl_scene_length(s) == 5 * SECOND && !strcmp(s->beats[0].name, "Beat 1"));

    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene, .name = "Two", .position = 1, .beat_duration = 3 * SECOND}, &beat2));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene, .name = "", .position = 0, .beat_duration = SECOND}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = scene, .name = "Zero", .position = 0, .beat_duration = 0}, NULL));
    s = fl_project_scene(p, scene);
    Fl_Tick start, end;
    CHECK(s->beat_count == 2 && fl_scene_length(s) == 8 * SECOND && s->duration == 8 * SECOND);
    CHECK(fl_scene_beat_range(s, beat2, &start, &end) && start == 5 * SECOND && end == 8 * SECOND);
    Fl_TimeAnchor at = fl_anchor_at(s, 6 * SECOND);
    CHECK(at.kind == FL_ANCHOR_BEAT && at.beat == beat2 && at.offset == SECOND && fl_anchor_resolve(s, &at) == 6 * SECOND);

    CHECK(fl_widget_create(p, scene, 0, FL_WIDGET_GROUP, 0, 0, &group));
    CHECK(fl_widget_create(p, scene, group, FL_WIDGET_RECTANGLE, 10, 20, &box));
    Fl_TimeAnchor zero = {FL_ANCHOR_BEAT, beat1, false, 0}, one = {FL_ANCHOR_BEAT, beat1, false, SECOND};
    CHECK(key(p, scene, box, FL_PROP_X, zero, 0, FL_EASE_LINEAR, &k));
    CHECK(key(p, scene, box, FL_PROP_X, one, 100, FL_EASE_LINEAR, &k2));
    CHECK(key(p, scene, box, FL_PROP_X, one, 120, FL_EASE_LINEAR, NULL) && fl_scene_track(fl_project_scene(p, scene), box, FL_PROP_X)->key_count == 2);
    CHECK(key(p, scene, box, FL_PROP_X, one, 100, FL_EASE_LINEAR, NULL));
    CHECK(!key(p, scene, box, FL_PROP_OPACITY, zero, 2, FL_EASE_LINEAR, NULL));
    CHECK(!key(p, scene, 999999, FL_PROP_X, zero, 0, FL_EASE_LINEAR, NULL));
    CHECK(!key(p, scene, box, FL_PROP_X, (Fl_TimeAnchor){FL_ANCHOR_BEAT, 424242, false, 0}, 0, FL_EASE_LINEAR, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = box, .property = FL_PROP_X, .anchor = zero, .ease = FL_EASE_COUNT}, NULL));
    CHECK(fabs(sample(p, scene, box, FL_PROP_X, SECOND / 2) - 50) < 1e-9);
    CHECK(sample(p, scene, box, FL_PROP_X, -SECOND) == 0 && sample(p, scene, box, FL_PROP_X, 3 * SECOND) == 100);
    CHECK(fabs(fl_ease_apply(FL_EASE_IN_OUT, 0.5) - 0.5) < 1e-12 && fl_ease_apply(FL_EASE_IN, 0.5) < 0.5 && fl_ease_apply(FL_EASE_OUT, 0.5) > 0.5);
    CHECK(key(p, scene, box, FL_PROP_X, zero, 0, FL_EASE_HOLD, NULL) && sample(p, scene, box, FL_PROP_X, SECOND - 1) == 0);
    CHECK(key(p, scene, box, FL_PROP_X, zero, 0, FL_EASE_LINEAR, NULL));

    /* Retiming the first beat moves keys anchored to later beats. */
    CHECK(key(p, scene, box, FL_PROP_Y, (Fl_TimeAnchor){FL_ANCHOR_BEAT, beat2, false, SECOND}, 40, FL_EASE_LINEAR, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = beat1, .name = "Intro", .beat_duration = 2 * SECOND}, NULL));
    s = fl_project_scene(p, scene);
    const Fl_Track *y = fl_scene_track(s, box, FL_PROP_Y);
    CHECK(y && fl_anchor_resolve(s, &y->keys[0].at) == 3 * SECOND && fl_scene_length(s) == 5 * SECOND);

    double black[4] = {0, 0, 0, 255}, white[4] = {255, 255, 255, 255}, mid[4];
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = box, .property = FL_PROP_FILL, .anchor = zero,
        .value = {black[0], black[1], black[2], black[3]}}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = box, .property = FL_PROP_FILL, .anchor = one,
        .value = {white[0], white[1], white[2], white[3]}}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(fl_track_sample(s, fl_scene_track(s, box, FL_PROP_FILL), SECOND / 2, mid) && fabs(mid[0] - 187.5) < 1.5 && mid[3] == 255);

    CHECK(key(p, scene, group, FL_PROP_OPACITY, zero, 0.5, FL_EASE_LINEAR, NULL) && key(p, scene, box, FL_PROP_OPACITY, zero, 0.5, FL_EASE_LINEAR, NULL));
    Fl_SceneState state = {0}; Fl_SceneGeometry geometry = {0};
    s = fl_project_scene(p, scene);
    CHECK(fl_scene_state_update(&state, s, SECOND / 2) && fl_scene_geometry_update(&geometry, s, &state));
    const Fl_Entity *e = fl_scene_entity(s, box);
    size_t index = (size_t)(e - s->entities);
    CHECK(fabs(state.entities[index].transform.x - 100) < 1e-9 && state.entities[index].appearance.fill[0] >= 186);
    CHECK(fabs(geometry.entities[index].opacity - 0.25) < 1e-9 && geometry.animated && geometry.tick == SECOND / 2);
    CHECK(fl_scene_geometry_update(&geometry, s, NULL) && geometry.entities[index].local.x == 10 && !geometry.animated);

    /* Keyed edits: animated channels become keys at the tick, others change the base. */
    Fl_Transform moved = state.entities[index].transform; moved.x = 70; moved.y = 33; moved.width = 400;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_TRANSFORMS, .scene = scene, .transforms = &(Fl_TransformEdit){box, moved},
        .transform_count = 1, .keyed = true, .tick = SECOND / 2}, NULL));
    s = fl_project_scene(p, scene); e = fl_scene_entity(s, box);
    Fl_Transform base; fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &base);
    CHECK(base.x == 10 && base.width == 400 && fl_scene_track(s, box, FL_PROP_X)->key_count == 3 && sample(p, scene, box, FL_PROP_X, SECOND / 2) == 70);
    CHECK(fl_scene_track(s, box, FL_PROP_Y)->key_count == 2 && base.y == 20);
    unsigned char alpha[8]; fl_opacity_encode(0.9, alpha);
    Fl_Component opacity = {FL_COMPONENT_OPACITY, 1, 8, alpha};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = scene, .entity = box, .name = "Box", .visible = true,
        .components = &opacity, .component_count = 1, .keyed = true, .tick = 2 * SECOND}, NULL));
    s = fl_project_scene(p, scene); e = fl_scene_entity(s, box);
    CHECK(fl_widget_opacity(e) == 1 && fl_scene_track(s, box, FL_PROP_OPACITY)->key_count == 2 && sample(p, scene, box, FL_PROP_OPACITY, 2 * SECOND) == 0.9);

    /* Clips reference audio assets; referenced assets cannot be removed. */
    Fl_Asset audio = {.name = "Voice", .kind = FL_ASSET_AUDIO, .hash = {7}, .extension = "wav", .size = 10, .duration = 2 * SECOND,
                      .sample_rate = 48000, .channels = 1};
    Fl_Asset picture = {.name = "Pic", .kind = FL_ASSET_IMAGE, .hash = {8}, .extension = "png", .size = 10, .width = 1, .height = 1};
    Fl_Id voice, pic, clip;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &audio}, &voice));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &picture}, &pic));
    Fl_AudioClip record = {.asset = voice, .start = {FL_ANCHOR_BEAT, beat2, false, 0}, .gain = 1, .fade_in = SECOND / 10};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene, .clip_record = &record}, &clip));
    Fl_AudioClip bad = record; bad.asset = pic;
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene, .clip_record = &bad}, NULL));
    bad = record; bad.gain = 9;
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_UPDATE, .scene = scene, .clip = clip, .clip_record = &bad}, NULL));
    CHECK(fl_project_asset_uses(p, voice) == 1 && !fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = voice}, NULL));
    record.gain = 0.5;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_UPDATE, .scene = scene, .clip = clip, .clip_record = &record}, NULL));

    CHECK(fl_project_save(p));
    fl_project_destroy(p);
    p = fl_project_open(argv[1], NULL);
    CHECK(p != NULL); if (!p) return 1;
    s = fl_project_scene(p, scene);
    CHECK(s && s->beat_count == 2 && !strcmp(s->beats[0].name, "Intro") && s->beats[0].duration == 2 * SECOND && fl_scene_length(s) == 5 * SECOND);
    CHECK(fl_scene_track_count(s) == 5 && fl_scene_track(s, box, FL_PROP_X)->key_count == 3 && sample(p, scene, box, FL_PROP_X, SECOND / 2) == 70);
    CHECK(s->clip_count == 1 && s->clips[0].gain == 0.5 && s->clips[0].start.beat == beat2 && s->clips[0].fade_in == SECOND / 10);

    /* Removing a beat removes its keys; the last beat cannot be removed. */
    Fl_Id removed_key = fl_scene_track(s, box, FL_PROP_Y)->keys[0].id;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_REMOVE, .scene = scene, .beat = beat2}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(!fl_scene_key(s, removed_key, NULL));
    CHECK(s->clips[0].start.kind == FL_ANCHOR_ABSOLUTE && fl_scene_length(s) == 2 * SECOND);
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_BEAT_REMOVE, .scene = scene, .beat = beat1}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_SETTINGS, .scene = scene, .width = 1920, .height = 1080, .duration = 4 * SECOND}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(s->beats[0].duration == 4 * SECOND && fl_scene_length(s) == 4 * SECOND);

    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_REMOVE, .scene = scene, .key = k}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_REMOVE, .scene = scene, .key = k}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = scene, .entity = group}, NULL));
    s = fl_project_scene(p, scene);
    CHECK(fl_scene_track_count(s) == 0);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_CLIP_REMOVE, .scene = scene, .clip = clip}, NULL) &&
          fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = voice}, NULL));
    CHECK(fl_project_save(p));
    check_beat_scaling(p, scene);
    check_beat_splitting(p);
    CHECK(fl_project_save(p));
    fl_scene_state_clear(&state); fl_scene_geometry_clear(&geometry);
    fl_project_destroy(p);
    p = fl_project_open(argv[1], NULL); CHECK(p);
    if (p) {
        s = fl_project_scene(p, scene);
        CHECK(s->beats[0].duration == 8 * SECOND && fl_scene_track_at(s, 0)->keys[0].at.offset == 2 * SECOND);
        fl_project_destroy(p);
    }
    check_key_ownership_repair(argv[1]);
    return FLOW_TEST_RESULT("fl_timeline_tests");
}
