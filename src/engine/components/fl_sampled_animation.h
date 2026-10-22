#pragma once
#include "fl_animation.h"
#include "doc/fl_json.h"

#define FL_SAMPLED_ANIMATION_VERSION 2u
/* Version 1 clips stored every channel except aspect_w and aspect_h. */
#define FL_SAMPLED_CHANNELS_V1 51u
#define FL_SAMPLED_ANIMATION_LIMIT (32u * 1024u * 1024u)
#define FL_SAMPLED_ANIMATION_KEYS 32768u
/* One sampled native pose. Text is a borrowed UTF-8 slice when the clip owns text output. */
typedef struct Fl_SampledKey {
    Fl_Tick tick;
    double values[FL_PROP_COUNT][4];
    const char *text;
    size_t text_size;
} Fl_SampledKey;
/* Encode bounded, strictly ordered canonical samples; text is held between keys. */
bool fl_sampled_animation_encode(const Fl_SampledKey *keys, size_t count, bool text, Fl_Component *out);
/* Validate a canonical sampled clip, including all numeric channels and text slices. */
bool fl_sampled_animation_valid(const Fl_Component *component);
/* Sample numeric channels linearly and text by hold; samples outside the interval hold its endpoint. */
bool fl_sampled_animation_sample(const Fl_Component *component, Fl_Tick tick, Fl_SampledKey *out, bool *text);
/* Read/write the native document's sampled pose array; returned JSON and component payloads are owned. */
cJSON *fl_sampled_animation_write(Fl_Json *context, const Fl_Component *component);
bool fl_sampled_animation_read(Fl_Json *context, const cJSON *samples, Fl_Component *out);
