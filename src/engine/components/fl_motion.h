#pragma once
#include "fl_component.h"

#define FL_MOTION_BYTES 40u

/* Kinetic type on one text entity. Glyphs stay in the shaped run. */
typedef struct Fl_TextMotion {
    double tracking;    /* extra pixels between glyphs */
    double stagger;     /* seconds between glyph entrances */
    double rise;        /* pixels a glyph travels as it enters */
    double fade;        /* 0 keeps the glyph opaque; 1 fades it in with the entrance */
    double duration;    /* seconds of each glyph's entrance */
} Fl_TextMotion;

Fl_TextMotion fl_motion_default(void);
void fl_motion_encode(const Fl_TextMotion *motion, unsigned char out[FL_MOTION_BYTES]);
bool fl_motion_decode(const Fl_Component *component, Fl_TextMotion *out);
bool fl_motion_active(const Fl_TextMotion *motion);
