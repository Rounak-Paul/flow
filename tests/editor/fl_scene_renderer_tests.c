#include "flow_test.h"
#include "fl_exporter.h"
#include "fl_scene_renderer.h"
#include "components/fl_script.h"
#include "core/fl_event.h"
#include "core/fl_job.h"
#include "media/fl_png.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_process.h"
#include "project/fl_asset_import.h"
#include "components/fl_style.h"
#include "schema/fl_widgets.h"
#include "text/fl_fonts.h"
#include <causality.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SECOND FL_TICKS_PER_SECOND

typedef struct Pixel {int r, g, b;} Pixel;

static Pixel at(const Fl_Frame *f, uint32_t x, uint32_t y)
{
    const unsigned char *p = f->rgba + ((size_t)y * f->width + x) * 4;
    return (Pixel){p[0], p[1], p[2]};
}

static bool near(Pixel a, int r, int g, int b, int tolerance)
{
    return abs(a.r - r) <= tolerance && abs(a.g - g) <= tolerance && abs(a.b - b) <= tolerance;
}

/* Create a primitive of the given size and colour through the same configure path the Inspector uses. */
static Fl_Id make(Fl_Project *p, Fl_Id scene, Fl_WidgetKind kind, double x, double y, double w, double h, const unsigned char fill[4])
{
    Fl_Id id = 0;
    CHECK(fl_widget_create(p, scene, 0, kind, x, y, &id));
    const Fl_Entity *e = fl_scene_entity(fl_project_scene(p, scene), id);
    Fl_Transform t; CHECK(fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &t));
    t.width = w; t.height = h;
    Fl_Appearance a = fl_widget_appearance(e);
    memcpy(a.fill, fill, 4); memcpy(a.stroke, fill, 4);
    unsigned char transform[56], style[24];
    fl_transform_encode(&t, transform); fl_appearance_encode(&a, style);
    Fl_Component components[2] = {{FL_COMPONENT_TRANSFORM, 1, 56, transform}, {FL_COMPONENT_APPEARANCE, 1, 24, style}};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = scene, .entity = id, .name = e->name, .visible = true,
        .components = components, .component_count = 2}, NULL));
    return id;
}

