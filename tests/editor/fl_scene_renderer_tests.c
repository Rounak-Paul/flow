#include "fl_render_host.h"
#include "flow_test.h"
#include "render/gpu/fl_exporter.h"
#include "render/gpu/fl_scene_renderer.h"
#include "components/fl_script.h"
#include "core/fl_event.h"
#include "core/fl_clock.h"
#include "core/fl_job.h"
#include "media/fl_png.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_process.h"
#include "project/fl_asset_import.h"
#include "project/fl_prototype_authoring.h"
#include "project/fl_prototype.h"
#include "media/fl_audio.h"
#include "project/fl_resource_snapshot.h"
#include "components/fl_style.h"
#include "schema/fl_widgets.h"
#include "components/fl_spatial.h"
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
    Fl_Pose t; CHECK(fl_pose_decode(fl_entity_component(e, FL_COMPONENT_POSE), &t));
    Fl_Bounds box = {w, h, 0};
    Fl_Appearance a = fl_widget_appearance(e);
    memcpy(a.fill, fill, 4); memcpy(a.stroke, fill, 4);
    unsigned char transform[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], style[24];
    fl_pose_encode(&t, transform); fl_bounds_encode(&box, bounds_bytes); fl_appearance_encode(&a, style);
    Fl_Component components[3] = {{.type=FL_COMPONENT_POSE, .version=1, .size=FL_POSE_BYTES, .data=transform}, {.type=FL_COMPONENT_BOUNDS, .version=1, .size=FL_BOUNDS_BYTES, .data=bounds_bytes}, {.type=FL_COMPONENT_APPEARANCE, .version=1, .size=24, .data=style}};
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = scene, .entity = id, .name = e->name, .visible = true,
        .components = components, .component_count = 3}, NULL));
    return id;
}

/* The picture ratio belongs to a camera. */
static void picture_aspect(Fl_Project *p, Fl_Id scene, unsigned w, unsigned h)
{
    Fl_Id id = 0;
    CHECK(fl_spatial_create(p, scene, 0, "camera", 0, 0, 0, &id));
    const Fl_Component *component = fl_entity_component(fl_scene_entity(fl_project_scene(p, scene), id), FL_COMPONENT_CAMERA);
    Fl_Camera camera;
    CHECK(component && fl_camera_decode(component, &camera));
    camera.aspect_w = w; camera.aspect_h = h;
    unsigned char bytes[FL_CAMERA_BYTES];
    fl_camera_encode(&camera, bytes);
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene, .entity = id,
        .component = {.type = FL_COMPONENT_CAMERA, .version = 1, .size = FL_CAMERA_BYTES, .data = bytes, .id = component->id}}, NULL));
}

