#include "project/fl_resource_snapshot.h"
#include "fl_exporter.h"
#include "core/fl_clock.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"
#include "platform/fl_process.h"
#include "media/fl_encoder.h"
#include "time/fl_audio_plan.h"
#include "time/fl_time.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define LOG_TAG "export"
#define SLICE_NS UINT64_C(25000000)
#define MAX_AUDIO_CLIPS 4096
#define PATH_MAX_ARG 64

typedef struct Fl_ExportStep { uint64_t job; } Fl_ExportStep;

static atomic_uint_fast64_t next_job = 1;

/* Reserve a process-wide identity; exhaustion disables new jobs rather than reusing a queued identity. */
static uint64_t reserve_job(void)
{
    uint_fast64_t value = atomic_load_explicit(&next_job, memory_order_relaxed);
    while (value != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(&next_job, &value, value + 1,
                                                memory_order_relaxed, memory_order_relaxed)) return value;
    }
    return 0;
}

typedef struct Fl_ExportMix {
    Fl_MixerClip *clips;
    size_t count;
    Fl_Tick duration;
    char path[FL_PATH_MAX];
    atomic_bool cancel;
    bool success;
    Fl_JobHandle job;
} Fl_ExportMix;

/* Worker renders the same converters/envelopes as playback into the captured resource root. */
static void render_audio(void *data)
{
    Fl_ExportMix *mix=data;
    mix->success=fl_audio_mixer_file(mix->path,mix->clips,mix->count,mix->duration,48000,&mix->cancel);
}

/* Fence before releasing worker metadata or removing captured resource files. */
static void release_mix(Fl_ExportMix *mix, Fl_JobSystem *jobs)
{
    if (!mix) return;
    atomic_store_explicit(&mix->cancel,true,memory_order_relaxed); fl_job_wait(jobs,mix->job);
    free(mix->clips); free(mix);
}

struct Fl_Exporter {
    Fl_EventBus *bus;
    Fl_SceneRenderer *source_renderer, *renderer;
    Fl_EventType step_event;
    Fl_Subscription subscription, resource_subscription;
    Fl_ResourceSnapshot *resources;
    char command[4*FL_PATH_MAX];
    Fl_Project *project;
    Fl_Id *scenes;
    size_t scene_count, scene_index;
    uint64_t frame_in_scene, frames_in_scene, overlap, job, started_ns, render_started_ns;
    Fl_Encoder *encoder;
    Fl_ExportMix *mix;
    char temporary[FL_PATH_MAX];
    Fl_ExportStatus status;
    unsigned char *preview;
    uint32_t preview_w, preview_h;
    uint64_t preview_sequence;
};

#define PREVIEW_EDGE 480u

static void set_message(Fl_Exporter *e, const char *format, ...)
{
    va_list args; va_start(args, format);
    vsnprintf(e->status.message, sizeof(e->status.message), format, args);
    va_end(args);
}

/* Drop the display copy. The next encoded frame allocates a new one. */
static void clear_preview(Fl_Exporter *e)
{
    free(e->preview);
    e->preview = NULL;
    e->preview_w = 0;
    e->preview_h = 0;
    e->preview_sequence = 0;
}

/* Keep a nearest-neighbour copy no larger than PREVIEW_EDGE for the export window. */
static void store_preview(Fl_Exporter *e, const Fl_Frame *frame)
{
    if (!frame->rgba || !frame->width || !frame->height) return;
    uint32_t w = frame->width, h = frame->height;
    if (w > PREVIEW_EDGE || h > PREVIEW_EDGE) {
        if (w >= h) {
            h = (uint32_t)((uint64_t)frame->height * PREVIEW_EDGE / frame->width);
            w = PREVIEW_EDGE;
        } else {
            w = (uint32_t)((uint64_t)frame->width * PREVIEW_EDGE / frame->height);
            h = PREVIEW_EDGE;
        }
        if (!w) w = 1;
        if (!h) h = 1;
    }
    size_t bytes = (size_t)w * h * 4;
    if (!e->preview || w != e->preview_w || h != e->preview_h) {
        unsigned char *pixels = malloc(bytes);
        if (!pixels) return;
        free(e->preview);
        e->preview = pixels;
        e->preview_w = w;
        e->preview_h = h;
    }
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t sy = (uint32_t)((uint64_t)y * frame->height / h);
        const unsigned char *src = frame->rgba + (size_t)sy * frame->width * 4;
        unsigned char *dst = e->preview + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t sx = (uint32_t)((uint64_t)x * frame->width / w);
            memcpy(dst + (size_t)x * 4, src + (size_t)sx * 4, 4);
        }
    }
    e->preview_sequence = e->preview_sequence == UINT64_MAX ? 1 : e->preview_sequence + 1;
}

