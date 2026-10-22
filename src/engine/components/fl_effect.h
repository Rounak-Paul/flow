#pragma once
#include "fl_component.h"
#include "doc/fl_json.h"
#include <stddef.h>
#include <stdint.h>

#define FL_EFFECT_HEADER 68u
#define FL_EFFECT_CUSTOM 6u

/* A shader component. Kinds 1..5 are built in. Kind 6 is custom GLSL: a function
   `vec4 effect(vec2 uv, float time, vec4 a, vec4 b)`. Uniforms are keyframeable. */
typedef struct Fl_Effect {
    uint32_t kind;
    double u[8];
    const char *source;    /* borrowed from the component when kind is custom; not owned */
    size_t source_size;
} Fl_Effect;

Fl_Effect fl_effect_default(void);
size_t fl_effect_encode(const Fl_Effect *effect, unsigned char *out, size_t capacity);
bool fl_effect_decode(const Fl_Component *component, Fl_Effect *out);
cJSON *fl_effect_write(Fl_Json *context, const Fl_Component *component);
bool fl_effect_read(Fl_Json *context, const cJSON *object, Fl_Component *out);
