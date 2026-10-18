#include "fl_exporter.h"
#include "core/fl_clock.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"
#include "platform/fl_process.h"
#include "time/fl_time.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "export"
#define SLICE_NS UINT64_C(25000000)

typedef struct Fl_ExportStep { Fl_Exporter *exporter; uint64_t job; } Fl_ExportStep;

struct Fl_Exporter {
    Fl_EventBus *bus;
    Fl_SceneRenderer *renderer;
    Fl_EventType step_event;
    Fl_Subscription subscription;
    Fl_Project *project;
    Fl_Id *scenes;
    size_t scene_count, scene_index;
    uint64_t frame_in_scene, frames_in_scene, job, started_ns;
    FILE *pipe;
    Fl_ExportStatus status;
};

static void set_message(Fl_Exporter *e, const char *format, ...)
{
    va_list args; va_start(args, format);
    vsnprintf(e->status.message, sizeof(e->status.message), format, args);
    va_end(args);
}

/* Frames needed to cover a scene at fps. */
static uint64_t scene_frames(const Fl_Scene *scene, uint32_t fps)
{
    double seconds = (double)fl_scene_length(scene) / FL_TICKS_PER_SECOND;
    uint64_t frames = (uint64_t)ceil(seconds * fps - 1e-9);
    return frames ? frames : 1;
}

/* Stop encoding and record the outcome. */
static void finish(Fl_Exporter *e, Fl_ExportState state, const char *reason)
{
    bool encoded = e->pipe ? fl_command_close(e->pipe) : true;
    e->pipe = NULL; e->project = NULL;
    free(e->scenes); e->scenes = NULL;
    e->status.elapsed_seconds = (double)(fl_clock_monotonic_ns() - e->started_ns) / 1e9;
    if (state == FL_EXPORT_DONE && !encoded) {state = FL_EXPORT_FAILED; reason = "ffmpeg did not finish cleanly";}
    e->status.state = state;
    if (reason) set_message(e, "%s", reason);
    if (state != FL_EXPORT_DONE) fl_fs_remove(e->status.path);
    else FL_LOG_INFO(LOG_TAG, "wrote %s (%llu frames)", e->status.path, (unsigned long long)e->status.frames_done);
    if (state == FL_EXPORT_FAILED) FL_LOG_ERROR(LOG_TAG, "%s", e->status.message);
}

static void queue_step(Fl_Exporter *e)
{
    Fl_ExportStep step = {e, e->job};
    if (!fl_event_post(e->bus, e->step_event, &step, sizeof(step))) finish(e, FL_EXPORT_FAILED, "the editor could not schedule the next frames");
}

/* UI-thread slice: render frames for a bounded time, then yield. */
static bool on_step(const Fl_Event *event, void *data)
{
    (void)data;
    const Fl_ExportStep *step = event->payload;
    Fl_Exporter *e = step->exporter;
    if (e->status.state != FL_EXPORT_RUNNING || step->job != e->job) return true;
    uint64_t slice_start = fl_clock_monotonic_ns();
    while (e->status.state == FL_EXPORT_RUNNING && fl_clock_monotonic_ns() - slice_start < SLICE_NS) {
        const Fl_Scene *scene = fl_project_scene(e->project, e->scenes[e->scene_index]);
        if (!scene) {finish(e, FL_EXPORT_FAILED, "a scene was deleted during the export"); break;}
        uint32_t w, h;
        Fl_Tick tick = (Fl_Tick)llround((double)e->frame_in_scene * FL_TICKS_PER_SECOND / e->status.fps);
        if (tick > fl_scene_length(scene)) tick = fl_scene_length(scene);
        Fl_Frame frame;
        if (!fl_frame_dimensions(scene, e->status.width, &w, &h) || w != e->status.width || h != e->status.height) {
            finish(e, FL_EXPORT_FAILED, "a scene changed size or aspect during the export"); break;
        }
        if (!fl_scene_renderer_render(e->renderer, e->project, scene, tick, w, &frame)) {finish(e, FL_EXPORT_FAILED, "a frame could not be rendered"); break;}
        size_t bytes = (size_t)w * h * 4;
        bool written = fwrite(frame.rgba, 1, bytes, e->pipe) == bytes;
        fl_frame_free(&frame);
        if (!written) {finish(e, FL_EXPORT_FAILED, "ffmpeg stopped accepting frames"); break;}
        ++e->status.frames_done;
        if (++e->frame_in_scene == e->frames_in_scene) {
            e->frame_in_scene = 0;
            if (++e->scene_index == e->scene_count) {finish(e, FL_EXPORT_DONE, "export finished"); break;}
            const Fl_Scene *next = fl_project_scene(e->project, e->scenes[e->scene_index]);
            if (!next) {finish(e, FL_EXPORT_FAILED, "a scene was deleted during the export"); break;}
            e->frames_in_scene = scene_frames(next, e->status.fps);
        }
    }
    if (e->status.state == FL_EXPORT_RUNNING) {
        e->status.elapsed_seconds = (double)(fl_clock_monotonic_ns() - e->started_ns) / 1e9;
        queue_step(e);
    }
    return true;
}

