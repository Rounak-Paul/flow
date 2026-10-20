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
#include <stdatomic.h>

#define LOG_TAG "export"
#define SLICE_NS UINT64_C(25000000)
#define MAX_AUDIO_CLIPS 48
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

struct Fl_Exporter {
    Fl_EventBus *bus;
    Fl_SceneRenderer *renderer;
    Fl_EventType step_event;
    Fl_Subscription subscription;
    Fl_Project *project;
    Fl_Id *scenes;
    size_t scene_count, scene_index;
    uint64_t frame_in_scene, frames_in_scene, overlap, job, started_ns;
    FILE *pipe;
    char temporary[FL_PATH_MAX];
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
    return true;
}

/* An audio clip placed on the export timeline, all values in seconds. */
typedef struct Fl_ExportAudio {
    char path[FL_PATH_MAX];
    double start, offset, duration, gain, fade_in, fade_out;    /* duration 0 plays to the end of the file */
} Fl_ExportAudio;

/*
 * Collect the audio clips of the exported scenes, positioned on the output timeline and trimmed to their scene.
 * Returns the clip count; sets *overflow when more than MAX_AUDIO_CLIPS exist. Clips whose files are gone are skipped.
 */
static size_t collect_audio(Fl_Project *project, const Fl_Id *scenes, size_t count, uint32_t fps, uint64_t overlap, Fl_ExportAudio *out, bool *overflow)
{
    size_t found = 0;
    uint64_t first_frame = 0;
    *overflow = false;
    for (size_t i = 0; i < count; ++i) {
        const Fl_Scene *scene = fl_project_scene(project, scenes[i]);
        const double length = (double)fl_scene_length(scene) / FL_TICKS_PER_SECOND, origin = (double)first_frame / fps;
        for (size_t k = 0; k < scene->clip_count; ++k) {
            const Fl_AudioClip *c = &scene->clips[k];
            const Fl_Asset *asset = fl_project_asset(project, c->asset);
            Fl_ExportAudio a = {.gain = c->gain};
            if (!asset || !fl_project_asset_path(project, asset, a.path, sizeof(a.path))) continue;
            Fl_Tick start_tick;
            if (!fl_anchor_sample(scene, &c->start, &start_tick)) continue;
            const double start = (double)start_tick / FL_TICKS_PER_SECOND;
            a.offset = (double)c->source_offset / FL_TICKS_PER_SECOND;
            double natural = c->duration ? (double)c->duration / FL_TICKS_PER_SECOND : asset->duration ? (double)asset->duration / FL_TICKS_PER_SECOND - a.offset : 0;
            double room = length - (start > 0 ? start : 0);
            if (room <= 0) continue;
            a.duration = natural > 0 && natural < room ? natural : room;
            a.start = origin + start;
            a.fade_in = (double)c->fade_in / FL_TICKS_PER_SECOND; a.fade_out = (double)c->fade_out / FL_TICKS_PER_SECOND;
            if (start < 0) {a.offset -= start; a.duration += start; a.start = origin; if (a.duration <= 0) continue;}
            if (found == MAX_AUDIO_CLIPS) {*overflow = true; return found;}
            out[found++] = a;
        }
        first_frame += scene_frames(scene, fps) - (i + 1 < count ? overlap : 0);
    }
    return found;
}

/* Append the ffmpeg inputs, filter graph and codec arguments that mix audio under the video. Returns false when a buffer is full. */
static bool append_audio(char *command, size_t size, const Fl_ExportAudio *audio, size_t count, const char *video_filter, double total_seconds)
{
    char graph[8192], piece[512], arg[PATH_MAX_ARG];
    snprintf(graph, sizeof(graph), "[0:v]%s[v];", video_filter);
    for (size_t i = 0; i < count; ++i) {
        const Fl_ExportAudio *a = &audio[i];
        snprintf(arg, sizeof(arg), "%.6f", a->offset);
        if (!fl_command_append(command, size, "-ss") || !fl_command_append(command, size, arg)) return false;
        snprintf(arg, sizeof(arg), "%.6f", a->duration);
        if (!fl_command_append(command, size, "-t") || !fl_command_append(command, size, arg) || !fl_command_append(command, size, "-i") ||
            !fl_command_append(command, size, a->path)) return false;
        const double fade_in = fmin(a->fade_in, a->duration), fade_out = fmin(a->fade_out, a->duration);
        int n = snprintf(piece, sizeof(piece), "[%zu:a]aformat=sample_rates=48000:channel_layouts=stereo", i + 1);
        if (fade_in > 0) n += snprintf(piece + n, sizeof(piece) - n, ",afade=t=in:st=0:d=%.6f", fade_in);
        if (fade_out > 0) n += snprintf(piece + n, sizeof(piece) - n, ",afade=t=out:st=%.6f:d=%.6f", a->duration - fade_out, fade_out);
        const long delay = lround(a->start * 1000);
        snprintf(piece + n, sizeof(piece) - n, ",volume=%.6f,adelay=%ld|%ld[a%zu];", a->gain, delay, delay, i);
        if (strlen(graph) + strlen(piece) >= sizeof(graph)) return false;
        strcat(graph, piece);
    }
    for (size_t i = 0; i < count; ++i) {
        snprintf(piece, sizeof(piece), "[a%zu]", i);
        if (strlen(graph) + strlen(piece) >= sizeof(graph)) return false;
        strcat(graph, piece);
    }
    snprintf(piece, sizeof(piece), "amix=inputs=%zu:normalize=0:duration=longest,alimiter=limit=0.98[m]", count);
    if (strlen(graph) + strlen(piece) >= sizeof(graph)) return false;
    strcat(graph, piece);
    snprintf(arg, sizeof(arg), "%.6f", total_seconds);
    const char *tail[] = {"-filter_complex", graph, "-map", "[v]", "-map", "[m]", "-c:a", "aac", "-b:a", "192k", "-t", arg};
    for (size_t i = 0; i < sizeof(tail) / sizeof(*tail); ++i) if (!fl_command_append(command, size, tail[i])) return false;
    return true;
}