/* Verify generated video pixels and prototype audio through the captured-resource export pipeline. */
static void test_media(Fl_SceneRenderer *renderer, Fl_Project *project, Fl_EventBus *bus, const char *directory)
{
    const char *ffmpeg=fl_ffmpeg_path(); if (!ffmpeg) return;
    char video[FL_PATH_MAX],audio[FL_PATH_MAX],output[FL_PATH_MAX],decoded[FL_PATH_MAX],command[4*FL_PATH_MAX]="",error[256];
    CHECK(fl_fs_join(video,sizeof(video),directory,"media.mp4") && fl_fs_join(audio,sizeof(audio),directory,"tone.wav") &&
        fl_fs_join(output,sizeof(output),directory,"media-export.mp4") && fl_fs_join(decoded,sizeof(decoded),directory,"export-audio.wav"));
    const char *video_args[]={ffmpeg,"-v","error","-y","-f","lavfi","-i","color=c=red:s=64x64:r=5:d=1", "-f","lavfi","-i","color=c=blue:s=64x64:r=7:d=1",
        "-filter_complex","[0:v][1:v]concat=n=2:v=1:a=0[v]","-map","[v]","-fps_mode","vfr","-c:v","libx264","-pix_fmt","yuv420p",video};
    bool ok=true;
    for (size_t i=0;ok && i<sizeof(video_args)/sizeof(*video_args);++i) ok=fl_command_append(command,sizeof(command),video_args[i]);
    CHECK(ok && fl_command_run(command)); command[0]=0;
    const char *audio_args[]={ffmpeg,"-v","error","-y","-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=0.25",audio};
    for (size_t i=0;ok && i<sizeof(audio_args)/sizeof(*audio_args);++i) ok=fl_command_append(command,sizeof(command),audio_args[i]);
    CHECK(ok && fl_command_run(command));
    Fl_StoredAsset stored={0}; Fl_Id video_id=0,audio_id=0;
    CHECK(fl_asset_store_file(fl_project_directory(project),video,&stored)==FL_IMPORT_ADDED && fl_asset_record(project,&stored,"VFR",&video_id)==FL_IMPORT_ADDED);
    CHECK(fl_asset_store_file(fl_project_directory(project),audio,&stored)==FL_IMPORT_ADDED && fl_asset_record(project,&stored,"Tone",&audio_id)==FL_IMPORT_ADDED);
    Fl_Id source_scene=0,destination=0,entity=0,prototype=0,owner=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_ADD,.name="Media source"},&source_scene));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_SETTINGS,.scene=source_scene,.width=100,.height=100,.duration=2*FL_TICKS_PER_SECOND},NULL));
    CHECK(fl_widget_create_video(project,source_scene,0,video_id,0,0,40,40,&entity));
    CHECK(fl_prototype_capture(project,source_scene,entity,"Media prototype",&prototype));
    const Fl_Asset *asset=fl_project_asset(project,prototype); Fl_Prototype source={0}; Fl_Json json={0};
    CHECK(asset && fl_prototype_read(&json,asset->source,(size_t)asset->size,&source));
    Fl_AudioClip *clip=calloc(1,sizeof(*clip)); CHECK(clip);
    if (clip) {
        *clip=(Fl_AudioClip){.id=source.next_id++,.asset=audio_id,.duration=FL_TICKS_PER_SECOND/5,.gain=1,.fade_in=FL_TICKS_PER_SECOND/50,.fade_out=FL_TICKS_PER_SECOND/50};
        source.scope.clips=clip; source.scope.clip_count=1;
    }
    cJSON *document=fl_prototype_write(&json,&source); char *text=document ? cJSON_PrintUnformatted(document) : NULL; cJSON_Delete(document);
    CHECK(text && fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_PROTOTYPE,.asset=prototype,.source=text,.source_size=strlen(text)},NULL));
    free(text); fl_prototype_clear(&source);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_ADD,.name="Media composition"},&destination));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_SETTINGS,.scene=destination,.width=100,.height=100,.duration=2*FL_TICKS_PER_SECOND},NULL));
    CHECK(fl_prototype_instantiate(project,destination,0,prototype,0,0,&owner));
    Fl_Id hidden=0;
    CHECK(fl_widget_create(project,destination,0,FL_WIDGET_GROUP,0,0,&hidden));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_VISIBLE,.scene=destination,.entity=hidden,.visible=false},NULL));
    for (size_t i=0;i<17;++i) CHECK(fl_widget_create_video(project,destination,hidden,video_id,0,0,20,20,NULL));
    Fl_Frame frame={0};
    CHECK(fl_scene_renderer_render(renderer,project,fl_project_scene(project,destination),FL_TICKS_PER_SECOND*2/5,100,&frame));
    if (frame.rgba) {Pixel pixel=at(&frame,10,10); CHECK(pixel.r>230 && pixel.b<45);} fl_frame_free(&frame);
    CHECK(fl_scene_renderer_render(renderer,project,fl_project_scene(project,destination),FL_TICKS_PER_SECOND*7/5,100,&frame));
    if (frame.rgba) {Pixel pixel=at(&frame,10,10); CHECK(pixel.b>230 && pixel.r<45);} fl_frame_free(&frame);
    Fl_Exporter *exporter=fl_exporter_create(bus,renderer); CHECK(exporter);
    CHECK(fl_exporter_start(exporter,project,&(Fl_ExportDesc){.path=output,.width=100,.fps=4,.scenes=&destination,.scene_count=1},error,sizeof(error)));
    uint64_t deadline=fl_clock_monotonic_ns()+UINT64_C(30000000000);
    while (fl_exporter_status(exporter).state==FL_EXPORT_RUNNING && fl_clock_monotonic_ns()<deadline) fl_event_bus_dispatch(bus);
    CHECK(fl_exporter_status(exporter).state==FL_EXPORT_DONE && fl_exporter_status(exporter).frames_done==8);
    fl_exporter_destroy(exporter); command[0]=0;
    const char *decode_args[]={ffmpeg,"-v","error","-y","-i",output,"-map","0:a","-t","0.1",decoded};
    for (size_t i=0;ok && i<sizeof(decode_args)/sizeof(*decode_args);++i) ok=fl_command_append(command,sizeof(command),decode_args[i]);
    CHECK(ok && fl_command_run(command));
    Fl_AudioInfo info={0}; Fl_AudioReader *reader=fl_audio_open(decoded,&info); CHECK(reader);
    if (reader) {float samples[1024]; CHECK(info.channels<=2 && fl_audio_read(reader,samples,256)==256); fl_audio_close(reader);}
}

