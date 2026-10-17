#include "fl_asset_import.h"
#include "core/fl_atomic.h"
#include "core/fl_clock.h"
#include "core/fl_log.h"
#include "core/fl_sha256.h"
#include "core/fl_thread.h"
#include "media/fl_media.h"
#include "platform/fl_fs.h"
#include "platform/fl_video_frame.h"
#include "text/fl_font_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "assets"
#define COPY_CHUNK (1u << 20)

typedef struct Fl_ImportRequest {
    uint64_t id;
    Fl_JobHandle job;
    char directory[FL_PATH_MAX];
    char source[FL_PATH_MAX];
    Fl_StoredAsset stored;
    Fl_ImportStatus status;
} Fl_ImportRequest;
struct Fl_AssetImporter {
    Fl_Project *project;
    Fl_JobSystem *jobs;
    Fl_EventBus *bus;
    Fl_EventType imported;
    Fl_ImportRequest **requests;
    size_t count, capacity;
    uint64_t next_request;
};

/* Produce a store-local temporary name unique across threads and processes. */
static bool temporary_path(const char *folder, char *out, size_t capacity)
{
    static volatile uint64_t counter;
    char leaf[96];
    snprintf(leaf, sizeof(leaf), ".import-%llx-%llx-%llx.tmp", (unsigned long long)fl_clock_monotonic_ns(),
             (unsigned long long)fl_thread_current_id(), (unsigned long long)fl_atomic_fetch_add_u64(&counter, 1));
    return fl_fs_join(out, capacity, folder, leaf);
}

/* Validate that stored bytes decode as the sniffed media, filling image dimensions. */
static bool validate_media(const char *path, Fl_StoredAsset *out)
{
    if (out->kind == FL_ASSET_IMAGE) {
        uint32_t w, h;
        if (!fl_image_probe(path, &w, &h)) return false;
        out->width = w; out->height = h; return true;
    }
    if (out->kind == FL_ASSET_FONT) return fl_font_file_valid(path);
    return true;
}

/* Record a stored video's displayed size; platform frameworks identify containers by the final extension. */
static void probe_video(const char *path, Fl_StoredAsset *out)
{
    uint32_t w, h, vw = 0, vh = 0;
    free(fl_video_frame(path, 16, &w, &h, &vw, &vh));
    out->width = vw; out->height = vh;
}

Fl_ImportStatus fl_asset_store_file(const char *directory, const char *source, Fl_StoredAsset *out)
{
    if (!directory || !source || !out) return FL_IMPORT_FAILED;
    *out = (Fl_StoredAsset){0};
    char folder[FL_PATH_MAX], temp[FL_PATH_MAX], final[FL_PATH_MAX];
    if (!fl_fs_join(folder, sizeof(folder), directory, "assets") || !fl_fs_mkdir_p(folder) ||
        !temporary_path(folder, temp, sizeof(temp))) return FL_IMPORT_FAILED;
    FILE *input = fl_fs_fopen(source, "rb");
    if (!input) return FL_IMPORT_FAILED;
    unsigned char *buffer = malloc(COPY_CHUNK);
    if (!buffer) { fclose(input); return FL_IMPORT_FAILED; }
    size_t head = fread(buffer, 1, FL_MEDIA_SNIFF_BYTES, input);
    Fl_MediaFormat format;
    if (ferror(input) || !fl_media_sniff(buffer, head, &format)) {
        bool unreadable = ferror(input) != 0;
        free(buffer); fclose(input);
        return unreadable ? FL_IMPORT_FAILED : FL_IMPORT_UNSUPPORTED;
    }
    out->kind = format.kind;
    snprintf(out->extension, sizeof(out->extension), "%s", format.extension);
    FILE *output = fl_fs_fopen(temp, "wb");
    bool ok = output != NULL;
    Fl_Sha256 hash; fl_sha256_init(&hash);
    for (size_t n = head; ok && n; n = fread(buffer, 1, COPY_CHUNK, input)) {
        fl_sha256_update(&hash, buffer, n);
        out->size += n;
        ok = fwrite(buffer, 1, n, output) == n;
    }
    ok = ok && !ferror(input) && fl_fs_flush(output);
    if (output && fclose(output) != 0) ok = false;
    fclose(input); free(buffer);
    fl_sha256_final(&hash, out->hash);
    Fl_ImportStatus status = ok ? FL_IMPORT_ADDED : FL_IMPORT_FAILED;
    if (ok && !validate_media(temp, out)) status = FL_IMPORT_UNSUPPORTED;
    if (status == FL_IMPORT_ADDED && !fl_asset_store_path(directory, out->hash, out->extension, final, sizeof(final)))
        status = FL_IMPORT_FAILED;
    if (status == FL_IMPORT_ADDED && fl_fs_file_size(final) == (int64_t)out->size) fl_fs_remove(temp);
    else if (status == FL_IMPORT_ADDED && fl_fs_commit(temp, final)) out->created = true;
    else status = status == FL_IMPORT_ADDED ? FL_IMPORT_FAILED : status;
    if (status == FL_IMPORT_ADDED) {
        if (out->kind == FL_ASSET_VIDEO) probe_video(final, out);
        return status;
    }
    fl_fs_remove(temp);
    return status;
}

/* Worker body; only the request's own storage is touched. */
static void import_job(void *data)
{
    Fl_ImportRequest *r = data;
    r->status = fl_asset_store_file(r->directory, r->source, &r->stored);
}

