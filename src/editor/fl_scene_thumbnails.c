#include "doc/fl_scene_frame.h"
#include "fl_scene_thumbnails.h"
#include "style.h"
#include "schema/fl_widgets.h"
#include "doc/fl_scene_geometry.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES_PER_AXIS 2

typedef struct Fl_ThumbnailEntry {
    Fl_Id scene;
    uint64_t revision;
    Ca_Image *image;
} Fl_ThumbnailEntry;
struct Fl_SceneThumbnails {
    Ca_Instance *instance;
    Fl_ThumbnailEntry *entries;
    size_t count, capacity;
};

typedef struct Fl_Artboard { double x, y, scale; } Fl_Artboard;

Fl_SceneThumbnails *fl_scene_thumbnails_create(Ca_Instance *instance)
{
    if (!instance) return NULL;
    Fl_SceneThumbnails *t = calloc(1, sizeof(*t));
    if (t) t->instance = instance;
    return t;
}

void fl_scene_thumbnails_destroy(Fl_SceneThumbnails *t)
{
    if (!t) return;
    for (size_t i = 0; i < t->count; ++i) ca_image_destroy(t->instance, t->entries[i].image);
    free(t->entries); free(t);
}

/* Blend one shape sample of the given coverage over an sRGB pixel. */
static void blend(unsigned char *pixel, const unsigned char colour[4], double opacity, double coverage)
{
    double a = colour[3] / 255.0 * opacity * coverage;
    for (int c = 0; c < 3; ++c) pixel[c] = (unsigned char)lround(pixel[c] + (colour[c] - pixel[c]) * a);
}

/* Fraction of a pixel's sample grid that lies inside the shape. */
static double coverage(const Fl_ResolvedEntity *v, Fl_WidgetKind kind, const Fl_Artboard *board,
                       const Fl_SceneFrame *frame, unsigned px, unsigned py)
{
    unsigned inside = 0;
    for (unsigned sy = 0; sy < SAMPLES_PER_AXIS; ++sy) for (unsigned sx = 0; sx < SAMPLES_PER_AXIS; ++sx) {
        double x = (px + (sx + .5) / SAMPLES_PER_AXIS - board->x) / board->scale;
        double y = (py + (sy + .5) / SAMPLES_PER_AXIS - board->y) / board->scale;
        if (x < frame->x || y < frame->y || x > frame->x + frame->width || y > frame->y + frame->height) continue;
        double lx, ly;
        fl_affine_point(v->inverse, x, y, &lx, &ly);
        if (lx < 0 || ly < 0 || lx > v->local.width || ly > v->local.height) continue;
        if (kind == FL_WIDGET_ELLIPSE || fl_widget_is_polygon(kind)) {
            if (!fl_widget_shape_contains(kind, lx / v->local.width * 2 - 1, ly / v->local.height * 2 - 1)) continue;
        }
        ++inside;
    }
    return (double)inside / (SAMPLES_PER_AXIS * SAMPLES_PER_AXIS);
}

/* Pixel-space bounds of a shape's transformed box, clipped to the canvas. */
static bool shape_bounds(const Fl_ResolvedEntity *v, const Fl_Artboard *board, unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1)
{
    double min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (unsigned corner = 0; corner < 4; ++corner) {
        double x, y;
        fl_affine_point(v->world, (corner & 1) * v->local.width, (corner >> 1) * v->local.height, &x, &y);
        x = x * board->scale + board->x; y = y * board->scale + board->y;
        min_x = fmin(min_x, x); max_x = fmax(max_x, x); min_y = fmin(min_y, y); max_y = fmax(max_y, y);
    }
    if (!isfinite(min_x + max_x + min_y + max_y) || max_x < 0 || max_y < 0 || min_x >= FL_STYLE_SCENE_THUMB_W || min_y >= FL_STYLE_SCENE_THUMB_H)
        return false;
    *x0 = (unsigned)fmax(floor(min_x), 0); *y0 = (unsigned)fmax(floor(min_y), 0);
    *x1 = (unsigned)fmin(ceil(max_x), FL_STYLE_SCENE_THUMB_W); *y1 = (unsigned)fmin(ceil(max_y), FL_STYLE_SCENE_THUMB_H);
    return true;
}

/* Darken the lower part of the canvas so card text stays legible over any scene. */
static void apply_scrim(unsigned char *pixels)
{
    for (unsigned y = 0; y < FL_STYLE_SCENE_THUMB_H; ++y) {
        double t = ((double)y / FL_STYLE_SCENE_THUMB_H - FL_STYLE_SCENE_THUMB_SCRIM_START) / (1 - FL_STYLE_SCENE_THUMB_SCRIM_START);
        if (t <= 0) continue;
        double keep = 1 - FL_STYLE_SCENE_THUMB_SCRIM_STRENGTH * t * t * (3 - 2 * t);
        unsigned char *row = pixels + (size_t)y * FL_STYLE_SCENE_THUMB_W * 4;
        for (unsigned x = 0; x < FL_STYLE_SCENE_THUMB_W; ++x) for (int c = 0; c < 3; ++c)
            row[x * 4 + c] = (unsigned char)lround(row[x * 4 + c] * keep);
    }
}