Fl_Exporter *fl_exporter_create(Fl_EventBus *bus, Fl_SceneRenderer *renderer)
{
    if (!bus || !renderer) return NULL;
    Fl_Exporter *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->bus = bus; e->renderer = renderer;
    e->step_event = fl_event_type_register(bus, "export.step", sizeof(Fl_ExportStep));
    e->subscription = fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = e->step_event, .fn = on_step, .user_data = e, .owner = e});
    if (e->step_event == FL_EVENT_TYPE_INVALID || !e->subscription.generation) {free(e); return NULL;}
    return e;
}

void fl_exporter_destroy(Fl_Exporter *e)
{
    if (!e) return;
    fl_event_unsubscribe_owner(e->bus, e);
    if (e->status.state == FL_EXPORT_RUNNING) finish(e, FL_EXPORT_CANCELLED, "the editor closed");
    free(e);
}

bool fl_exporter_start(Fl_Exporter *e, Fl_Project *project, const Fl_ExportDesc *d, char *error, size_t error_size)
{
    snprintf(error, error_size, "invalid export request");
    if (!e || !project || !d || !d->path || !d->path[0]) return false;
    if (e->status.state == FL_EXPORT_RUNNING) {snprintf(error, error_size, "an export is already running"); return false;}
    if (d->fps < 1 || d->fps > 120 || d->crf > 51) {snprintf(error, error_size, "fps is 1..120 and crf 0..51"); return false;}
    const char *ffmpeg = fl_ffmpeg_path();
    if (!ffmpeg) {snprintf(error, error_size, "ffmpeg was not found; install it or set FLOW_FFMPEG"); return false;}
    size_t count = d->scenes ? d->scene_count : fl_project_scene_count(project);
    if (!count) {snprintf(error, error_size, "there is nothing to export"); return false;}
    Fl_Id *scenes = malloc(count * sizeof(*scenes));
    if (!scenes) {snprintf(error, error_size, "out of memory"); return false;}
    uint64_t total = 0;
    uint32_t width = 0, height = 0;
    for (size_t i = 0; i < count; ++i) {
        const Fl_Scene *s = d->scenes ? fl_project_scene(project, d->scenes[i]) : fl_project_scene_at(project, i);
        uint32_t w, h;
        if (!s || !fl_frame_dimensions(s, d->width, &w, &h)) {snprintf(error, error_size, "scene %zu is missing or width %u is not a valid size", i + 1, d->width); free(scenes); return false;}
        if (i && (w != width || h != height)) {snprintf(error, error_size, "scenes have different aspect ratios; export them separately"); free(scenes); return false;}
        width = w; height = h; scenes[i] = s->id; total += scene_frames(s, d->fps);
    }
    if ((width | height) & 1) {snprintf(error, error_size, "the output size %ux%u must be even; choose another width", width, height); free(scenes); return false;}
    char command[4 * FL_PATH_MAX], size[32], rate[16], crf[8];
    snprintf(size, sizeof(size), "%ux%u", width, height); snprintf(rate, sizeof(rate), "%u", d->fps); snprintf(crf, sizeof(crf), "%u", d->crf ? d->crf : 16);
    command[0] = 0;
    const char *args[] = {ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", size, "-r", rate, "-i", "-", "-an",
        "-vf", "scale=out_color_matrix=bt709:flags=accurate_rnd+full_chroma_int", "-c:v", "libx264", "-preset", "medium", "-crf", crf,
        "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-movflags", "+faststart", d->path};
    bool built = true;
    for (size_t i = 0; built && i < sizeof(args) / sizeof(*args); ++i) built = fl_command_append(command, sizeof(command), args[i]);
    FILE *pipe = built ? fl_command_open_writer(command) : NULL;
    if (!pipe) {snprintf(error, error_size, "could not start ffmpeg"); free(scenes); return false;}
    ++e->job;
    free(e->scenes);
    e->scenes = scenes; e->scene_count = count; e->scene_index = 0; e->frame_in_scene = 0; e->pipe = pipe; e->project = project;
    e->frames_in_scene = scene_frames(fl_project_scene(project, scenes[0]), d->fps);
    e->started_ns = fl_clock_monotonic_ns();
    e->status = (Fl_ExportStatus){.state = FL_EXPORT_RUNNING, .frames_total = total, .width = width, .height = height, .fps = d->fps};
    snprintf(e->status.path, sizeof(e->status.path), "%s", d->path);
    FL_LOG_INFO(LOG_TAG, "exporting %llu frames at %ux%u, %u fps to %s", (unsigned long long)total, width, height, d->fps, d->path);
    error[0] = 0;
    queue_step(e);
    return e->status.state == FL_EXPORT_RUNNING || (snprintf(error, error_size, "%s", e->status.message), false);
}

void fl_exporter_cancel(Fl_Exporter *e)
{
    if (e && e->status.state == FL_EXPORT_RUNNING) finish(e, FL_EXPORT_CANCELLED, "cancelled");
}

void fl_exporter_abort(Fl_Exporter *e, const char *reason)
{
    if (e && e->status.state == FL_EXPORT_RUNNING) finish(e, FL_EXPORT_FAILED, reason);
}

Fl_ExportStatus fl_exporter_status(const Fl_Exporter *e)
{
    return e ? e->status : (Fl_ExportStatus){0};
}
