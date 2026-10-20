#pragma once
#include "core/fl_job.h"
#include "project/fl_project.h"
#include "platform/fl_paths.h"

typedef enum Fl_ImportStatus {
    FL_IMPORT_ADDED,            /* new asset record created */
    FL_IMPORT_EXISTING,         /* identical content already imported; asset is the existing record */
    FL_IMPORT_UNSUPPORTED,      /* not a recognised source asset or media file */
    FL_IMPORT_FAILED            /* unreadable source, storage failure, or rejected operation */
} Fl_ImportStatus;

/* Imported resource metadata plus privately owned staging bytes for editable source assets. */
typedef struct Fl_StoredAsset {
    Fl_AssetKind kind;
    unsigned char hash[32];
    char extension[FL_ASSET_EXTENSION_MAX + 1];
    uint64_t size, width, height;
    uint64_t duration;          /* ticks (FL_TICKS_PER_SECOND) for audio/video; zero when unknown */
    uint32_t sample_rate, channels;
    char *source;               /* owned editable source staging bytes; record consumes, caller frees if abandoned */
    bool created;               /* this call created the store file */
} Fl_StoredAsset;

/*
 * Hash and validate an import on any thread. Media is stored at assets/<sha256>.<ext>.
 * Editable documents return owned source bytes instead of a shared staging file; fl_asset_record consumes
 * them, or the caller must free out->source when abandoning the result. Touches no project state.
 */
Fl_ImportStatus fl_asset_store_file(const char *directory, const char *source, Fl_StoredAsset *out);

/* Record through the shared project owner; consumes stored->source on success or failure, retaining metadata. */
Fl_ImportStatus fl_asset_record(Fl_Project *project, Fl_StoredAsset *stored, const char *name, Fl_Id *out_id);

/*
 * Asynchronous importer: files are stored on worker threads, then recorded
 * through shared asset operations on the owner thread. Each request ends with exactly
 * one FL_EVENT_ASSET_IMPORTED.
 */
typedef struct Fl_AssetImporter Fl_AssetImporter;
#define FL_EVENT_ASSET_IMPORTED "asset.imported"
typedef struct Fl_AssetImported {
    Fl_AssetImporter *importer;
    uint64_t request;           /* value returned by fl_asset_import */
    Fl_ImportStatus status;
    Fl_Id asset;                /* zero unless ADDED or EXISTING */
    char name[128];             /* source file name, truncated */
} Fl_AssetImported;

/* Create an importer for project; all arguments are borrowed and required. Owner thread only. */
Fl_AssetImporter *fl_asset_importer_create(Fl_Project *project, Fl_JobSystem *jobs, Fl_EventBus *bus);
/* Wait for in-flight work, discard unrecorded store files and free. Must precede project destruction. */
void fl_asset_importer_destroy(Fl_AssetImporter *importer);
/* Queue source for import; returns a non-zero request id, or zero when it could not be scheduled. */
uint64_t fl_asset_import(Fl_AssetImporter *importer, const char *source);
/* Number of requests still running. */
size_t fl_asset_importer_pending(const Fl_AssetImporter *importer);
