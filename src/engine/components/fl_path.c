#include "fl_path.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void put_double(unsigned char *out, double value)
{
    uint64_t bits; memcpy(&bits, &value, 8);
    for (unsigned i = 0; i < 8; ++i) out[i] = (unsigned char)(bits >> (8 * i));
}
static bool get_double(const unsigned char *in, double *value)
{
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= (uint64_t)in[i] << (8 * i);
    memcpy(value, &bits, 8);
    return isfinite(*value);
}

Fl_Path fl_path_default(double width, double height)
{
    Fl_Path path = {.trim_end = 1, .count = 2};
    path.points[0] = (Fl_PathPoint){FL_PATH_MOVE, 0, height, 0, 0, 0, 0};
    path.points[1] = (Fl_PathPoint){FL_PATH_CUBIC, width, 0, width * 0.3, height, width * 0.7, 0};
    return path;
}

size_t fl_path_encode(const Fl_Path *path, unsigned char *out, size_t capacity)
{
    if (!path || !out || path->count > FL_CURVE_MAX || path->trim_start < 0 || path->trim_end > 1 || path->trim_start > path->trim_end) return 0;
    size_t size = 24 + (size_t)path->count * 49;
    if (capacity < size) return 0;
    memset(out, 0, size);
    put_double(out, path->trim_start); put_double(out + 8, path->trim_end);
    for (unsigned i = 0; i < 4; ++i) out[16 + i] = (unsigned char)(path->count >> (8 * i));
    for (uint32_t i = 0; i < path->count; ++i) {
        unsigned char *p = out + 24 + (size_t)i * 49;
        const Fl_PathPoint *pt = &path->points[i];
        if (pt->verb < FL_PATH_MOVE || pt->verb > FL_PATH_CUBIC) return 0;
        p[0] = pt->verb;
        put_double(p + 1, pt->x); put_double(p + 9, pt->y); put_double(p + 17, pt->c1x);
        put_double(p + 25, pt->c1y); put_double(p + 33, pt->c2x); put_double(p + 41, pt->c2y);
    }
    return size;
}

bool fl_path_decode(const Fl_Component *c, Fl_Path *out)
{
    if (!c || !out || c->type != FL_COMPONENT_PATH || c->version != 1 || !c->data || c->size < 24) return false;
    Fl_Path path = {0};
    if (!get_double(c->data, &path.trim_start) || !get_double(c->data + 8, &path.trim_end)) return false;
    uint32_t count = 0;
    for (unsigned i = 0; i < 4; ++i) count |= (uint32_t)c->data[16 + i] << (8 * i);
    if (count > FL_CURVE_MAX || path.trim_start < 0 || path.trim_end > 1 || path.trim_start > path.trim_end + 1e-9) return false;
    if (c->size != 24 + (size_t)count * 49) return false;
    path.count = count;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *p = c->data + 24 + (size_t)i * 49;
        Fl_PathPoint *pt = &path.points[i];
        pt->verb = p[0];
        if (pt->verb < FL_PATH_MOVE || pt->verb > FL_PATH_CUBIC) return false;
        if (!get_double(p + 1, &pt->x) || !get_double(p + 9, &pt->y) || !get_double(p + 17, &pt->c1x) ||
            !get_double(p + 25, &pt->c1y) || !get_double(p + 33, &pt->c2x) || !get_double(p + 41, &pt->c2y)) return false;
        if (fabs(pt->x) > 1e6 || fabs(pt->y) > 1e6) return false;
    }
    *out = path;
    return true;
}

typedef struct Seg { double x0, y0, x1, y1, len; } Seg;