/* Frames needed to cover a scene at fps. */
static uint64_t scene_frames(const Fl_Scene *scene, uint32_t fps)
{
    double seconds = (double)fl_scene_length(scene) / FL_TICKS_PER_SECOND;
    uint64_t frames = (uint64_t)ceil(seconds * fps - 1e-9);
    return frames ? frames : 1;
}

/* Blend frame over base in linear light, base weighted 1 - t. Both are opaque frames of the same size. */
static void dissolve(Fl_Frame *base, const Fl_Frame *over, double t)
{
    static float to_linear[256];
    static unsigned char to_srgb[4097];
    static bool ready;
    if (!ready) {
        for (int i = 0; i < 256; ++i) {double c = i / 255.0; to_linear[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));}
        for (int i = 0; i <= 4096; ++i) {double c = i / 4096.0; to_srgb[i] = (unsigned char)lround(255 * (c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055));}
        ready = true;
    }
    const float keep = (float)(1 - t), add = (float)t;
    const size_t pixels = (size_t)base->width * base->height;
    for (size_t i = 0; i < pixels; ++i) {
        unsigned char *a = base->rgba + i * 4; const unsigned char *b = over->rgba + i * 4;
        for (int c = 0; c < 3; ++c) a[c] = to_srgb[(int)lrintf((to_linear[a[c]] * keep + to_linear[b[c]] * add) * 4096.f)];
    }
}

/* Render a scene at its frame index into frame. False when the size changed or rendering failed. */
static bool render_scene(Fl_Exporter *e, const Fl_Scene *scene, uint64_t index, Fl_Frame *frame, const char **reason)
{
    uint32_t w, h;
    Fl_Tick tick = (Fl_Tick)llround((double)index * FL_TICKS_PER_SECOND / e->status.fps);
    if (tick > fl_scene_length(scene)) tick = fl_scene_length(scene);
    if (!fl_frame_dimensions(scene, e->status.width, &w, &h) || w != e->status.width || h != e->status.height) {
        *reason = "a scene changed size or aspect during the export"; return false;
    }
    if (!fl_scene_renderer_render(e->renderer, e->project, scene, tick, w, frame)) {*reason = fl_scene_renderer_error(e->renderer); return false;}
    e->status.gpu_wait_seconds+=fl_scene_renderer_stats(e->renderer).gpu_wait_ms/1000.;
    return true;
}

/* Resolve all native/prototype audio against the captured resources and output scene offsets. */
static Fl_ExportMix *collect_audio(Fl_Project *project, const Fl_Id *scenes, size_t count, uint32_t fps,
                                  uint64_t overlap, uint64_t frames, char *error, size_t capacity)
{
    Fl_ExportMix *mix=calloc(1,sizeof(*mix)); if (!mix) return NULL;
    atomic_init(&mix->cancel,false);
    long double ticks=(long double)frames*FL_TICKS_PER_SECOND/fps;
    if (ticks>=(long double)INT64_MAX || !fl_fs_join(mix->path,sizeof(mix->path),fl_project_directory(project),"audio.rf64")) {
        snprintf(error,capacity,"Export audio duration/path exceeds supported range"); free(mix); return NULL;
    }
    mix->duration=(Fl_Tick)llroundl(ticks); uint64_t first_frame=0;
    for (size_t i=0;i<count;++i) {
        const Fl_Scene *scene=fl_project_scene(project,scenes[i]); Fl_AudioPlan plan={0};
        if (!fl_audio_plan_compile(scene,&plan)) {snprintf(error,capacity,"%s",plan.error); fl_audio_plan_clear(&plan); free(mix->clips); free(mix); return NULL;}
        if (plan.count>MAX_AUDIO_CLIPS-mix->count) {snprintf(error,capacity,"Export exceeds %u resolved audio segments",MAX_AUDIO_CLIPS); fl_audio_plan_clear(&plan); free(mix->clips); free(mix); return NULL;}
        Fl_MixerClip *grown=plan.count ? realloc(mix->clips,(mix->count+plan.count)*sizeof(*grown)) : mix->clips;
        bool ok=!plan.count || grown;
        if (ok) mix->clips=grown;
        for (size_t j=0;ok && j<plan.count;++j) ok=fl_audio_segment_clip(project,&plan.segments[j],(double)first_frame/fps,&mix->clips[mix->count++]);
        fl_audio_plan_clear(&plan);
        if (!ok) {snprintf(error,capacity,"Cannot resolve export audio resources/times"); free(mix->clips); free(mix); return NULL;}
        first_frame+=scene_frames(scene,fps)-(i+1<count ? overlap : 0);
    }
    return mix;
}