/* Return the final path component, accepting either separator. */
static const char *base_name(const char *path)
{
    const char *name = path;
    for (const char *c = path; *c; ++c) if (*c == '/' || *c == '\\') name = c + 1;
    return name;
}

/* Remove a store file this importer created when no record ended up owning it. */
static void discard_store(Fl_AssetImporter *im, const Fl_ImportRequest *r)
{
    char path[FL_PATH_MAX];
    if (r->stored.created && !fl_project_asset_by_hash(im->project, r->stored.hash) &&
        fl_asset_store_path(r->directory, r->stored.hash, r->stored.extension, path, sizeof(path))) fl_fs_remove(path);
}

/* Record a finished request on the owner thread and announce it. */
static void finish(Fl_AssetImporter *im, Fl_ImportRequest *r)
{
    Fl_AssetImported event = {im, r->id, r->status, 0, {0}};
    const char *name = base_name(r->source);
    snprintf(event.name, sizeof(event.name), "%s", name[0] ? name : r->source);
    if (r->status == FL_IMPORT_ADDED) {
        const Fl_Asset *existing = fl_project_asset_by_hash(im->project, r->stored.hash);
        if (existing) { event.status = FL_IMPORT_EXISTING; event.asset = existing->id; }
        else {
            char display[256];
            snprintf(display, sizeof(display), "%s", event.name);
            char *dot = strrchr(display, '.');
            if (dot && dot != display) *dot = 0;
            Fl_Asset record = {.name = display, .kind = r->stored.kind, .extension = r->stored.extension,
                               .size = r->stored.size, .width = r->stored.width, .height = r->stored.height};
            memcpy(record.hash, r->stored.hash, 32);
            if (!fl_project_apply(im->project, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &event.asset)) {
                event.status = FL_IMPORT_FAILED; discard_store(im, r);
            }
        }
    }
    if (event.status == FL_IMPORT_UNSUPPORTED) FL_LOG_WARN(LOG_TAG, "unsupported file type: %s", event.name);
    else if (event.status == FL_IMPORT_FAILED) FL_LOG_ERROR(LOG_TAG, "could not import %s", event.name);
    else FL_LOG_INFO(LOG_TAG, "%s %s", event.status == FL_IMPORT_ADDED ? "imported" : "already in project:", event.name);
    fl_event_publish(im->bus, im->imported, &event, sizeof(event));
}

/* Match a job completion to one of this importer's requests. */
static bool job_done(const Fl_Event *event, void *data)
{
    Fl_AssetImporter *im = data;
    const Fl_JobDoneEvent *done = event->payload;
    for (size_t i = 0; i < im->count; ++i) {
        Fl_ImportRequest *r = im->requests[i];
        if (r->job.index != done->job.index || r->job.generation != done->job.generation) continue;
        im->requests[i] = im->requests[--im->count];
        finish(im, r); free(r);
        break;
    }
    return false;
}

Fl_AssetImporter *fl_asset_importer_create(Fl_Project *project, Fl_JobSystem *jobs, Fl_EventBus *bus)
{
    if (!project || !jobs || !bus) return NULL;
    Fl_AssetImporter *im = calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->project = project; im->jobs = jobs; im->bus = bus; im->next_request = 1;
    im->imported = fl_event_type_register(bus, FL_EVENT_ASSET_IMPORTED, sizeof(Fl_AssetImported));
    if (im->imported == FL_EVENT_TYPE_INVALID || !fl_event_subscribe(bus, &(Fl_SubscribeDesc){
            .type = fl_jobs_done_event(jobs), .fn = job_done, .user_data = im, .owner = im}).generation) {
        free(im); return NULL;
    }
    return im;
}

void fl_asset_importer_destroy(Fl_AssetImporter *im)
{
    if (!im) return;
    fl_event_unsubscribe_owner(im->bus, im);
    for (size_t i = 0; i < im->count; ++i) {
        fl_job_wait(im->jobs, im->requests[i]->job);
        if (im->requests[i]->status == FL_IMPORT_ADDED) discard_store(im, im->requests[i]);
        free(im->requests[i]);
    }
    free(im->requests); free(im);
}

uint64_t fl_asset_import(Fl_AssetImporter *im, const char *source)
{
    const char *directory = fl_project_directory(im ? im->project : NULL);
    if (!im || !source || !source[0] || !directory || strlen(source) >= FL_PATH_MAX || strlen(directory) >= FL_PATH_MAX) return 0;
    if (im->count == im->capacity) {
        size_t capacity = im->capacity ? im->capacity * 2 : 8;
        Fl_ImportRequest **requests = realloc(im->requests, capacity * sizeof(*requests));
        if (!requests) return 0;
        im->requests = requests; im->capacity = capacity;
    }
    Fl_ImportRequest *r = calloc(1, sizeof(*r));
    if (!r) return 0;
    r->id = im->next_request++;
    snprintf(r->directory, sizeof(r->directory), "%s", directory);
    snprintf(r->source, sizeof(r->source), "%s", source);
    r->status = FL_IMPORT_FAILED;
    r->job = fl_job_submit(im->jobs, &(Fl_JobDesc){.fn = import_job, .data = r, .priority = FL_JOB_PRIORITY_NORMAL, .notify = true});
    if (!r->job.generation) { free(r); return 0; }
    im->requests[im->count++] = r;
    return r->id;
}

size_t fl_asset_importer_pending(const Fl_AssetImporter *im) { return im ? im->count : 0; }