static void flatten(const Fl_Path *path, Seg *segs, size_t *count, size_t cap)
{
    double cx = 0, cy = 0, sx = 0, sy = 0;
    bool have = false;
    for (uint32_t i = 0; i < path->count; ++i) {
        const Fl_PathPoint *p = &path->points[i];
        if (p->verb == FL_PATH_MOVE) {cx = sx = p->x; cy = sy = p->y; have = true; continue;}
        if (!have) continue;
        if (p->verb == FL_PATH_LINE) {
            if (*count < cap) segs[(*count)++] = (Seg){cx, cy, p->x, p->y, hypot(p->x - cx, p->y - cy)};
            cx = p->x; cy = p->y;
        } else {
            double x0 = cx, y0 = cy;
            for (int s = 1; s <= 12; ++s) {
                double t = s / 12.0, u = 1 - t;
                double x = u*u*u*x0 + 3*u*u*t*p->c1x + 3*u*t*t*p->c2x + t*t*t*p->x;
                double y = u*u*u*y0 + 3*u*u*t*p->c1y + 3*u*t*t*p->c2y + t*t*t*p->y;
                if (*count < cap) segs[(*count)++] = (Seg){cx, cy, x, y, hypot(x - cx, y - cy)};
                cx = x; cy = y;
            }
        }
    }
    (void)sx; (void)sy;
}

static void trim_segs(Seg *segs, size_t count, double start, double end, Seg *out, size_t *out_count)
{
    double total = 0;
    for (size_t i = 0; i < count; ++i) total += segs[i].len;
    double a = start * total, b = end * total, cursor = 0;
    *out_count = 0;
    for (size_t i = 0; i < count; ++i) {
        double next = cursor + segs[i].len;
        if (next > a && cursor < b && segs[i].len > 1e-8) {
            double t0 = fmax(0, (a - cursor) / segs[i].len), t1 = fmin(1, (b - cursor) / segs[i].len);
            if (t1 > t0) {
                double x0 = segs[i].x0 + (segs[i].x1 - segs[i].x0) * t0, y0 = segs[i].y0 + (segs[i].y1 - segs[i].y0) * t0;
                double x1 = segs[i].x0 + (segs[i].x1 - segs[i].x0) * t1, y1 = segs[i].y0 + (segs[i].y1 - segs[i].y0) * t1;
                out[(*out_count)++] = (Seg){x0, y0, x1, y1, hypot(x1 - x0, y1 - y0)};
            }
        }
        cursor = next;
    }
}

unsigned char *fl_path_raster(const Fl_Path *path, double width, double height, double stroke, uint32_t *out_w, uint32_t *out_h)
{
    if (!path || !out_w || !out_h || width < 1 || height < 1) return NULL;
    double scale = fmin(1.0, 512.0 / fmax(width, height));
    uint32_t w = (uint32_t)fmax(1, ceil(width * scale)), h = (uint32_t)fmax(1, ceil(height * scale));
    Seg raw[FL_CURVE_MAX * 12], kept[FL_CURVE_MAX * 12];
    size_t raw_count = 0, kept_count = 0;
    flatten(path, raw, &raw_count, FL_CURVE_MAX * 12);
    trim_segs(raw, raw_count, path->trim_start, path->trim_end, kept, &kept_count);
    unsigned char *pixels = calloc((size_t)w * h, 2);
    if (!pixels) return NULL;
    double sw = stroke > 0 ? fmax(stroke * scale, 1.0) : 0;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        double px = ((double)x + 0.5) / scale, py = ((double)y + 0.5) / scale, best = 1e9;
        int wind = 0;
        for (size_t i = 0; i < kept_count; ++i) {
            double x0 = kept[i].x0, y0 = kept[i].y0, x1 = kept[i].x1, y1 = kept[i].y1;
            double vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0;
            double den = vx * vx + vy * vy;
            double t = den > 1e-12 ? fmin(1, fmax(0, (wx * vx + wy * vy) / den)) : 0;
            double dx = px - (x0 + vx * t), dy = py - (y0 + vy * t);
            best = fmin(best, sqrt(dx * dx + dy * dy));
            if ((y0 <= py && y1 > py) || (y1 <= py && y0 > py)) {
                double ix = x0 + (py - y0) * vx / (y1 - y0);
                if (px < ix) wind ^= 1;
            }
        }
        double edge = sw > 0 && best < sw * 0.5 + 1 ? 1 - fmax(0, best - sw * 0.5) : 0;
        if (edge < 0) edge = 0;
        if (edge > 1) edge = 1;
        unsigned char *pxl = pixels + ((size_t)y * w + x) * 2;
        pxl[0] = wind ? 255 : 0;
        pxl[1] = (unsigned char)lround(edge * 255);
    }
    *out_w = w; *out_h = h;
    return pixels;
}

