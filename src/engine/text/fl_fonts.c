#include "fl_fonts.h"
#include "text/fl_embedded_fonts.h"
#include "core/fl_log.h"
#include "platform/fl_paths.h"
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "fonts"
#define SOURCE_COUNT 3

typedef struct Fl_FontScanJob { Fl_FontCatalog *catalog; } Fl_FontScanJob;
typedef struct Fl_FamilyEntry { const char *name; Fl_FontSource source; } Fl_FamilyEntry;
struct Fl_Fonts {
    Fl_EventBus *bus;
    Fl_JobSystem *jobs;
    Fl_Project *project;
    Fl_FontCatalog *catalogs[SOURCE_COUNT];     /* indexed by Fl_FontSource */
    Fl_FontScanJob *scan;
    Fl_JobHandle scan_job;
    Fl_FamilyEntry *families;
    size_t family_count;
    uint64_t generation;
    Fl_EventType changed;
};

/* Worker body: build the system catalog without touching shared service state. */
static void scan_system(void *data)
{
    Fl_FontScanJob *job = data;
    job->catalog = fl_font_catalog_scan_system();
}

/* Merge per-source family lists, keeping the highest-precedence source for each name. */
static void rebuild_families(Fl_Fonts *f)
{
    size_t total = 0;
    for (int s = 0; s < SOURCE_COUNT; ++s) total += fl_font_catalog_family_count(f->catalogs[s]);
    Fl_FamilyEntry *merged = total ? malloc(total * sizeof(*merged)) : NULL;
    free(f->families); f->families = NULL; f->family_count = 0;
    if (total && !merged) { FL_LOG_ERROR(LOG_TAG, "out of memory listing font families"); return; }
    size_t cursor[SOURCE_COUNT] = {0}, count = 0;
    for (;;) {
        int pick = -1; const char *name = NULL;
        for (int s = 0; s < SOURCE_COUNT; ++s) {
            const char *candidate = fl_font_catalog_family(f->catalogs[s], cursor[s]);
            if (candidate && (!name || fl_font_family_compare(candidate, name) < 0)) { pick = s; name = candidate; }
        }
        if (pick < 0) break;
        merged[count++] = (Fl_FamilyEntry){name, (Fl_FontSource)pick};
        for (int s = 0; s < SOURCE_COUNT; ++s) {
            const char *candidate = fl_font_catalog_family(f->catalogs[s], cursor[s]);
            if (candidate && !fl_font_family_compare(candidate, name)) ++cursor[s];
        }
    }
    f->families = merged; f->family_count = count;
}

/* Advance the generation and notify observers after any catalog swap. */
static void announce(Fl_Fonts *f)
{
    rebuild_families(f);
    ++f->generation;
    Fl_FontsChanged change = {f, f->generation};
    fl_event_publish(f->bus, f->changed, &change, sizeof(change));
}

/* Rebuild the project catalog from the bound project's font assets. */
static void rebuild_project(Fl_Fonts *f)
{
    size_t count = 0, fonts = 0, total = fl_project_asset_count(f->project);
    for (size_t i = 0; i < total; ++i) fonts += fl_project_asset_at(f->project, i)->kind == FL_ASSET_FONT;
    char (*paths)[FL_PATH_MAX] = fonts ? malloc(fonts * sizeof(*paths)) : NULL;
    const char **list = fonts ? malloc(fonts * sizeof(*list)) : NULL;
    if (fonts && (!paths || !list)) { free(paths); free(list); FL_LOG_ERROR(LOG_TAG, "out of memory indexing project fonts"); return; }
    for (size_t i = 0; i < total && count < fonts; ++i) {
        const Fl_Asset *a = fl_project_asset_at(f->project, i);
        if (a->kind == FL_ASSET_FONT && fl_project_asset_path(f->project, a, paths[count], FL_PATH_MAX)) {
            list[count] = paths[count]; ++count;
        }
    }
    Fl_FontCatalog *catalog = count ? fl_font_catalog_from_files(list, count) : NULL;
    free(paths); free(list);
    if (count && !catalog) FL_LOG_WARN(LOG_TAG, "project fonts could not be indexed");
    fl_font_catalog_destroy(f->catalogs[FL_FONT_SOURCE_PROJECT]);
    f->catalogs[FL_FONT_SOURCE_PROJECT] = catalog;
    announce(f);
}

