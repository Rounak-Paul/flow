#pragma once
#include "project/fl_project.h"
#include <causality.h>

/*
 * Scene card thumbnails. Each scene's shapes are rasterized on the CPU into
 * a small image that keeps the scene's aspect ratio inside a fixed canvas,
 * and uploaded as a Causality image. Images are rebuilt only when a scene's
 * revision changes and are destroyed when their scene leaves the project or
 * the cache is destroyed. Owner thread only.
 */
typedef struct Fl_SceneThumbnails Fl_SceneThumbnails;

/*
 * Create an empty cache.
 *
 * @param instance Causality instance that owns the uploaded images; borrowed until destroy.
 * @return The cache, or NULL on allocation failure.
 */
Fl_SceneThumbnails *fl_scene_thumbnails_create(Ca_Instance *instance);

/* Destroy every image and the cache. Call after the UI referencing the images is gone. */
void fl_scene_thumbnails_destroy(Fl_SceneThumbnails *thumbnails);

/*
 * Return the thumbnail of a scene, rasterizing it when missing or stale.
 *
 * @param thumbnails Cache.
 * @param scene      Scene to draw; its id and revision key the cache.
 * @return Image owned by the cache, valid until the next call or destroy; NULL when rendering failed.
 */
Ca_Image *fl_scene_thumbnails_get(Fl_SceneThumbnails *thumbnails, const Fl_Scene *scene);

/*
 * Destroy thumbnails of scenes that no longer exist.
 *
 * @param thumbnails Cache.
 * @param project    Project whose scene list decides what stays.
 */
void fl_scene_thumbnails_retain(Fl_SceneThumbnails *thumbnails, const Fl_Project *project);
