#include "fl_frame.h"
#include "doc/fl_scene_frame.h"
#include <math.h>
#include <stdlib.h>

bool fl_frame_dimensions(const Fl_Scene *scene, uint32_t width, uint32_t *out_width, uint32_t *out_height)
{
    Fl_SceneFrame frame = fl_scene_frame(scene);
    if (!width || width > 16384 || !(frame.width > 0) || !(frame.height > 0)) return false;
    double height = frame.height * width / frame.width;
    if (!(height >= 0.5) || height > 16384) return false;
    uint32_t h = (uint32_t)lround(height);
    if (!h || (uint64_t)width * h > FL_RENDER_MAX_PIXELS) return false;
    *out_width = width; *out_height = h;
    return true;
}

void fl_frame_free(Fl_Frame *frame)
{
    if (!frame) return;
    free(frame->rgba); *frame = (Fl_Frame){0};
}
