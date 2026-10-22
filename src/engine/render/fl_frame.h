#pragma once
#include "project/fl_project.h"

/* Missing-image placeholder colours (sRGB RGBA8) shared by every renderer. */
#define FL_RENDER_PLACEHOLDER_FILL_RGBA {38, 44, 54, 255}
#define FL_RENDER_PLACEHOLDER_STROKE_RGBA {88, 98, 114, 255}
#define FL_RENDER_PLACEHOLDER_STROKE 2.0

/* Largest frame rendered offline, in pixels, so a request cannot exhaust memory. */
#define FL_RENDER_MAX_PIXELS 8847360u

/* Rendered image: tightly packed straight-alpha RGBA8, top-left origin. Release with fl_frame_free. */
typedef struct Fl_Frame {
    unsigned char *rgba;
    uint32_t width, height;
} Fl_Frame;

/* Output size of a scene rendered width pixels wide: the height follows the first visible camera, or the canvas.
   False when the scene or width is invalid or the frame exceeds FL_RENDER_MAX_PIXELS. */
bool fl_frame_dimensions(const Fl_Scene *scene, uint32_t width, uint32_t *out_width, uint32_t *out_height);
/* Free a frame's pixels and clear it. */
void fl_frame_free(Fl_Frame *frame);