/* Exercises the offscreen render path on a real GPU through a windowless instance; reports a skip without a Vulkan device. */
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char dir[FL_PATH_MAX], manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), argv[1], "renderer") && fl_fs_mkdir_p(dir) && fl_fs_join(manifest, sizeof(manifest), dir, "project.flow.json"));
    fl_fs_remove(manifest);
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){0});
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.bus = bus});
    Fl_Fonts *fonts = fl_fonts_create(&(Fl_FontsDesc){.bus = bus});
    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){.app_name = "flow-renderer-test"});
    Fl_Project *p = fl_project_create(dir, "Renderer", NULL);
    CHECK(bus && jobs && fonts && p);
    if (!instance) {printf("fl_scene_renderer_tests: skipped (no Vulkan device)\n"); return 0;}
    if (!bus || !jobs || !fonts || !p) return 1;
    Fl_SceneRenderer *renderer = fl_scene_renderer_create(&(Fl_SceneRendererDesc){.gpu=fl_render_host_context(instance), .fonts = fonts, .jobs = jobs});
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
        .component = {.type=FL_COMPONENT_OPACITY,.version=1,.size=8,.data=(unsigned char[8]){0, 0, 0, 0, 0, 0, 0xe0, 0x3f}}}, NULL));
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
    const char *source = "return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.pose.x = 1500 end }";
    Fl_Component script = {0};
    Fl_Id script_id = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .scene = scene_id, .name = "Jump", .source = source, .source_size = strlen(source)}, &script_id));
    CHECK(fl_script_encode(&(Fl_Script){.script=script_id,.enabled=true}, &script));
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

    /* A fresh captured renderer must decode owned bytes after the original store file disappears. */
    char capture_error[256],original_media[FL_PATH_MAX];
    Fl_ResourceSnapshot *resources=fl_resource_snapshot_create(p,jobs,&scene_id,1,capture_error,sizeof(capture_error));
    CHECK(resources);
    uint64_t capture_deadline=fl_clock_monotonic_ns()+UINT64_C(10000000000);
    while (fl_resource_snapshot_status(resources)==FL_RESOURCES_PENDING && fl_clock_monotonic_ns()<capture_deadline) fl_event_bus_dispatch(bus);
    CHECK(fl_resource_snapshot_status(resources)==FL_RESOURCES_READY);
    Fl_Project *captured=fl_resource_snapshot_project(resources);
    Fl_SceneRenderer *captured_renderer=fl_scene_renderer_snapshot(renderer,captured,&scene_id,1,capture_error,sizeof(capture_error));
    CHECK(captured_renderer && fl_project_asset_path(p,fl_project_asset(p,asset),original_media,sizeof(original_media)));
    CHECK(fl_fs_remove(original_media));
    CHECK(fl_scene_renderer_render(captured_renderer,captured,fl_project_scene(captured,scene_id),0,960,&frame));
    if (frame.rgba) CHECK(near(at(&frame,750,150),10,200,120,2));
    fl_frame_free(&frame);
    size_t restore_size=0; void *restore=fl_fs_read_file(png_path,1024*1024,&restore_size);
    CHECK(restore && fl_fs_write_atomic(original_media,restore,restore_size)); free(restore);
    fl_scene_renderer_destroy(captured_renderer); fl_resource_snapshot_destroy(resources);

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
            Fl_Pose t; CHECK(fl_pose_decode(fl_entity_component(e, FL_COMPONENT_POSE), &t));
            Fl_Bounds box = {sizes[i][0], sizes[i][1], 0};
            unsigned char transform[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], style_bytes[FL_STYLE_BYTES], appearance[24];
            fl_pose_encode(&t, transform); fl_bounds_encode(&box, bounds_bytes);
            Fl_Appearance a = fl_widget_appearance(e);
            memcpy(a.fill, i == 1 ? red : white, 4); a.stroke_width = 0; fl_appearance_encode(&a, appearance);
            Fl_Style look = fl_style_default();
            if (i == 0) {look.clip = true; look.radius = 60;}
            if (i == 2) look.blur = 20;
            fl_style_encode(&look, style_bytes);
            Fl_Component components[4] = {{.type=FL_COMPONENT_POSE, .version=1, .size=FL_POSE_BYTES, .data=transform}, {.type=FL_COMPONENT_BOUNDS, .version=1, .size=FL_BOUNDS_BYTES, .data=bounds_bytes}, {.type=FL_COMPONENT_STYLE, .version=1, .size=FL_STYLE_BYTES, .data=style_bytes}, {.type=FL_COMPONENT_APPEARANCE, .version=1, .size=24, .data=appearance}};
            CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = effects_scene, .entity = ids[i], .name = e->name, .visible = true,
                .components = components, .component_count = i == 0 ? 3 : 4}, NULL));
        }
    }
    scene = fl_project_scene(p, effects_scene);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 960, &frame));
    CHECK(near(at(&frame, 300, 225), 255, 0, 0, 1) && near(at(&frame, 180, 225), 25, 31, 47, 1) && near(at(&frame, 300, 100), 25, 31, 47, 1));
    CHECK(near(at(&frame, 204, 154), 25, 31, 47, 3) && near(at(&frame, 212, 160), 255, 0, 0, 1));
    CHECK(near(at(&frame, 650, 200), 255, 255, 255, 12) && at(&frame, 596, 200).r > 70 && at(&frame, 596, 200).r < 200 && near(at(&frame, 540, 200), 25, 31, 47, 6));
    fl_frame_free(&frame);

    /* A camera's aspect frames the output; invalid requests fail without drawing. */
    picture_aspect(p, scene_id, 1, 1);
    scene = fl_project_scene(p, scene_id);
    CHECK(fl_scene_renderer_render(renderer, p, scene, 0, 500, &frame) && frame.width == 500 && frame.height == 500);
    fl_frame_free(&frame);
    CHECK(!fl_scene_renderer_render(renderer, p, scene, -1, 500, &frame) && !fl_scene_renderer_render(renderer, p, scene, 0, 16384, &frame));

    /* Export commits complete output atomically, retains the captured document and preserves prior output on cancellation. */
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
        Fl_ExportPreview preview = {0};
        CHECK(status.state == FL_EXPORT_DONE && status.frames_done == 50 && status.frames_total == 50 && status.width == 320 && status.height == 320);
        CHECK(status.crf == 16 && status.crossfade == 0 && status.scene_count == 1);
        CHECK(fl_exporter_preview(exporter, &preview) && preview.rgba && preview.width == 320 && preview.height == 320 && preview.sequence);
        CHECK(fl_fs_file_size(output) > 1000);
        int64_t completed_size=fl_fs_file_size(output);
        CHECK(fl_exporter_start(exporter,p,&(Fl_ExportDesc){.path=output,.width=320,.fps=10,.scenes=&first_scene,.scene_count=1},error,sizeof(error)));
        fl_exporter_cancel(exporter);
        CHECK(fl_exporter_status(exporter).state==FL_EXPORT_CANCELLED && fl_fs_file_size(output)==completed_size);
        CHECK(fl_exporter_start(exporter,p,&(Fl_ExportDesc){.path=output,.width=320,.fps=10,.scenes=&first_scene,.scene_count=1},error,sizeof(error)));
        fl_exporter_abort(exporter,"test failure");
        CHECK(fl_exporter_status(exporter).state==FL_EXPORT_FAILED && fl_fs_file_size(output)==completed_size);
        CHECK(fl_exporter_start(exporter,p,&(Fl_ExportDesc){.path=output,.width=320,.fps=10,.scenes=&first_scene,.scene_count=1},error,sizeof(error)));
        CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_ASPECT,.scene=first_scene,.aspect_width=2,.aspect_height=1},NULL));
        while (fl_exporter_status(exporter).state==FL_EXPORT_RUNNING) fl_event_bus_dispatch(bus);
        CHECK(fl_exporter_status(exporter).state==FL_EXPORT_DONE && fl_exporter_status(exporter).width==320 && fl_exporter_status(exporter).height==320);
        CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_ASPECT,.scene=first_scene,.aspect_width=1,.aspect_height=1},NULL));
        fl_fs_remove(output);
        CHECK(fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = &first_scene, .scene_count = 1}, error, sizeof(error)));
        fl_exporter_cancel(exporter);
        CHECK(fl_exporter_status(exporter).state == FL_EXPORT_CANCELLED && !fl_fs_exists(output));
        const Fl_Id both[2] = {scene_id, effects_scene};
        CHECK(!fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = both, .scene_count = 2}, error, sizeof(error)) && strstr(error, "aspect"));
        picture_aspect(p, effects_scene, 1, 1);
        CHECK(!fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = both, .scene_count = 2, .crossfade = 5}, error, sizeof(error)) && strstr(error, "shorter"));
        CHECK(fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path = output, .width = 320, .fps = 10, .scenes = both, .scene_count = 2, .crossfade = 1}, error, sizeof(error)));
        while (fl_exporter_status(exporter).state == FL_EXPORT_RUNNING) fl_event_bus_dispatch(bus);
        status = fl_exporter_status(exporter);
        CHECK(status.state == FL_EXPORT_DONE && status.frames_done == 90 && status.frames_total == 90);
        CHECK(status.crf == 16 && status.crossfade == 1 && status.scene_count == 2);
        CHECK(fl_exporter_preview(exporter, &preview) && preview.width == 320 && preview.height == 320 && preview.sequence);
        fl_fs_remove(output);
        CHECK(fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path=output,.width=320,.fps=10,.scenes=&first_scene,.scene_count=1}, error, sizeof(error)));
        fl_exporter_destroy(exporter);
        exporter = fl_exporter_create(bus, renderer);
        CHECK(exporter);
        CHECK(!fl_exporter_preview(exporter, &preview));
        fl_event_bus_dispatch(bus);
        CHECK(fl_exporter_status(exporter).state == FL_EXPORT_IDLE);
        CHECK(fl_exporter_start(exporter, p, &(Fl_ExportDesc){.path=output,.width=320,.fps=10,.scenes=&first_scene,.scene_count=1}, error, sizeof(error)));
        while (fl_exporter_status(exporter).state == FL_EXPORT_RUNNING) fl_event_bus_dispatch(bus);
        CHECK(fl_exporter_status(exporter).state == FL_EXPORT_DONE);
        fl_fs_remove(output);
        fl_exporter_destroy(exporter);
    }

    /* Exact rendering reuses simulation checkpoints and rejects callback errors instead of exporting base values. */
    Fl_Id simulated_scene=0, simulated_asset=0;
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_ADD,.name="Simulation"},&simulated_scene));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_SETTINGS,.scene=simulated_scene,.width=100,.height=100,.duration=5*FL_TICKS_PER_SECOND},NULL));
    Fl_Id simulated_entity=make(p,simulated_scene,FL_WIDGET_RECTANGLE,0,0,20,20,red);
    const char *simulation_source="return {step_hz=60,initialize=function(ctx) ctx.state={x=0} end,step=function(ctx) ctx.state.x=ctx.state.x+10*ctx.time.dt end,sample=function(ctx) ctx.entity.pose.x=ctx.state.x end}";
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.name="GPU simulation",.source=simulation_source,.source_size=strlen(simulation_source)},&simulated_asset));
    Fl_Component behavior={0}; CHECK(fl_script_encode(&(Fl_Script){.script=simulated_asset,.enabled=true},&behavior));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=simulated_scene,.entity=simulated_entity,.component=behavior},NULL));
    fl_component_clear(&behavior);
    const Fl_Scene *simulation=fl_project_scene(p,simulated_scene);
    Fl_Frame expected={0}, repeated={0};
    CHECK(fl_scene_renderer_render(renderer,p,simulation,FL_TICKS_PER_SECOND,100,&expected));
    CHECK(fl_scene_renderer_render(renderer,p,simulation,3*FL_TICKS_PER_SECOND,100,&repeated)); fl_frame_free(&repeated);
    CHECK(fl_scene_renderer_render(renderer,p,simulation,FL_TICKS_PER_SECOND,100,&repeated));
    CHECK(expected.rgba && repeated.rgba && !memcmp(expected.rgba,repeated.rgba,100*100*4));
    fl_frame_free(&expected); fl_frame_free(&repeated);
    const char *broken="return {sample=function(ctx) error('frame failure') end}";
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.asset =simulated_asset,.source=broken,.source_size=strlen(broken)},NULL));
    CHECK(!fl_scene_renderer_render(renderer,p,simulation,FL_TICKS_PER_SECOND,100,&repeated));
    CHECK(strstr(fl_scene_renderer_error(renderer),"frame failure"));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA,.asset =simulated_asset,.source=simulation_source,.source_size=strlen(simulation_source)},NULL));
    CHECK(fl_scene_renderer_render(renderer,p,simulation,FL_TICKS_PER_SECOND,100,&repeated) && !fl_scene_renderer_error(renderer)[0]);
    fl_frame_free(&repeated);

    /* Live composition renders generated behavior at scoped time through the same exact GPU path. */
    Fl_Id prototype=0,composition_scene=0,instance_entity=0;
    CHECK(fl_prototype_capture(p,simulated_scene,simulated_entity,"Rendered prototype",&prototype));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_ADD,.name="Composition"},&composition_scene));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_SETTINGS,.scene=composition_scene,.width=100,.height=100,.duration=5*FL_TICKS_PER_SECOND},NULL));
    CHECK(fl_prototype_instantiate(p,composition_scene,0,prototype,40,0,&instance_entity));
    CHECK(fl_scene_renderer_render(renderer,p,fl_project_scene(p,composition_scene),FL_TICKS_PER_SECOND,100,&repeated));
    if (repeated.rgba) CHECK(near(at(&repeated,55,5),255,0,0,1) && !near(at(&repeated,45,5),255,0,0,1));
    fl_frame_free(&repeated);
    /* Lua structural output shares native render schemas and owner motion without authored child mutations. */
    Fl_Id generator_asset=0,generator_scene=0,generator_entity=0;
    const char *generator="return {build=function(ctx) return {version=1,root=1,next_id=2,next_component_id=4,duration=120000,beats={},clips={},entities={{id=1,parent=0,name='Generated',visible=true,components={{id=1,type='pose',version=1,x=0,y=0,z=0,rx=0,ry=0,rz=0,scale_x=1,scale_y=1,scale_z=1,pivot_x=0,pivot_y=0,pivot_z=0},{id=2,type='bounds',version=1,width=20,height=20,depth=0},{id=3,type='rectangle',version=1,color={255,0,0,255}}}}}} end,sample=function(ctx) ctx.entity.pose.x=10*ctx.time.seconds end}";
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Rendered generator",.source=generator,.source_size=strlen(generator)},&generator_asset));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_ADD,.name="Generated scene"},&generator_scene));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_SCENE_SETTINGS,.scene=generator_scene,.width=100,.height=100,.duration=2*FL_TICKS_PER_SECOND},NULL));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=generator_scene,.name="Generator owner"},&generator_entity));
    CHECK(fl_script_encode(&(Fl_Script){.script=generator_asset,.enabled=true},&behavior));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=generator_scene,.entity=generator_entity,.component=behavior},NULL)); fl_component_clear(&behavior);
    CHECK(fl_scene_renderer_render(renderer,p,fl_project_scene(p,generator_scene),FL_TICKS_PER_SECOND,100,&repeated));
    if (repeated.rgba) CHECK(near(at(&repeated,15,5),255,0,0,1) && !near(at(&repeated,5,5),255,0,0,1));
    fl_frame_free(&repeated);
    test_media(renderer,p,bus,argv[1]);
    fl_scene_renderer_destroy(renderer);
    fl_project_destroy(p);
    fl_fonts_destroy(fonts);
    fl_jobs_destroy(jobs);
    fl_event_bus_destroy(bus);
    ca_instance_destroy(instance);
    return FLOW_TEST_RESULT("fl_scene_renderer_tests");
}
