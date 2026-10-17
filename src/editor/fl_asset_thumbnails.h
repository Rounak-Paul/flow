#pragma once
#include "core/fl_job.h"
#include "project/fl_project.h"
#include <causality.h>

/*
 * Asset preview images for the editor UI. Previews are generated (or read
 * from the project's thumbnail cache) on the job system and uploaded as
 * Causality images on the owner thread; the ready callback then lets the
 * owner rebuild. Images live until the cache is destroyed, so retained UI
 * nodes never reference a freed image.
 */
typedef struct Fl_AssetThumbnails Fl_AssetThumbnails;
/* Notifies the owner that a requested preview finished (successfully or not). */
typedef void (*Fl_ThumbnailsReadyFn)(void *user_data);

/* Create a cache borrowing instance, project, jobs and bus; all required. Owner thread only. */
Fl_AssetThumbnails *fl_asset_thumbnails_create(Ca_Instance *instance, Fl_Project *project, Fl_JobSystem *jobs,
                                               Fl_EventBus *bus, Fl_ThumbnailsReadyFn ready, void *ready_data);
/* Wait for in-flight previews, then destroy every image. Call after the UI using them is gone. */
void fl_asset_thumbnails_destroy(Fl_AssetThumbnails *thumbnails);
/* Return asset's preview (requesting it on first use) and its pixel size; NULL while pending or without a visual. */
Ca_Image *fl_asset_thumbnails_get(Fl_AssetThumbnails *thumbnails, const Fl_Asset *asset, uint32_t *width, uint32_t *height);