/* Stop encoding and record the outcome. */
static void finish(Fl_Exporter *e, Fl_ExportState state, const char *reason)
{
    char saved_reason[256];
    if (reason) {snprintf(saved_reason,sizeof(saved_reason),"%s",reason); reason=saved_reason;}
    bool encoded=e->encoder && fl_encoder_status(e->encoder)==FL_ENCODER_DONE;
    fl_encoder_destroy(e->encoder); e->encoder=NULL;
    release_mix(e->mix,fl_scene_renderer_jobs(e->source_renderer)); e->mix=NULL;
    fl_scene_renderer_destroy(e->renderer); e->renderer=NULL;
    fl_resource_snapshot_destroy(e->resources); e->resources=NULL; e->project = NULL;
    free(e->scenes); e->scenes = NULL;
    e->status.elapsed_seconds = (double)(fl_clock_monotonic_ns() - e->started_ns) / 1e9;
    if (state == FL_EXPORT_DONE && !encoded) {state = FL_EXPORT_FAILED; reason = "ffmpeg did not finish cleanly";}
    if (state==FL_EXPORT_DONE) {
        FILE *output=fl_fs_fopen(e->temporary,"r+b");
        bool flushed=output && fl_fs_flush(output);
        if (output && fclose(output)) flushed=false;
        if (!flushed || !fl_fs_commit(e->temporary,e->status.path)) {
            state=FL_EXPORT_FAILED; reason="could not commit the completed export";
        }
    }
    e->status.state = state;
    if (reason) set_message(e, "%s", reason);
    if (state != FL_EXPORT_DONE) fl_fs_remove(e->temporary);
    else FL_LOG_INFO(LOG_TAG, "wrote %s (%llu frames)", e->status.path, (unsigned long long)e->status.frames_done);
    e->temporary[0]=0;
    if (state == FL_EXPORT_FAILED) FL_LOG_ERROR(LOG_TAG, "%s", e->status.message);
}

static void queue_step(Fl_Exporter *e)
{
    Fl_ExportStep step = {.job = e->job};
    if (!fl_event_post(e->bus, e->step_event, &step, sizeof(step))) finish(e, FL_EXPORT_FAILED, "the editor could not schedule the next frames");
}

/* Resume only when this export's resource job completes; queued events contain job handles, not owners. */
static bool resources_done(const Fl_Event *event, void *data)
{
    Fl_Exporter *e=data;
    const Fl_JobDoneEvent *done=event->payload;
    if (e->status.state==FL_EXPORT_RUNNING && (fl_resource_snapshot_matches(e->resources,done) ||
        (e->mix && e->mix->job.generation && e->mix->job.index==done->job.index && e->mix->job.generation==done->job.generation))) queue_step(e);
    return false;
}

