#include "fl_effect.h"
#include "doc/fl_json.h"
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

Fl_Effect fl_effect_default(void)
{
    Fl_Effect e = {.kind = 1};
    e.u[0] = 0.4; e.u[1] = 0.6;
    return e;
}

size_t fl_effect_encode(const Fl_Effect *effect, unsigned char *out, size_t capacity)
{
    Fl_Effect e = effect ? *effect : fl_effect_default();
    size_t source = e.kind == FL_EFFECT_CUSTOM && e.source ? e.source_size : 0;
    if (!out || e.kind < 1 || e.kind > FL_EFFECT_CUSTOM || source > 65536 || capacity < FL_EFFECT_HEADER + source) return 0;
    memset(out, 0, FL_EFFECT_HEADER + source);
    out[0] = (unsigned char)e.kind;
    for (int i = 0; i < 8; ++i) put_double(out + 4 + (size_t)i * 8, e.u[i]);
    if (source) memcpy(out + FL_EFFECT_HEADER, e.source, source);
    return FL_EFFECT_HEADER + source;
}

bool fl_effect_decode(const Fl_Component *c, Fl_Effect *out)
{
    if (!c || !out || c->type != FL_COMPONENT_EFFECT || c->version != 1 || !c->data || c->size < FL_EFFECT_HEADER || c->data[0] < 1 || c->data[0] > FL_EFFECT_CUSTOM) return false;
    Fl_Effect e = {.kind = c->data[0], .source = (const char *)c->data + FL_EFFECT_HEADER, .source_size = c->size - FL_EFFECT_HEADER};
    for (int i = 0; i < 8; ++i) if (!get_double(c->data + 4 + (size_t)i * 8, &e.u[i]) || fabs(e.u[i]) > 1e6) return false;
    if (e.kind != FL_EFFECT_CUSTOM) e.source_size = 0;
    else if (e.source_size && !fl_json_utf8_valid(e.source, e.source_size)) return false;
    *out = e;
    return true;
}

cJSON *fl_effect_write(Fl_Json *c, const Fl_Component *component)
{
    Fl_Effect e;
    if (!fl_effect_decode(component, &e)) {fl_json_fail(c, "Invalid effect"); return NULL;}
    cJSON *o = cJSON_CreateObject(), *u = cJSON_CreateArray();
    fl_json_add(c, o, "kind", cJSON_CreateNumber(e.kind));
    for (int i = 0; i < 8; ++i) fl_json_add(c, u, NULL, cJSON_CreateNumber(e.u[i]));
    fl_json_add(c, o, "uniforms", u);
    if (e.kind == FL_EFFECT_CUSTOM) {
        char *text = malloc(e.source_size + 1);
        if (!text) {fl_json_fail(c, "Cannot allocate effect source"); return o;}
        if (e.source_size) memcpy(text, e.source, e.source_size);
        text[e.source_size] = 0;
        fl_json_add(c, o, "source", cJSON_CreateString(text));
        free(text);
    }
    return o;
}

bool fl_effect_read(Fl_Json *c, const cJSON *object, Fl_Component *out)
{
    const cJSON *kind = fl_json_member(c, object, "kind", cJSON_Number), *uniforms = fl_json_member(c, object, "uniforms", cJSON_Array);
    if (!kind || !uniforms || cJSON_GetArraySize(uniforms) != 8 || kind->valuedouble < 1 || kind->valuedouble > FL_EFFECT_CUSTOM) return fl_json_fail(c, "Invalid effect");
    Fl_Effect e = {.kind = (uint32_t)kind->valuedouble};
    int i = 0;
    for (const cJSON *n = uniforms->child; n; n = n->next, ++i) if (!fl_json_number(c, n, &e.u[i])) return false;
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(object, "source");
    if (e.kind == FL_EFFECT_CUSTOM) {
        if (!cJSON_IsString(source)) return fl_json_fail(c, "A custom effect needs source");
        e.source = source->valuestring; e.source_size = strlen(source->valuestring);
    } else if (source) return fl_json_fail(c, "Built-in effects have no source");
    unsigned char *bytes = malloc(FL_EFFECT_HEADER + e.source_size);
    size_t size = bytes ? fl_effect_encode(&e, bytes, FL_EFFECT_HEADER + e.source_size) : 0;
    if (!size) {free(bytes); return fl_json_fail(c, "Invalid effect");}
    out->data = bytes; out->size = size; out->version = 1;
    return true;
}
