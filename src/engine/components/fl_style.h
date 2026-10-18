#pragma once
#include "fl_component.h"

/* Horizontal placement of wrapped text inside its box. */
typedef enum Fl_TextAlign { FL_ALIGN_LEFT, FL_ALIGN_CENTER, FL_ALIGN_RIGHT, FL_ALIGN_COUNT } Fl_TextAlign;

/*
 * Optional look beyond fill and stroke: rounded corners, a two-colour linear gradient, a soft drop shadow and
 * text alignment. An entity without the component uses the defaults (all zero).
 */
typedef struct Fl_Style {
    double radius;                      /* corner radius in local pixels; clamped to half the shorter side when drawn */
    bool gradient;                      /* blend from the fill colour to gradient_to along gradient_angle */
    unsigned char gradient_to[4];       /* sRGB RGBA8 */
    double gradient_angle;              /* radians; 0 runs left to right, pi/2 top to bottom */
    double shadow_blur;                 /* blur radius in local pixels; 0 disables the shadow */
    double shadow_x, shadow_y;          /* shadow offset in local pixels */
    unsigned char shadow_color[4];      /* sRGB RGBA8 */
    Fl_TextAlign align;
    double crop[4];                     /* source region of an image or video: x, y, width, height as fractions of the whole */
    bool clip;                          /* children (and the entity itself) are cut to this entity's box, rounded by radius */
    double blur;                        /* gaussian blur in scene pixels applied to the entity and its children together */
} Fl_Style;

/* Encoded size of the version-one style payload. */
#define FL_STYLE_BYTES 91u

/* Encode style into out as little-endian canonical bytes. */
void fl_style_encode(const Fl_Style *style, unsigned char out[FL_STYLE_BYTES]);
/* Decode a version-one style component; false for wrong size or out-of-range values. */
bool fl_style_decode(const Fl_Component *component, Fl_Style *out);
/* A style with every field at its default: no rounding, gradient or shadow, left aligned, the whole source visible. */
Fl_Style fl_style_default(void);
/* Read an entity's style, falling back to the defaults. */
Fl_Style fl_entity_style(const Fl_Entity *entity);