cJSON *fl_path_write(Fl_Json *c, const Fl_Component *component)
{
    Fl_Path path;
    if (!fl_path_decode(component, &path)) {fl_json_fail(c, "Invalid path"); return NULL;}
    cJSON *o = cJSON_CreateObject(), *commands = cJSON_CreateArray();
    fl_json_add(c, o, "trim_start", cJSON_CreateNumber(path.trim_start));
    fl_json_add(c, o, "trim_end", cJSON_CreateNumber(path.trim_end));
    for (uint32_t i = 0; i < path.count; ++i) {
        const Fl_PathPoint *pt = &path.points[i];
        cJSON *item = cJSON_CreateObject();
        fl_json_add(c, item, "op", cJSON_CreateString(pt->verb == FL_PATH_MOVE ? "M" : pt->verb == FL_PATH_LINE ? "L" : "C"));
        fl_json_add(c, item, "x", cJSON_CreateNumber(pt->x)); fl_json_add(c, item, "y", cJSON_CreateNumber(pt->y));
        if (pt->verb == FL_PATH_CUBIC) {
            fl_json_add(c, item, "c1x", cJSON_CreateNumber(pt->c1x)); fl_json_add(c, item, "c1y", cJSON_CreateNumber(pt->c1y));
            fl_json_add(c, item, "c2x", cJSON_CreateNumber(pt->c2x)); fl_json_add(c, item, "c2y", cJSON_CreateNumber(pt->c2y));
        }
        fl_json_add(c, commands, NULL, item);
    }
    fl_json_add(c, o, "commands", commands);
    return o;
}

bool fl_path_read(Fl_Json *c, const cJSON *object, Fl_Component *out)
{
    const cJSON *commands = fl_json_member(c, object, "commands", cJSON_Array);
    double start = 0, end = 1;
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(object, "trim_start"), *te = cJSON_GetObjectItemCaseSensitive(object, "trim_end");
    if (ts && !fl_json_number(c, ts, &start)) return false;
    if (te && !fl_json_number(c, te, &end)) return false;
    if (!commands) return false;
    Fl_Path path = {.trim_start = start, .trim_end = end};
    for (const cJSON *item = commands->child; item; item = item->next) {
        if (path.count >= FL_CURVE_MAX || !cJSON_IsObject(item)) return fl_json_fail(c, "Invalid path command");
        const cJSON *op = fl_json_member(c, item, "op", cJSON_String);
        Fl_PathPoint *pt = &path.points[path.count];
        if (!op || !fl_json_number(c, fl_json_member(c, item, "x", cJSON_Number), &pt->x) || !fl_json_number(c, fl_json_member(c, item, "y", cJSON_Number), &pt->y)) return false;
        if (!strcmp(op->valuestring, "M")) pt->verb = FL_PATH_MOVE;
        else if (!strcmp(op->valuestring, "L")) pt->verb = FL_PATH_LINE;
        else if (!strcmp(op->valuestring, "C")) {
            pt->verb = FL_PATH_CUBIC;
            if (!fl_json_number(c, fl_json_member(c, item, "c1x", cJSON_Number), &pt->c1x) || !fl_json_number(c, fl_json_member(c, item, "c1y", cJSON_Number), &pt->c1y) ||
                !fl_json_number(c, fl_json_member(c, item, "c2x", cJSON_Number), &pt->c2x) || !fl_json_number(c, fl_json_member(c, item, "c2y", cJSON_Number), &pt->c2y)) return false;
        } else return fl_json_fail(c, "Path op must be M, L or C");
        ++path.count;
    }
    unsigned char *bytes = malloc(24 + (size_t)path.count * 49);
    size_t size = bytes ? fl_path_encode(&path, bytes, 24 + (size_t)path.count * 49) : 0;
    if (!size) {free(bytes); return fl_json_fail(c, "Invalid path");}
    out->data = bytes; out->size = size; out->version = 1;
    return true;
}
