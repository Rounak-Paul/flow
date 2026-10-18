#include "fl_canvas_view.h"
#include <math.h>

Fl_CanvasMapping fl_canvas_mapping(Fl_SceneFrame target, double width, double height, double margin)
{
    if (!isfinite(width) || !isfinite(height) || !isfinite(margin) || margin < 0 ||
        !isfinite(target.x) || !isfinite(target.y) ||
        !isfinite(target.width) || !isfinite(target.height) || target.width <= 0 || target.height <= 0 ||
        width <= 2 * margin || height <= 2 * margin) return (Fl_CanvasMapping){0};
    double scale = fmin((width - 2 * margin) / target.width, (height - 2 * margin) / target.height);
    double w = width / scale, h = height / scale;
    Fl_SceneFrame world = {target.x + target.width * .5 - w * .5,
        target.y + target.height * .5 - h * .5, w, h};
    if (!isfinite(w) || !isfinite(h) || !isfinite(world.x) || !isfinite(world.y)) return (Fl_CanvasMapping){0};
    target.x = world.x + (world.width - target.width) * .5;
    target.y = world.y + (world.height - target.height) * .5;
    return (Fl_CanvasMapping){.world=world, .target=target, .scale=scale};
}