/* Adopt the finished system scan delivered through the job-completion event. */
static bool job_done(const Fl_Event *event, void *data)
{
    Fl_Fonts *f = data;
    const Fl_JobDoneEvent *done = event->payload;
    if (!f->scan || done->job.index != f->scan_job.index || done->job.generation != f->scan_job.generation) return false;
    Fl_FontCatalog *catalog = f->scan->catalog;
    free(f->scan); f->scan = NULL; f->scan_job = (Fl_JobHandle){0};
    if (!catalog) { FL_LOG_WARN(LOG_TAG, "system fonts could not be enumerated"); return false; }
    FL_LOG_INFO(LOG_TAG, "indexed %zu system font families (%zu faces)",
                fl_font_catalog_family_count(catalog), fl_font_catalog_face_count(catalog));
    f->catalogs[FL_FONT_SOURCE_SYSTEM] = catalog;
    announce(f);
    return false;
}

/* Track font asset additions/removals in the bound project. */
static bool project_changed(const Fl_Event *event, void *data)
{
    Fl_Fonts *f = data;
    const Fl_ProjectChange *change = event->payload;
    if (change->project != f->project) return false;
    if (change->kind == FL_OP_ASSET_REMOVE) { if (f->catalogs[FL_FONT_SOURCE_PROJECT]) rebuild_project(f); }
    else if (change->kind == FL_OP_ASSET_ADD) {
        const Fl_Asset *a = fl_project_asset(f->project, change->asset);
        if (a && a->kind == FL_ASSET_FONT) rebuild_project(f);
    }
    return false;
}

Fl_Fonts *fl_fonts_create(const Fl_FontsDesc *desc)
{
    if (!desc || !desc->bus || (desc->scan_system && !desc->jobs)) return NULL;
    Fl_Fonts *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->bus = desc->bus; f->jobs = desc->jobs;
    f->changed = fl_event_type_register(f->bus, FL_EVENT_FONTS_CHANGED, sizeof(Fl_FontsChanged));
    Fl_EventType project = fl_event_type_register(f->bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange));
    f->catalogs[FL_FONT_SOURCE_BUNDLED] = fl_font_catalog_from_memory(fl_embedded_inter_regular, fl_embedded_inter_regular_size);
    bool ok = f->changed != FL_EVENT_TYPE_INVALID && project != FL_EVENT_TYPE_INVALID &&
              fl_font_catalog_face_count(f->catalogs[FL_FONT_SOURCE_BUNDLED]) &&
              fl_event_subscribe(f->bus, &(Fl_SubscribeDesc){.type = project, .fn = project_changed, .user_data = f, .owner = f}).generation;
    if (ok && desc->scan_system) {
        ok = fl_event_subscribe(f->bus, &(Fl_SubscribeDesc){.type = fl_jobs_done_event(f->jobs), .fn = job_done,
                                                           .user_data = f, .owner = f}).generation != 0;
        f->scan = ok ? calloc(1, sizeof(*f->scan)) : NULL;
        if (f->scan) f->scan_job = fl_job_submit(f->jobs, &(Fl_JobDesc){.fn = scan_system, .data = f->scan,
                                                 .priority = FL_JOB_PRIORITY_LOW, .notify = true});
        if (!f->scan || !f->scan_job.generation) { free(f->scan); f->scan = NULL; ok = false; }
    }
    if (!ok) { fl_fonts_destroy(f); return NULL; }
    rebuild_families(f);
    return f;
}