/* UI-thread slice: render frames for a bounded time, then yield. */
static bool on_step(const Fl_Event *event, void *data)
{
    Fl_Exporter *e = data;
    const Fl_ExportStep *step = event->payload;
    if (step->job != e->job) return false;
    if (e->status.state != FL_EXPORT_RUNNING) return true;
    Fl_ResourceStatus resources=fl_resource_snapshot_status(e->resources);
    if (resources==FL_RESOURCES_PENDING) return true;
    if (resources!=FL_RESOURCES_READY) {finish(e,FL_EXPORT_FAILED,fl_resource_snapshot_error(e->resources)); return true;}
    if (e->mix) {
        Fl_JobSystem *jobs=fl_scene_renderer_jobs(e->source_renderer);
        if (!e->mix->job.generation) {
            e->mix->job=fl_job_submit(jobs,&(Fl_JobDesc){.fn=render_audio,.data=e->mix,.priority=FL_JOB_PRIORITY_NORMAL,.notify=true});
            if (!e->mix->job.generation) finish(e,FL_EXPORT_FAILED,"Cannot schedule audio rendering");
            else set_message(e,"Rendering audio");
            return true;
        }
        if (!fl_job_is_done(jobs,e->mix->job)) return true;
        if (!e->mix->success) {finish(e,FL_EXPORT_FAILED,"Cannot render captured audio resources"); return true;}
    }
    if (!e->encoder) {
        e->encoder=fl_encoder_create(e->command,e->bus,e->step_event,e->job);
        if (!e->encoder) {finish(e,FL_EXPORT_FAILED,"could not create encoder worker"); return true;}
        set_message(e,"Rendering"); return true;
    }
    Fl_EncoderStatus encoding=fl_encoder_status(e->encoder);
    if (encoding==FL_ENCODER_FAILED) {finish(e,FL_EXPORT_FAILED,"ffmpeg stopped accepting frames or did not finish cleanly"); return true;}
    if (encoding==FL_ENCODER_DONE) {finish(e,FL_EXPORT_DONE,"export finished"); return true;}
    if (encoding!=FL_ENCODER_READY) return true;
    uint64_t slice_start = fl_clock_monotonic_ns();
    if (!e->render_started_ns) {
        e->render_started_ns=slice_start;
        e->status.preparation_seconds=(double)(slice_start-e->started_ns)/1e9;
    }
    while (e->status.state == FL_EXPORT_RUNNING && fl_clock_monotonic_ns() - slice_start < SLICE_NS) {
        if (fl_encoder_status(e->encoder)!=FL_ENCODER_READY) break;
        const Fl_Scene *scene = fl_project_scene(e->project, e->scenes[e->scene_index]);
        if (!scene) {finish(e, FL_EXPORT_FAILED, "a scene was deleted during the export"); break;}
        const char *reason = NULL;
        Fl_Frame frame;
        if (!render_scene(e, scene, e->frame_in_scene, &frame, &reason)) {finish(e, FL_EXPORT_FAILED, reason); break;}
        const bool dissolving = e->scene_index + 1 < e->scene_count && e->frame_in_scene + e->overlap >= e->frames_in_scene && e->overlap;
        if (dissolving) {
            const Fl_Scene *next = fl_project_scene(e->project, e->scenes[e->scene_index + 1]);
            const uint64_t into = e->frame_in_scene + e->overlap - e->frames_in_scene;
            Fl_Frame incoming = {0};
            bool ok = next && render_scene(e, next, into, &incoming, &reason);
            if (ok) dissolve(&frame, &incoming, (double)(into + 1) / (e->overlap + 1));
            fl_frame_free(&incoming);
            if (!ok) {fl_frame_free(&frame); finish(e, FL_EXPORT_FAILED, reason ? reason : "a scene was deleted during the export"); break;}
        }
        size_t bytes = (size_t)frame.width * frame.height * 4;
        store_preview(e, &frame);
        bool written=fl_encoder_submit(e->encoder,frame.rgba,bytes);
        if (written) frame.rgba=NULL;
        fl_frame_free(&frame);
        if (!written) {finish(e, FL_EXPORT_FAILED, "ffmpeg stopped accepting frames"); break;}
        ++e->status.frames_done;
        e->status.render_seconds=(double)(fl_clock_monotonic_ns()-e->render_started_ns)/1e9;
        e->status.frames_per_second=e->status.frames_done>=8 && e->status.render_seconds>0 ?
            (double)e->status.frames_done/e->status.render_seconds : 0;
        if (++e->frame_in_scene == e->frames_in_scene) {
            e->frame_in_scene = e->overlap;
            if (++e->scene_index == e->scene_count) {fl_encoder_end(e->encoder); set_message(e,"Finishing encoding"); break;}
            const Fl_Scene *next = fl_project_scene(e->project, e->scenes[e->scene_index]);
            if (!next) {finish(e, FL_EXPORT_FAILED, "a scene was deleted during the export"); break;}
            e->frames_in_scene = scene_frames(next, e->status.fps);
        }
    }
    if (e->status.state == FL_EXPORT_RUNNING) {
        e->status.elapsed_seconds = (double)(fl_clock_monotonic_ns() - e->started_ns) / 1e9;
        if (fl_encoder_status(e->encoder)==FL_ENCODER_READY) queue_step(e);
    }
    return true;
}