/* Stop encoding and record the outcome. */
static void finish(Fl_Exporter *e, Fl_ExportState state, const char *reason)
{
    bool encoded = e->pipe ? fl_command_close(e->pipe) : true;
    e->pipe = NULL; fl_project_destroy(e->project); e->project = NULL;
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

/* UI-thread slice: render frames for a bounded time, then yield. */
static bool on_step(const Fl_Event *event, void *data)
{
    Fl_Exporter *e = data;
    const Fl_ExportStep *step = event->payload;
    if (step->job != e->job) return false;
    if (e->status.state != FL_EXPORT_RUNNING) return true;
    uint64_t slice_start = fl_clock_monotonic_ns();
    while (e->status.state == FL_EXPORT_RUNNING && fl_clock_monotonic_ns() - slice_start < SLICE_NS) {
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
        bool written = fwrite(frame.rgba, 1, bytes, e->pipe) == bytes;
        fl_frame_free(&frame);
        if (!written) {finish(e, FL_EXPORT_FAILED, "ffmpeg stopped accepting frames"); break;}
        ++e->status.frames_done;
        if (++e->frame_in_scene == e->frames_in_scene) {
            e->frame_in_scene = e->overlap;
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
    Fl_Project *view=fl_project_read_view(project);
    if (!view) {snprintf(error,error_size,"could not capture the export document"); free(scenes); return false;}
    Fl_ExportAudio audio[MAX_AUDIO_CLIPS];
    bool overflow;
    const size_t audio_count = collect_audio(view, scenes, count, d->fps, overlap, audio, &overflow);
    if (overflow) {snprintf(error, error_size, "more than %d audio clips in the exported scenes", MAX_AUDIO_CLIPS); fl_project_destroy(view); free(scenes); return false;}
    static const char video_filter[] = "scale=out_color_matrix=bt709:flags=accurate_rnd+full_chroma_int";
    command[0] = 0;
    const char *head[] = {ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", size, "-r", rate, "-i", "-"};
    const char *tail[] = {"-c:v", "libx264", "-preset", "medium", "-crf", crf, "-pix_fmt", "yuv420p", "-colorspace", "bt709",
        "-color_primaries", "bt709", "-color_trc", "bt709", "-movflags", "+faststart", temporary};
    bool built = true;
    for (size_t i = 0; built && i < sizeof(head) / sizeof(*head); ++i) built = fl_command_append(command, sizeof(command), head[i]);
    if (built && audio_count) built = append_audio(command, sizeof(command), audio, audio_count, video_filter, (double)total / d->fps);
    else if (built) built = fl_command_append(command, sizeof(command), "-an") && fl_command_append(command, sizeof(command), "-vf") && fl_command_append(command, sizeof(command), video_filter);
    for (size_t i = 0; built && i < sizeof(tail) / sizeof(*tail); ++i) built = fl_command_append(command, sizeof(command), tail[i]);
    FILE *reserved=built ? fl_fs_fopen(temporary,"wbx") : NULL;
    bool owned=reserved!=NULL;
    if (reserved && fclose(reserved)) built=false;
    FILE *pipe = built && owned ? fl_command_open_writer(command) : NULL;
    if (!pipe) {snprintf(error, error_size, "could not start ffmpeg"); if (owned) fl_fs_remove(temporary); fl_project_destroy(view); free(scenes); return false;}
    e->job = job;
    free(e->scenes);
    e->scenes = scenes; e->scene_count = count; e->scene_index = 0; e->frame_in_scene = 0; e->overlap = overlap; e->pipe = pipe; e->project = view;
    snprintf(e->temporary,sizeof(e->temporary),"%s",temporary);
    e->frames_in_scene = scene_frames(fl_project_scene(view, scenes[0]), d->fps);
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
