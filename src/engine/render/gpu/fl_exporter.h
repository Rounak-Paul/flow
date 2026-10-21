#pragma once
#include "core/fl_event.h"
#include "render/gpu/fl_scene_renderer.h"
#include "platform/fl_paths.h"

/*
 * Video export: renders the project's scenes in order through the scene renderer and pipes the frames to an
 * ffmpeg encoder (H.264 in MP4, BT.709, yuv420p). Work is time-sliced on the UI thread through the application
 * bus; one export runs at a time. Captures immutable documents, verified media copies and owned font bytes.
 * Encoder backpressure and process completion run on a dedicated worker with a bounded frame mailbox.
 * Output replaces the destination only after successful encoding. Public API is UI thread only.
 */
typedef struct Fl_Exporter Fl_Exporter;

typedef enum Fl_ExportState { FL_EXPORT_IDLE, FL_EXPORT_RUNNING, FL_EXPORT_DONE, FL_EXPORT_FAILED, FL_EXPORT_CANCELLED } Fl_ExportState;

typedef struct Fl_ExportStatus {
    Fl_ExportState state;
    uint64_t frames_done, frames_total;
    uint32_t width, height, fps;
    uint32_t crf;               /* value sent to the encoder; a request of 0 is reported as 16 */
    double crossfade;           /* seconds applied between scenes; 0 when a single scene ignores it */
    size_t scene_count;
    double elapsed_seconds;
    char path[FL_PATH_MAX];
    char message[256];          /* failure reason, or a summary when done */
} Fl_ExportStatus;

/* Latest encoded picture, downscaled for display. Pixels are borrowed and stay valid until the next
   preview update, the next export start, or fl_exporter_destroy. */
typedef struct Fl_ExportPreview {
    const unsigned char *rgba;
    uint32_t width, height;
    uint64_t sequence;          /* increases whenever the pixels change; never 0 when rgba is set */
} Fl_ExportPreview;

typedef struct Fl_ExportDesc {
    const char *path;           /* output .mp4; its folder must exist */
    uint32_t width;             /* pixels; height follows the scenes' aspect. Both must be even */
    uint32_t fps;               /* 1..120 */
    uint32_t crf;               /* x264 quality, 0 (lossless) .. 51; 0 selects 16 */
    const Fl_Id *scenes;        /* scenes to export in order; NULL exports every scene */
    size_t scene_count;
    double crossfade;           /* seconds each scene dissolves into the next, overlapping them; 0 cuts. Shorter than every scene */
} Fl_ExportDesc;

/* Create an exporter drawing with renderer (borrowed, outlives it). NULL on failure. */
Fl_Exporter *fl_exporter_create(Fl_EventBus *bus, Fl_SceneRenderer *renderer);
/* Abort any running export and free. NULL is safe. */
void fl_exporter_destroy(Fl_Exporter *exporter);
/* Begin exporting project. False with a reason in error when the request is invalid, ffmpeg is missing or one is running. */
bool fl_exporter_start(Fl_Exporter *exporter, Fl_Project *project, const Fl_ExportDesc *desc, char *error, size_t error_size);
/* Stop a running export, discarding the partial file's completion. No effect when idle. */
void fl_exporter_cancel(Fl_Exporter *exporter);
/* Fail a running export because its project is going away. */
void fl_exporter_abort(Fl_Exporter *exporter, const char *reason);
/* Snapshot of the latest export. Elapsed time advances while the export is running. */
Fl_ExportStatus fl_exporter_status(const Fl_Exporter *exporter);
/* Borrow the latest encoded frame. False when no frame has been produced yet. */
bool fl_exporter_preview(const Fl_Exporter *exporter, Fl_ExportPreview *preview);
