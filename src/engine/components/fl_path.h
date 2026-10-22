#pragma once
#include "fl_component.h"
#include "doc/fl_json.h"
#include <stdint.h>

#define FL_CURVE_MAX 48u

typedef enum Fl_PathVerb { FL_PATH_MOVE = 1, FL_PATH_LINE = 2, FL_PATH_CUBIC = 3 } Fl_PathVerb;

typedef struct Fl_PathPoint {
    unsigned char verb;
    double x, y, c1x, c1y, c2x, c2y;
} Fl_PathPoint;

/* Local-pixel commands. trim_start/trim_end are fractions of the flattened length, 0..1. */
typedef struct Fl_Path {
    double trim_start, trim_end;
    uint32_t count;
    Fl_PathPoint points[FL_CURVE_MAX];
} Fl_Path;

/* A stroked curve across a box, fully visible. */
Fl_Path fl_path_default(double width, double height);
size_t fl_path_encode(const Fl_Path *path, unsigned char *out, size_t capacity);
bool fl_path_decode(const Fl_Component *component, Fl_Path *out);
/* Rasterise fill and stroke into an 8-bit coverage mask. Caller frees the result. */
unsigned char *fl_path_raster(const Fl_Path *path, double width, double height, double stroke, uint32_t *out_w, uint32_t *out_h);

cJSON *fl_path_write(Fl_Json *context, const Fl_Component *component);
bool fl_path_read(Fl_Json *context, const cJSON *object, Fl_Component *out);