void fl_fonts_destroy(Fl_Fonts *f)
{
    if (!f) return;
    if (f->scan) { fl_job_wait(f->jobs, f->scan_job); fl_font_catalog_destroy(f->scan->catalog); free(f->scan); }
    fl_event_unsubscribe_owner(f->bus, f);
    for (int s = 0; s < SOURCE_COUNT; ++s) fl_font_catalog_destroy(f->catalogs[s]);
    free(f->families); free(f);
}

void fl_fonts_set_project(Fl_Fonts *f, Fl_Project *project)
{
    if (!f || (f->project == project && !project)) return;
    f->project = project;
    rebuild_project(f);
}

Fl_FontMatch fl_fonts_match(const Fl_Fonts *f, const Fl_Typography *t)
{
    Fl_FontMatch match = {0};
    if (!f) return match;
    unsigned weight = t && t->weight >= 1 && t->weight <= 1000 ? t->weight : 400;
    bool italic = t && t->italic;
    for (int s = 0; t && s < SOURCE_COUNT && !match.face; ++s) {
        match.face = fl_font_catalog_match(f->catalogs[s], t->family, weight, italic);
        match.source = (Fl_FontSource)s;
    }
    match.found = match.face != NULL;
    if (!match.face) {
        match.face = fl_font_catalog_match(f->catalogs[FL_FONT_SOURCE_BUNDLED], FL_TYPOGRAPHY_DEFAULT_FAMILY, weight, italic);
        match.source = FL_FONT_SOURCE_BUNDLED;
    }
    if (match.face) {
        match.synthetic_bold = weight >= 600 && match.face->weight < 500;
        match.synthetic_italic = italic && !match.face->italic;
    }
    return match;
}

bool fl_fonts_scanning(const Fl_Fonts *f) { return f && f->scan; }
uint64_t fl_fonts_generation(const Fl_Fonts *f) { return f ? f->generation : 0; }
size_t fl_fonts_family_count(const Fl_Fonts *f) { return f ? f->family_count : 0; }
const char *fl_fonts_family(const Fl_Fonts *f, size_t index, Fl_FontSource *source)
{
    if (!f || index >= f->family_count) return NULL;
    if (source) *source = f->families[index].source;
    return f->families[index].name;
}

size_t fl_fonts_faces(const Fl_Fonts *f, const char *family, const Fl_FontFace **faces)
{
    if (faces) *faces = NULL;
    for (int s = 0; f && s < SOURCE_COUNT; ++s) {
        size_t count = fl_font_catalog_faces(f->catalogs[s], family, faces);
        if (count) return count;
    }
    return 0;
}

Fl_Fonts *fl_fonts_snapshot(const Fl_Fonts *fonts, const Fl_Typography *requests, size_t count)
{
    if (!fonts || (count && !requests) || count>=SIZE_MAX/sizeof(Fl_FontFace *)) return NULL;
    Fl_Fonts *snapshot=calloc(1,sizeof(*snapshot));
    const Fl_FontFace **faces=calloc(count+1,sizeof(*faces));
    if (!snapshot || !faces) {free(snapshot); free(faces); return NULL;}
    bool ok=true;
    for (int source=0;ok && source<SOURCE_COUNT;++source) {
        size_t selected=0;
        for (size_t i=0;i<count;++i) {
            Fl_FontMatch match=fl_fonts_match(fonts,&requests[i]);
            if (match.face && match.source==(Fl_FontSource)source) faces[selected++]=match.face;
        }
        if (source==FL_FONT_SOURCE_BUNDLED) {
            Fl_FontMatch fallback=fl_fonts_match(fonts,NULL);
            if (fallback.face) faces[selected++]=fallback.face;
        }
        snapshot->catalogs[source]=fl_font_catalog_snapshot(faces,selected);
        ok=snapshot->catalogs[source]!=NULL;
    }
    free(faces);
    if (!ok) {fl_fonts_destroy(snapshot); return NULL;}
    snapshot->generation=1; rebuild_families(snapshot); return snapshot;
}
