#include "fl_asset_thumbnails.h"
#include "style.h"
#include "media/fl_thumbnail.h"
#include <stdlib.h>
#include <string.h>

typedef struct Fl_ThumbnailJob {
    Fl_ThumbnailSource source;
    unsigned char *pixels;
    uint32_t width, height;
} Fl_ThumbnailJob;
typedef enum Fl_ThumbnailState { THUMB_PENDING, THUMB_READY, THUMB_NONE } Fl_ThumbnailState;
typedef struct Fl_ThumbnailEntry {
    Fl_Id asset;
    Fl_ThumbnailState state;
    Fl_JobHandle handle;
    Fl_ThumbnailJob *job;
    Ca_Image *image;
    uint32_t width, height;
} Fl_ThumbnailEntry;
struct Fl_AssetThumbnails {
    Ca_Instance *instance;
    Fl_Project *project;
    Fl_JobSystem *jobs;
    Fl_EventBus *bus;
    Fl_ThumbnailsReadyFn ready;
    void *ready_data;
    Fl_ThumbnailEntry *entries;
    size_t count, capacity;
};

/* Worker body: produce or load one preview into the job's own storage. */
static void generate(void *data)
{
    Fl_ThumbnailJob *job = data;
    job->pixels = fl_thumbnail_get(&job->source, FL_STYLE_THUMBNAIL_PIXELS, &job->width, &job->height);
}

/* Upload a finished preview on the owner thread and notify the owner. */
static bool job_done(const Fl_Event *event, void *data)
{
    Fl_AssetThumbnails *t = data;
    const Fl_JobDoneEvent *done = event->payload;
    for (size_t i = 0; i < t->count; ++i) {
        Fl_ThumbnailEntry *e = &t->entries[i];
        if (e->state != THUMB_PENDING || e->handle.index != done->job.index || e->handle.generation != done->job.generation) continue;
        Fl_ThumbnailJob *job = e->job;
        e->image = job->pixels && job->width <= INT32_MAX && job->height <= INT32_MAX
            ? ca_image_create(t->instance, job->pixels, (int)job->width, (int)job->height) : NULL;
        e->width = job->width; e->height = job->height;
        e->state = e->image ? THUMB_READY : THUMB_NONE;
        free(job->pixels); free(job); e->job = NULL;
        if (t->ready) t->ready(t->ready_data);
        break;
    }
    return false;
}

Fl_AssetThumbnails *fl_asset_thumbnails_create(Ca_Instance *instance, Fl_Project *project, Fl_JobSystem *jobs,
                                               Fl_EventBus *bus, Fl_ThumbnailsReadyFn ready, void *ready_data)
{
    if (!instance || !project || !jobs || !bus) return NULL;
    Fl_AssetThumbnails *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    *t = (Fl_AssetThumbnails){instance, project, jobs, bus, ready, ready_data, NULL, 0, 0};
    if (!fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_jobs_done_event(jobs), .fn = job_done, .user_data = t, .owner = t}).generation) {
        free(t); return NULL;
    }
    return t;
}

void fl_asset_thumbnails_destroy(Fl_AssetThumbnails *t)
{
    if (!t) return;
    fl_event_unsubscribe_owner(t->bus, t);
    for (size_t i = 0; i < t->count; ++i) {
        Fl_ThumbnailEntry *e = &t->entries[i];
        if (e->state == THUMB_PENDING) { fl_job_wait(t->jobs, e->handle); if (e->job) free(e->job->pixels); free(e->job); }
        if (e->image) ca_image_destroy(t->instance, e->image);
    }
    free(t->entries); free(t);
}

Ca_Image *fl_asset_thumbnails_get(Fl_AssetThumbnails *t, const Fl_Asset *asset, uint32_t *width, uint32_t *height)
{
    if (!t || !asset) return NULL;
    for (size_t i = 0; i < t->count; ++i) {
        Fl_ThumbnailEntry *e = &t->entries[i];
        if (e->asset != asset->id) continue;
        if (e->state != THUMB_READY) return NULL;
        if (width) *width = e->width;
        if (height) *height = e->height;
        return e->image;
    }
    if (t->count == t->capacity) {
        size_t capacity = t->capacity ? t->capacity * 2 : 32;
        Fl_ThumbnailEntry *grown = realloc(t->entries, capacity * sizeof(*grown));
        if (!grown) return NULL;
        t->entries = grown; t->capacity = capacity;
    }
    Fl_ThumbnailEntry *e = &t->entries[t->count++];
    *e = (Fl_ThumbnailEntry){.asset = asset->id, .state = THUMB_NONE};
    e->job = calloc(1, sizeof(*e->job));
    if (!e->job || !fl_thumbnail_source(t->project, asset, &e->job->source)) { free(e->job); e->job = NULL; return NULL; }
    e->handle = fl_job_submit(t->jobs, &(Fl_JobDesc){.fn = generate, .data = e->job, .priority = FL_JOB_PRIORITY_LOW, .notify = true});
    if (e->handle.generation) e->state = THUMB_PENDING;
    else { free(e->job); e->job = NULL; }
    return NULL;
}