/* Exercises the offscreen render path on a real GPU through a windowless instance; reports a skip without a Vulkan device. */
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char dir[FL_PATH_MAX], manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), argv[1], "renderer") && fl_fs_mkdir_p(dir) && fl_fs_join(manifest, sizeof(manifest), dir, "project.flow"));
    fl_fs_remove(manifest);
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){0});
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.bus = bus});
    Fl_Fonts *fonts = fl_fonts_create(&(Fl_FontsDesc){.bus = bus});
    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){.app_name = "flow-renderer-test"});
    Fl_Project *p = fl_project_create(dir, "Renderer", NULL);
    CHECK(bus && jobs && fonts && p);
    if (!instance) {printf("fl_scene_renderer_tests: skipped (no Vulkan device)\n"); return 0;}
    if (!bus || !jobs || !fonts || !p) return 1;
    Fl_SceneRenderer *renderer = fl_scene_renderer_create(&(Fl_SceneRendererDesc){.instance = instance, .fonts = fonts, .jobs = jobs});
    CHECK(renderer != NULL);
    if (!renderer) return 1;
    Fl_Id scene_id = fl_project_scene_at(p, 0)->id;
    const unsigned char red[4] = {255, 0, 0, 255}, white[4] = {255, 255, 255, 255}, green[4] = {0, 255, 0, 255};

    /* Empty scene: the backdrop at the requested size, including the very edge pixels. */
    Fl_Frame frame;
    const Fl_Scene *scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    CHECK(frame.width == 960 && frame.height == 540 && near(at(&frame, 0, 0), 25, 31, 47, 1) && near(at(&frame, 959, 539), 25, 31, 47, 1));
    fl_frame_free(&frame);

    /* Shapes land where the scene says, at output scale. */
    Fl_Id box = make(p, scene_id, FL_WIDGET_RECTANGLE, 200, 100, 400, 200, red);
    make(p, scene_id, FL_WIDGET_ELLIPSE, 1000, 500, 400, 400, green);
    Fl_Id line = make(p, scene_id, FL_WIDGET_LINE, 200, 700, 800, 40, white);
    make(p, scene_id, FL_WIDGET_TRIANGLE, 1450, 700, 200, 200, red);
    make(p, scene_id, FL_WIDGET_DIAMOND, 1700, 700, 200, 200, red);
    make(p, scene_id, FL_WIDGET_STAR, 200, 850, 200, 200, red);
    make(p, scene_id, FL_WIDGET_HEXAGON, 600, 800, 260, 225, red);
    scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    CHECK(near(at(&frame, 200, 100), 255, 0, 0, 1) && near(at(&frame, 98, 48), 25, 31, 47, 1) && near(at(&frame, 303, 153), 25, 31, 47, 1));
    CHECK(near(at(&frame, 600, 350), 0, 255, 0, 1) && near(at(&frame, 508, 258), 25, 31, 47, 1));
    CHECK(near(at(&frame, 300, 360), 255, 255, 255, 1));
    CHECK(near(at(&frame, 775, 440), 255, 0, 0, 1) && near(at(&frame, 730, 355), 25, 31, 47, 1) && near(at(&frame, 735, 365), 25, 31, 47, 1));
    CHECK(near(at(&frame, 900, 400), 255, 0, 0, 1) && near(at(&frame, 857, 358), 25, 31, 47, 1));
    CHECK(near(at(&frame, 150, 480), 255, 0, 0, 1) && near(at(&frame, 105, 430), 25, 31, 47, 1));
    CHECK(near(at(&frame, 365, 456), 255, 0, 0, 1) && near(at(&frame, 305, 405), 25, 31, 47, 1));
    Fl_Frame again;
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &again) && !memcmp(frame.rgba, again.rgba, (size_t)frame.width * frame.height * 4));
    fl_frame_free(&again);
    Fl_Frame full;
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 1920, &full) && full.height == 1080 && near(at(&full, 400, 200), 255, 0, 0, 1));
    fl_frame_free(&full);
    fl_frame_free(&frame);

    /* Half-opaque red blends in linear light over the backdrop: sRGB red lands near 188. */
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = box,
        .component = {FL_COMPONENT_OPACITY, 1, 8, (unsigned char[8]){0, 0, 0, 0, 0, 0, 0xe0, 0x3f}}}, NULL));
    scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    Pixel half = at(&frame, 200, 100);
    CHECK(half.r > 150 && half.r < 215 && half.g < 60 && half.b < 80);
    fl_frame_free(&frame);

    /* Keyframes drive the rendered position. */
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene_id, .entity = line, .property = FL_PROP_X, .anchor = {.offset = 0}, .value = {200}}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene_id, .entity = line, .property = FL_PROP_X, .anchor = {.offset = 2 * SECOND}, .value = {900}}, NULL));
    scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    CHECK(near(at(&frame, 120, 360), 255, 255, 255, 1) && near(at(&frame, 50, 360), 25, 31, 47, 1));
    fl_frame_free(&frame);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 2 * SECOND, 960, &frame));
    CHECK(near(at(&frame, 120, 360), 25, 31, 47, 1) && near(at(&frame, 460, 360), 255, 255, 255, 1));
    fl_frame_free(&frame);

    /* Lua scripts apply to the frame. */
    const char *source = "function on_evaluate(self, t) self.transform.x = 1500 end";
    Fl_Component script = {0};
    CHECK(fl_script_encode(&(Fl_Script){source, strlen(source), true}, &script));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = box, .component = script}, NULL));
    free((void *)script.data);
    scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    CHECK(near(at(&frame, 200, 100), 25, 31, 47, 1) && at(&frame, 800, 100).r > 150);
    fl_frame_free(&frame);

    /* Text leaves ink only inside its box, rasterized at output scale. */
    make(p, scene_id, FL_WIDGET_TEXT, 100, 800, 900, 200, white);
    scene = fl_project_scene(p, scene_id);
    for (uint32_t width = 960; width <= 1920; width += 960) {
        CHECK(fl_scene_renderer_render(renderer, p, scene, 0, width, &frame));
        double k = width / 1920.0;
        size_t inside = 0, outside = 0;
        for (uint32_t y = (uint32_t)(760 * k); y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
            Pixel px = at(&frame, x, y);
            if (px.r > 100 && px.g > 100 && px.b > 100) {
                bool in_box = x >= 100 * k - 1 && x < 1000 * k + 1 && y >= 800 * k - 1 && y < 1000 * k + 1;
                inside += in_box; outside += !in_box;
            }
        }
        CHECK(inside > 200 && outside == 0);
        fl_frame_free(&frame);
    }

    /* Images sample the asset; the placeholder shows only when the asset is unusable. */
    char png_path[FL_PATH_MAX];
    CHECK(fl_fs_join(png_path, sizeof(png_path), argv[1], "swatch.png"));
    uint8_t swatch[4 * 4 * 4];
    for (size_t i = 0; i < 16; ++i) {swatch[i * 4] = 10; swatch[i * 4 + 1] = 200; swatch[i * 4 + 2] = 120; swatch[i * 4 + 3] = 255;}
    size_t png_size = 0;
    unsigned char *png = fl_png_encode_region(swatch, 4, 4, 0, 0, 4, 4, 0, &png_size, NULL, NULL);
    CHECK(png && fl_fs_write_atomic(png_path, png, png_size)); free(png);
    Fl_StoredAsset stored;
    CHECK(fl_asset_store_file(dir, png_path, &stored) == FL_IMPORT_ADDED);
    Fl_Asset record = {.name = "swatch", .kind = stored.kind, .extension = stored.extension, .size = stored.size, .width = stored.width, .height = stored.height};
    memcpy(record.hash, stored.hash, 32);
    Fl_Id asset = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &asset));
    CHECK(fl_widget_create_image(p, scene_id, 0, asset, 1300, 100, 400, 300, NULL));
    scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    CHECK(near(at(&frame, 750, 150), 10, 200, 120, 2));
    fl_frame_free(&frame);

    /* Clip cuts children to the box (rounded corners included); blur spreads an entity's light beyond its edge. */
    Fl_Id effects_scene = 0, clipper = 0, overflow = 0, glow = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_ADD, .name = "Effects"}, &effects_scene));
    CHECK(fl_widget_create(p, effects_scene, 0, FL_WIDGET_GROUP, 400, 300, &clipper));
    CHECK(fl_widget_create(p, effects_scene, clipper, FL_WIDGET_RECTANGLE, -100, -100, &overflow));
    CHECK(fl_widget_create(p, effects_scene, 0, FL_WIDGET_RECTANGLE, 1200, 300, &glow));
    {
        Fl_Id ids[3] = {clipper, overflow, glow};
        double sizes[3][2] = {{400, 300}, {800, 600}, {200, 200}};
        for (int i = 0; i < 3; ++i) {
            const Fl_Entity *e = fl_scene_entity(fl_project_scene(p, effects_scene), ids[i]);
            Fl_Transform t; CHECK(fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &t));
            t.width = sizes[i][0]; t.height = sizes[i][1];
            unsigned char transform[56], style_bytes[FL_STYLE_BYTES], appearance[24];
            fl_transform_encode(&t, transform);
            Fl_Appearance a = fl_widget_appearance(e);
            memcpy(a.fill, i == 1 ? red : white, 4); a.stroke_width = 0; fl_appearance_encode(&a, appearance);
            Fl_Style look = fl_style_default();
            if (i == 0) {look.clip = true; look.radius = 60;}
            if (i == 2) look.blur = 20;
            fl_style_encode(&look, style_bytes);
            Fl_Component components[3] = {{FL_COMPONENT_TRANSFORM, 1, 56, transform}, {FL_COMPONENT_STYLE, 1, FL_STYLE_BYTES, style_bytes}, {FL_COMPONENT_APPEARANCE, 1, 24, appearance}};
            CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = effects_scene, .entity = ids[i], .name = e->name, .visible = true,
                .components = components, .component_count = i == 0 ? 2 : 3}, NULL));
        }
    }
    scene = fl_project_scene(p, effects_scene);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    CHECK(near(at(&frame, 300, 225), 255, 0, 0, 1) && near(at(&frame, 180, 225), 25, 31, 47, 1) && near(at(&frame, 300, 100), 25, 31, 47, 1));
    CHECK(near(at(&frame, 204, 154), 25, 31, 47, 3) && near(at(&frame, 212, 160), 255, 0, 0, 1));
    CHECK(near(at(&frame, 650, 200), 255, 255, 255, 12) && at(&frame, 596, 200).r > 70 && at(&frame, 596, 200).r < 200 && near(at(&frame, 540, 200), 25, 31, 47, 6));
    fl_frame_free(&frame);

    /* Target aspect frames the output; invalid requests fail without drawing. */
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = scene_id, .aspect_width = 1, .aspect_height = 1}, NULL));
    scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 500, &frame) && frame.width == 500 && frame.height == 500);
    fl_frame_free(&frame);
    CHECK(!fl_scene_renderer_render(renderer, p, scene, -1, 500, &frame) && !fl_scene_renderer_render(renderer, p, scene, 0, 16384, &frame));

    /* Export pipes every frame to ffmpeg and finishes on its own; invalid requests and cancellation leave no file behind. */
    if (fl_ffmpeg_path()) {
        Fl_Exporter *exporter = fl_exporter_create(bus, renderer);
        char output[FL_PATH_MAX], error[128];
        const Fl_Id first_scene = scene_id;
        CHECK(exporter && fl_fs_join(output, sizeof(output), argv[1], "export.mp4"));
        fl_fs_remove(output);
        CHECK(!fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 321, .fps = 10, .scenes = &first_scene, .scene_count = 1}, error, sizeof(error)) && strstr(error, "even"));
        CHECK(!fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 0, .scenes = &first_scene, .scene_count = 1}, error, sizeof(error)));
        CHECK(fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = &first_scene, .scene_count = 1}, error, sizeof(error)));
        CHECK(!fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = &first_scene, .scene_count = 1}, error, sizeof(error)) && strstr(error, "already"));
        while (fl_exporter_status(exporter).state == FL_EXPORT_RUNNING) fl_event_bus_dispatch(bus);
        Fl_ExportStatus status = fl_exporter_status(exporter);
        CHECK(status.state == FL_EXPORT_DONE && status.frames_done == 50 && status.frames_total == 50 && status.width == 320 && status.height == 320);
        CHECK(fl_fs_file_size(output) > 1000);
        fl_fs_remove(output);
        CHECK(fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = &first_scene, .scene_count = 1}, error, sizeof(error)));
        fl_exporter_cancel(exporter);
        CHECK(fl_exporter_status(exporter).state == FL_EXPORT_CANCELLED && !fl_fs_exists(output));
        const Fl_Id both[2] = {scene_id, effects_scene};
        CHECK(!fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = both, .scene_count = 2}, error, sizeof(error)) && strstr(error, "aspect"));
        CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = effects_scene, .aspect_width = 1, .aspect_height = 1}, NULL));
        CHECK(!fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = both, .scene_count = 2, .crossfade = 5}, error, sizeof(error)) && strstr(error, "shorter"));
        CHECK(fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = both, .scene_count = 2, .crossfade = 1}, error, sizeof(error)));
        while (fl_exporter_status(exporter).state == FL_EXPORT_RUNNING) fl_event_bus_dispatch(bus);
        status = fl_exporter_status(exporter);
        CHECK(status.state == FL_EXPORT_DONE && status.frames_done == 90 && status.frames_total == 90);
        fl_fs_remove(output);
        fl_exporter_destroy(exporter);
    }

    fl_scene_renderer_destroy(renderer);
    fl_project_destroy(p);
    fl_fonts_destroy(fonts);
    fl_jobs_destroy(jobs);
    fl_event_bus_destroy(bus);
    ca_instance_destroy(instance);
    return FLOW_TEST_RESULT("fl_scene_renderer_tests");
}