/* Rasterize a scene's filled shapes as of its first frame, aspect-fit and centred on the canvas. */
static unsigned char *rasterize(const Fl_Scene *scene)
{
    if (!scene->width || !scene->height) return NULL;
    size_t bytes = (size_t)FL_STYLE_SCENE_THUMB_W * FL_STYLE_SCENE_THUMB_H * 4;
    unsigned char *pixels = malloc(bytes);
    if (!pixels) return NULL;
    const unsigned char canvas[4] = FL_STYLE_SCENE_THUMB_CANVAS_RGBA, artboard[4] = FL_STYLE_SCENE_THUMB_ARTBOARD_RGBA;
    for (size_t i = 0; i < bytes; i += 4) memcpy(pixels + i, canvas, 4);
    Fl_SceneFrame frame = fl_scene_frame(scene);
    double sw = frame.width, sh = frame.height;
    Fl_Artboard board = {.scale = fmin(FL_STYLE_SCENE_THUMB_W / sw, FL_STYLE_SCENE_THUMB_H / sh)};
    board.x = (FL_STYLE_SCENE_THUMB_W - sw * board.scale) * .5; board.y = (FL_STYLE_SCENE_THUMB_H - sh * board.scale) * .5;
    unsigned ax0 = (unsigned)lround(board.x), ay0 = (unsigned)lround(board.y);
    unsigned ax1 = FL_STYLE_SCENE_THUMB_W - ax0, ay1 = FL_STYLE_SCENE_THUMB_H - ay0;
    for (unsigned y = ay0; y < ay1; ++y) for (unsigned x = ax0; x < ax1; ++x)
        memcpy(pixels + ((size_t)y * FL_STYLE_SCENE_THUMB_W + x) * 4, artboard, 4);

    board.x -= frame.x * board.scale; board.y -= frame.y * board.scale;
    Fl_SceneState state = {0};
    const Fl_SceneState *first_frame = fl_scene_state_update(&state, scene, 0) ? &state : NULL;
    Fl_SceneGeometry geometry = {0};
    if (fl_scene_geometry_update(&geometry, scene, first_frame)) {
        const unsigned char placeholder[4] = FL_STYLE_IMAGE_PLACEHOLDER_RGBA;
        for (size_t i = 0; i < geometry.count; ++i) {
            size_t index = geometry.order[i];
            const Fl_ResolvedEntity *v = &geometry.entities[index];
            Fl_WidgetKind kind = v->kind;
            if (!v->visible || !v->invertible || v->opacity <= 0 || v->local.width <= 0 || v->local.height <= 0) continue;
            if (kind != FL_WIDGET_RECTANGLE && kind != FL_WIDGET_ELLIPSE && kind != FL_WIDGET_IMAGE && !fl_widget_is_polygon(kind)) continue;
            unsigned x0, y0, x1, y1;
            if (!shape_bounds(v, &board, &x0, &y0, &x1, &y1)) continue;
            Fl_Appearance look = first_frame ? first_frame->entities[index].appearance : fl_widget_appearance(&scene->entities[index]);
            const unsigned char *fill = kind == FL_WIDGET_IMAGE ? placeholder : look.fill;
            for (unsigned y = y0; y < y1; ++y) for (unsigned x = x0; x < x1; ++x) {
                double cover = coverage(v, kind, &board, &frame, x, y);
                if (cover > 0) blend(pixels + ((size_t)y * FL_STYLE_SCENE_THUMB_W + x) * 4, fill, v->opacity, cover);
            }
        }
    }
    fl_scene_geometry_clear(&geometry);
    fl_scene_state_clear(&state);
    apply_scrim(pixels);
    return pixels;
}

Ca_Image *fl_scene_thumbnails_get(Fl_SceneThumbnails *t, const Fl_Scene *scene)
{
    if (!t || !scene) return NULL;
    Fl_ThumbnailEntry *entry = NULL;
    for (size_t i = 0; i < t->count && !entry; ++i) if (t->entries[i].scene == scene->id) entry = &t->entries[i];
    if (entry && entry->revision == scene->revision) return entry->image;
    if (!entry) {
        if (t->count == t->capacity) {
            size_t capacity = t->capacity ? t->capacity * 2 : 8;
            if (capacity > SIZE_MAX / sizeof(*t->entries)) return NULL;
            Fl_ThumbnailEntry *grown = realloc(t->entries, capacity * sizeof(*grown));
            if (!grown) return NULL;
            t->entries = grown; t->capacity = capacity;
        }
        entry = &t->entries[t->count++];
        *entry = (Fl_ThumbnailEntry){.scene = scene->id};
    }
    unsigned char *pixels = rasterize(scene);
    Ca_Image *image = pixels ? ca_image_create(t->instance, pixels, FL_STYLE_SCENE_THUMB_W, FL_STYLE_SCENE_THUMB_H) : NULL;
    free(pixels);
    if (!image) return entry->image;
    ca_image_destroy(t->instance, entry->image);
    entry->image = image; entry->revision = scene->revision;
    return image;
}

void fl_scene_thumbnails_retain(Fl_SceneThumbnails *t, const Fl_Project *project)
{
    if (!t || !project) return;
    size_t kept = 0;
    for (size_t i = 0; i < t->count; ++i) {
        if (fl_project_scene(project, t->entries[i].scene)) t->entries[kept++] = t->entries[i];
        else ca_image_destroy(t->instance, t->entries[i].image);
    }
    t->count = kept;
}