Fl_Exporter *fl_exporter_create(Fl_EventBus *bus, Fl_SceneRenderer *renderer)
{
    if (!bus || !renderer || fl_jobs_event_bus(fl_scene_renderer_jobs(renderer))!=bus) return NULL;
    Fl_Exporter *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->bus = bus; e->source_renderer = renderer;
    e->step_event = fl_event_type_register(bus, "export.step", sizeof(Fl_ExportStep));
    e->subscription = fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = e->step_event, .fn = on_step, .user_data = e, .owner = e});
    e->resource_subscription=fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_jobs_done_event(fl_scene_renderer_jobs(renderer)),.fn=resources_done,.user_data=e,.owner=e});
    if (e->step_event == FL_EVENT_TYPE_INVALID || !e->subscription.generation || !e->resource_subscription.generation) {fl_event_unsubscribe_owner(bus,e); free(e); return NULL;}
    return e;
}

void fl_exporter_destroy(Fl_Exporter *e)
{
    if (!e) return;
    fl_event_unsubscribe_owner(e->bus, e);
    if (e->status.state == FL_EXPORT_RUNNING) finish(e, FL_EXPORT_CANCELLED, "the editor closed");
    free(e->preview);
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
    const uint64_t job = reserve_job();
    if (!job) {snprintf(error, error_size, "export job identities exhausted"); return false;}
    size_t count = d->scenes ? d->scene_count : fl_project_scene_count(project);
    if (!count) {snprintf(error, error_size, "there is nothing to export"); return false;}
    if (count>SIZE_MAX/sizeof(Fl_Id)) {snprintf(error,error_size,"too many export scenes"); return false;}
    Fl_Id *scenes = malloc(count * sizeof(*scenes));
    if (!scenes) {snprintf(error, error_size, "out of memory"); return false;}
    uint64_t total = 0, shortest = UINT64_MAX;
    uint32_t width = 0, height = 0;
    for (size_t i = 0; i < count; ++i) {
        const Fl_Scene *s = d->scenes ? fl_project_scene(project, d->scenes[i]) : fl_project_scene_at(project, i);
        uint32_t w, h;
        if (!s || !fl_frame_dimensions(s, d->width, &w, &h)) {snprintf(error, error_size, "scene %zu is missing or width %u is not a valid size", i + 1, d->width); free(scenes); return false;}
        if (i && (w != width || h != height)) {snprintf(error, error_size, "scenes have different aspect ratios; export them separately"); free(scenes); return false;}
        uint64_t frames=scene_frames(s,d->fps);
        if (frames>UINT64_MAX-total) {snprintf(error,error_size,"export frame count exceeds supported range"); free(scenes); return false;}
        width = w; height = h; scenes[i] = s->id; total += frames;
        if (scene_frames(s, d->fps) < shortest) shortest = scene_frames(s, d->fps);
    }
    if ((width | height) & 1) {snprintf(error, error_size, "the output size %ux%u must be even; choose another width", width, height); free(scenes); return false;}
    if (!isfinite(d->crossfade) || d->crossfade < 0 || d->crossfade > 60) {snprintf(error, error_size, "crossfade is 0..60 seconds"); free(scenes); return false;}
    const uint64_t overlap = count > 1 ? (uint64_t)llround(d->crossfade * d->fps) : 0;
    if (overlap >= shortest) {snprintf(error, error_size, "the crossfade must be shorter than every scene"); free(scenes); return false;}
    total -= overlap * (count - 1);
    char command[4 * FL_PATH_MAX], size[32], rate[16], crf[8];
    snprintf(size, sizeof(size), "%ux%u", width, height); snprintf(rate, sizeof(rate), "%u", d->fps); snprintf(crf, sizeof(crf), "%u", d->crf ? d->crf : 16);
    char temporary[FL_PATH_MAX];
    int length=snprintf(temporary,sizeof(temporary),"%s.flow-%016llx-%016llx.mp4",d->path,
        (unsigned long long)fl_clock_monotonic_ns(),(unsigned long long)job);
    if (length<0 || (size_t)length>=sizeof(temporary)) {snprintf(error,error_size,"export path is too long"); free(scenes); return false;}
    Fl_ResourceSnapshot *resources=fl_resource_snapshot_create(project,fl_scene_renderer_jobs(e->source_renderer),scenes,count,error,error_size);
    Fl_Project *view=fl_resource_snapshot_project(resources);
    if (!view) {free(scenes); return false;}
    Fl_ExportMix *mix=collect_audio(view,scenes,count,d->fps,overlap,total,error,error_size);
    if (!mix) {fl_resource_snapshot_destroy(resources); free(scenes); return false;}
    bool has_audio=mix->count!=0;
    static const char video_filter[] = "scale=out_color_matrix=bt709:flags=accurate_rnd+full_chroma_int";
    command[0] = 0;
    const char *head[] = {ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", size, "-r", rate, "-i", "-"};
    const char *tail[] = {"-c:v", "libx264", "-preset", "medium", "-crf", crf, "-pix_fmt", "yuv420p", "-colorspace", "bt709",
        "-color_primaries", "bt709", "-color_trc", "bt709", "-movflags", "+faststart", temporary};
    bool built = true;
    for (size_t i = 0; built && i < sizeof(head) / sizeof(*head); ++i) built = fl_command_append(command, sizeof(command), head[i]);
    if (built && has_audio) {
        const char *audio[]={"-i",mix->path,"-map","0:v","-map","1:a","-c:a","aac","-b:a","192k","-vf",video_filter};
        for (size_t i=0;built && i<sizeof(audio)/sizeof(*audio);++i) built=fl_command_append(command,sizeof(command),audio[i]);
    } else if (built) built=fl_command_append(command,sizeof(command),"-an") && fl_command_append(command,sizeof(command),"-vf") && fl_command_append(command,sizeof(command),video_filter);
    for (size_t i = 0; built && i < sizeof(tail) / sizeof(*tail); ++i) built = fl_command_append(command, sizeof(command), tail[i]);
    FILE *reserved=built ? fl_fs_fopen(temporary,"wbx") : NULL;
    bool owned=reserved!=NULL;
    if (reserved && fclose(reserved)) built=false;
    if (!built || !owned) {snprintf(error,error_size,"could not prepare export output"); if (owned) fl_fs_remove(temporary); release_mix(mix,fl_scene_renderer_jobs(e->source_renderer)); fl_resource_snapshot_destroy(resources); free(scenes); return false;}
    error[0]=0;
    Fl_SceneRenderer *renderer=fl_scene_renderer_snapshot(e->source_renderer,view,scenes,count,error,error_size);
    if (!renderer) {fl_fs_remove(temporary); release_mix(mix,fl_scene_renderer_jobs(e->source_renderer)); fl_resource_snapshot_destroy(resources); free(scenes); return false;}
    e->renderer=renderer; e->resources=resources; e->render_started_ns=0;
    if (!has_audio) {release_mix(mix,fl_scene_renderer_jobs(e->source_renderer)); mix=NULL;}
    e->mix=mix;
    snprintf(e->command,sizeof(e->command),"%s",command);
    e->job = job;
    free(e->scenes);
    e->scenes = scenes; e->scene_count = count; e->scene_index = 0; e->frame_in_scene = 0; e->overlap = overlap; e->encoder = NULL; e->project = view;
    snprintf(e->temporary,sizeof(e->temporary),"%s",temporary);
    e->frames_in_scene = scene_frames(fl_project_scene(view, scenes[0]), d->fps);
    e->started_ns = fl_clock_monotonic_ns();
    clear_preview(e);
    e->status = (Fl_ExportStatus){.state = FL_EXPORT_RUNNING, .frames_total = total, .width = width, .height = height, .fps = d->fps,
        .crf = d->crf ? d->crf : 16, .crossfade = overlap ? d->crossfade : 0, .scene_count = count};
    snprintf(e->status.path, sizeof(e->status.path), "%s", d->path);
    set_message(e,"Preparing resources");
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
    if (!e) return (Fl_ExportStatus){0};
    Fl_ExportStatus status = e->status;
    if (status.state == FL_EXPORT_RUNNING && e->started_ns)
        status.elapsed_seconds = (double)(fl_clock_monotonic_ns() - e->started_ns) / 1e9;
    return status;
}

bool fl_exporter_preview(const Fl_Exporter *e, Fl_ExportPreview *out)
{
    if (out) *out = (Fl_ExportPreview){0};
    if (!e || !out || !e->preview || !e->preview_w || !e->preview_h || !e->preview_sequence) return false;
    out->rgba = e->preview;
    out->width = e->preview_w;
    out->height = e->preview_h;
    out->sequence = e->preview_sequence;
    return true;
}
