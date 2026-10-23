#include "doc/fl_json.h"
#include "fl_agent_internal.h"
#include "project/fl_manuscript.h"
#include "project/fl_speech_cues.h"
#include "doc/fl_document.h"
#include "components/fl_script.h"
#include "components/fl_style.h"
#include "components/fl_video_clip.h"
#include "components/fl_transform.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SECONDS 1.0e6
#define RAD_TO_DEG (180.0 / 3.14159265358979323846)

bool fl_fail(Fl_Fail *fail, const char *code, const char *format, ...)
{
    if (!fail) return false;
    snprintf(fail->code, sizeof(fail->code), "%s", code);
    va_list args; va_start(args, format);
    vsnprintf(fail->message, sizeof(fail->message), format, args);
    va_end(args);
    return false;
}

cJSON *fl_fail_json(const Fl_Fail *fail)
{
    cJSON *error = cJSON_CreateObject();
    if (!error) return NULL;
    cJSON_AddStringToObject(error, "code", fail->code);
    cJSON_AddStringToObject(error, "message", fail->message);
    return error;
}

bool agent_seconds_to_ticks(double seconds, Fl_Tick *ticks)
{
    if (!isfinite(seconds) || fabs(seconds) > MAX_SECONDS) return false;
    return fl_time_from_seconds(seconds,ticks);
}

double agent_ticks_to_seconds(int64_t ticks) { return (double)ticks / FL_TICKS_PER_SECOND; }

bool agent_object(const cJSON *value, Fl_Fail *fail, const char *ctx)
{
    return cJSON_IsObject(value) || fl_fail(fail, "invalid_argument", "%s must be an object", ctx);
}

bool agent_only_keys(const cJSON *object, Fl_Fail *fail, const char *ctx, const char *const *allowed)
{
    for (const cJSON *item = object ? object->child : NULL; item; item = item->next) {
        bool known = false;
        for (size_t i = 0; allowed[i]; ++i) known |= !strcmp(allowed[i], item->string);
        if (!known) return fl_fail(fail, "unknown_field", "%s has no field \"%s\"", ctx, item->string);
    }
    return true;
}

bool agent_get_string(const cJSON *o, const char *key, bool required, const char **out, Fl_Fail *f, const char *ctx)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    *out = NULL;
    if (!v || cJSON_IsNull(v)) return !required || fl_fail(f, "missing_field", "%s requires \"%s\"", ctx, key);
    if (!cJSON_IsString(v)) return fl_fail(f, "invalid_argument", "%s: \"%s\" must be a string", ctx, key);
    *out = v->valuestring;
    return true;
}

bool agent_get_number(const cJSON *o, const char *key, bool required, double *out, bool *present, Fl_Fail *f, const char *ctx)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (present) *present = false;
    if (!v || cJSON_IsNull(v)) return !required || fl_fail(f, "missing_field", "%s requires \"%s\"", ctx, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return fl_fail(f, "invalid_argument", "%s: \"%s\" must be a finite number", ctx, key);
    *out = v->valuedouble; if (present) *present = true;
    return true;
}

bool agent_get_bool(const cJSON *o, const char *key, bool *out, bool *present, Fl_Fail *f, const char *ctx)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (present) *present = false;
    if (!v || cJSON_IsNull(v)) return true;
    if (!cJSON_IsBool(v)) return fl_fail(f, "invalid_argument", "%s: \"%s\" must be true or false", ctx, key);
    *out = cJSON_IsTrue(v); if (present) *present = true;
    return true;
}

cJSON *agent_id_json(Fl_Id id)
{
    return id <= UINT64_C(9007199254740991) ? cJSON_CreateNumber((double)id) : fl_json_u64(id);
}

bool agent_id_value(const cJSON *value, const Fl_AgentRefs *refs, Fl_Id *out, Fl_Fail *fail, const char *ctx)
{
    *out = 0;
    if (cJSON_IsString(value) && value->valuestring[0] == '$') {
        for (size_t i = 0; refs && i < refs->count; ++i) {
            if (!strcmp(refs->names[i], value->valuestring)) {*out = refs->ids[i]; return true;}
        }
        return fl_fail(fail, "unknown_reference", "%s: no earlier operation created %s", ctx, value->valuestring);
    }
    Fl_Json json = {0};
    return (fl_json_uint(&json, value, out) && *out) || fl_fail(fail, "invalid_argument", "%s requires a positive exact ID", ctx);
}

bool agent_get_id(const cJSON *object, const char *key, const Fl_AgentRefs *refs, bool required, Fl_Id *out, Fl_Fail *fail, const char *ctx)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    *out = 0;
    if (!value || cJSON_IsNull(value)) return !required || fl_fail(fail, "missing_field", "%s requires %s", ctx, key);
    return agent_id_value(value, refs, out, fail, ctx);
}

bool agent_get_seconds(const cJSON *o, const char *key, bool required, Fl_Tick *out, bool *present, Fl_Fail *f, const char *ctx)
{
    double seconds; bool has;
    if (!agent_get_number(o, key, required, &seconds, &has, f, ctx)) return false;
    if (present) *present = has;
    if (!has) return true;
    return agent_seconds_to_ticks(seconds, out) || fl_fail(f, "invalid_argument", "%s: \"%s\" is out of range", ctx, key);
}

bool agent_parse_color(const cJSON *v, unsigned char rgba[4], Fl_Fail *f, const char *ctx)
{
    if (cJSON_IsString(v)) {
        const char *s = v->valuestring;
        size_t n = strlen(s);
        if (s[0] != '#' || (n != 7 && n != 9)) return fl_fail(f, "invalid_argument", "%s must be \"#RRGGBB\" or \"#RRGGBBAA\"", ctx);
        unsigned char out[4] = {0, 0, 0, 255};
        for (size_t i = 0; i < (n - 1) / 2; ++i) {
            unsigned value = 0;
            for (size_t j = 0; j < 2; ++j) {
                char c = s[1 + i * 2 + j];
                unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16u;
                if (d > 15) return fl_fail(f, "invalid_argument", "%s has a non-hex digit", ctx);
                value = value * 16 + d;
            }
            out[i] = (unsigned char)value;
        }
        memcpy(rgba, out, 4); return true;
    }
    if (cJSON_IsArray(v) && (cJSON_GetArraySize(v) == 3 || cJSON_GetArraySize(v) == 4)) {
        unsigned char out[4] = {0, 0, 0, 255};
        int i = 0;
        for (const cJSON *c = v->child; c; c = c->next, ++i) {
            if (!cJSON_IsNumber(c) || c->valuedouble < 0 || c->valuedouble > 255) return fl_fail(f, "invalid_argument", "%s channels must be numbers in 0..255", ctx);
            out[i] = (unsigned char)lround(c->valuedouble);
        }
        memcpy(rgba, out, 4); return true;
    }
    return fl_fail(f, "invalid_argument", "%s must be \"#RRGGBB[AA]\" or [r,g,b,a]", ctx);
}

/* An anchor is absolute seconds, {"time": s}, or {"beat": id, "edge": "start"|"end", "offset": s}. */
bool agent_parse_anchor(const cJSON *v, const Fl_AgentRefs *refs, Fl_TimeAnchor *out, Fl_Fail *f, const char *ctx)
{
    Fl_TimeAnchor anchor = {0};
    if (cJSON_IsNumber(v)) {
        if (!agent_seconds_to_ticks(v->valuedouble, &anchor.offset)) return fl_fail(f, "invalid_argument", "%s is out of range", ctx);
    } else if (cJSON_IsObject(v)) {
        static const char *const keys[] = {"time", "beat", "cue", "edge", "offset", "position", NULL};
        if (!agent_only_keys(v, f, ctx, keys)) return false;
        bool has;
        if (cJSON_GetObjectItemCaseSensitive(v, "time")) {
            if (cJSON_GetObjectItemCaseSensitive(v,"cue") || cJSON_GetObjectItemCaseSensitive(v,"beat") || cJSON_GetObjectItemCaseSensitive(v,"edge") ||
                cJSON_GetObjectItemCaseSensitive(v,"offset") || cJSON_GetObjectItemCaseSensitive(v,"position"))
                return fl_fail(f,"invalid_argument","%s: absolute time cannot combine with beat fields",ctx);
            if (!agent_get_seconds(v, "time", true, &anchor.offset, &has, f, ctx)) return false;
        } else if (cJSON_GetObjectItemCaseSensitive(v,"cue")) {
            const char *edge = "start"; anchor.kind = FL_ANCHOR_SPEECH;
            if (cJSON_GetObjectItemCaseSensitive(v,"beat") || cJSON_GetObjectItemCaseSensitive(v,"position") ||
                !agent_get_id(v,"cue",refs,true,&anchor.cue,f,ctx) || !agent_get_string(v,"edge",false,&edge,f,ctx) ||
                !agent_get_seconds(v,"offset",false,&anchor.offset,&has,f,ctx)) return fl_fail(f,"invalid_argument","%s: invalid speech cue anchor",ctx);
            if (!edge) edge = "start";
            if (strcmp(edge,"start") && strcmp(edge,"end")) return fl_fail(f,"invalid_argument","%s: cue edge must be start or end",ctx);
            anchor.end = !strcmp(edge,"end");
        } else {
            const char *edge = "start";
            anchor.kind = FL_ANCHOR_BEAT;
            if (!agent_get_id(v, "beat", refs, true, &anchor.beat, f, ctx) || !agent_get_string(v, "edge", false, &edge, f, ctx) ||
                !agent_get_seconds(v, "offset", false, &anchor.offset, &has, f, ctx)) return false;
            if (!edge) edge = "start";
            if (strcmp(edge, "start") && strcmp(edge, "end")) return fl_fail(f, "invalid_argument", "%s: edge must be \"start\" or \"end\"", ctx);
            anchor.end = !strcmp(edge, "end");
            const cJSON *position = cJSON_GetObjectItemCaseSensitive(v, "position");
            if (position) {
                if (cJSON_GetObjectItemCaseSensitive(v, "edge") || !cJSON_IsNumber(position) ||
                    !isfinite(position->valuedouble) || position->valuedouble < 0 || position->valuedouble > 1)
                    return fl_fail(f, "invalid_argument", "%s: position must be 0..1 and cannot combine with edge", ctx);
                anchor.kind = FL_ANCHOR_BEAT_POSITION; anchor.position = position->valuedouble;
            }

        }
    } else return fl_fail(f, "invalid_argument", "%s must be seconds or {beat, edge, offset}", ctx);
    *out = anchor; return true;
}

static const char *const widget_names[FL_WIDGET_COUNT] = {"entity", "rectangle", "ellipse", "line", "arrow", "text", "image", "video", "triangle", "diamond", "hexagon", "star", "path", "effect"};
static const char *const ease_names[FL_EASE_COUNT] = {"linear", "in", "out", "in_out", "hold", "cubic", "spring"};

const char *agent_widget_name(Fl_WidgetKind kind) { return kind >= 0 && kind < FL_WIDGET_COUNT ? widget_names[kind] : "unknown"; }
bool agent_widget_from_name(const char *name, Fl_WidgetKind *kind)
{
    for (int i = 0; i < FL_WIDGET_COUNT; ++i) if (!strcmp(name, widget_names[i])) {*kind = (Fl_WidgetKind)i; return true;}
    if (!strcmp(name, "group")) {*kind = FL_WIDGET_ENTITY; return true;}
    return false;
}
bool agent_property_from_name(const char *name, Fl_AnimProperty *property)
{
    for (int i = 0; i < FL_PROP_COUNT; ++i)     if (!strcmp(name, fl_property_name((Fl_AnimProperty)i)) || (i == FL_PROP_RZ && !strcmp(name, "rotation"))) {*property = (Fl_AnimProperty)i; return true;}
    return false;
}
const char *agent_ease_name(Fl_Ease ease) { return ease >= 0 && ease < FL_EASE_COUNT ? ease_names[ease] : "linear"; }
bool agent_ease_from_name(const char *name, Fl_Ease *ease)
{
    for (int i = 0; i < FL_EASE_COUNT; ++i) if (!strcmp(name, ease_names[i])) {*ease = (Fl_Ease)i; return true;}
    return false;
}

const Fl_Scene *agent_scene_of_entity(const Fl_Project *p, Fl_Id id)
{
    for (size_t i = 0; i < fl_project_scene_count(p); ++i) {
        const Fl_Scene *s = fl_project_scene_at(p, i);
        if (fl_scene_entity(s, id)) return s;
    }
    return NULL;
}
const Fl_Scene *agent_scene_of_beat(const Fl_Project *p, Fl_Id id)
{
    for (size_t i = 0; i < fl_project_scene_count(p); ++i) {
        const Fl_Scene *s = fl_project_scene_at(p, i);
        for (size_t b = 0; b < s->beat_count; ++b) if (s->beats[b].id == id) return s;
    }
    return NULL;
}
const Fl_Scene *agent_scene_of_phase(const Fl_Project *p, Fl_Id id)
{
    for (size_t i = 0; i < fl_project_scene_count(p); ++i) {
        const Fl_Scene *s = fl_project_scene_at(p, i);
        if (fl_scene_phase(s, id)) return s;
    }
    return NULL;
}
const Fl_Scene *agent_scene_of_clip(const Fl_Project *p, Fl_Id id)
{
    for (size_t i = 0; i < fl_project_scene_count(p); ++i) {
        const Fl_Scene *s = fl_project_scene_at(p, i);
        for (size_t c = 0; c < s->clip_count; ++c) if (s->clips[c].id == id) return s;
    }
    return NULL;
}
cJSON *agent_script_json(const Fl_Project *project, const Fl_Asset *script, bool with_source)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddItemToObject(o, "id", agent_id_json(script->id));
    cJSON_AddStringToObject(o, "name", script->name);
    cJSON_AddNumberToObject(o, "bytes", (double)script->size);
    cJSON *users = cJSON_AddArrayToObject(o, "used_by");
    for (size_t n = 0; users && n < fl_project_scene_count(project); ++n) {
        const Fl_Scene *s = fl_project_scene_at(project, n);
        for (size_t i = 0; i < s->entity_count; ++i)
            if (fl_entity_script_component(&s->entities[i], script->id))
                cJSON_AddItemToArray(users, agent_id_json(s->entities[i].id));
    }
    if (with_source) cJSON_AddStringToObject(o, "source", script->source);
    return o;
}
const Fl_Scene *agent_scene_of_key(const Fl_Project *p, Fl_Id id, const Fl_Track **track)
{
    for (size_t i = 0; i < fl_project_scene_count(p); ++i) {
        const Fl_Scene *s = fl_project_scene_at(p, i);
        if (fl_scene_key(s, id, track)) return s;
    }
    return NULL;
}

void agent_color_json(cJSON *object, const char *key, const unsigned char c[4])
{
    char text[10];
    snprintf(text, sizeof(text), "#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);
    cJSON_AddStringToObject(object, key, text);
}

cJSON *agent_scene_summary(const Fl_Scene *s)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddItemToObject(o, "id", agent_id_json(s->id));
    cJSON_AddStringToObject(o, "name", s->name);
    cJSON_AddItemToObject(o, "revision", agent_id_json(s->revision));
    cJSON_AddNumberToObject(o, "width", (double)s->width);
    cJSON_AddNumberToObject(o, "height", (double)s->height);
    cJSON_AddNumberToObject(o, "length_seconds", agent_ticks_to_seconds(fl_scene_length(s)));
    cJSON_AddNumberToObject(o, "entities", (double)s->entity_count);
    cJSON_AddNumberToObject(o, "beats", (double)s->beat_count);
    return o;
}

cJSON *agent_asset_json(const Fl_Asset *a, size_t uses)
{
    static const char *const kinds[] = {"", "image", "video", "audio", "font", "lua", "flproto"};
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddItemToObject(o, "id", agent_id_json(a->id));
    cJSON_AddStringToObject(o, "name", a->name);
    cJSON_AddStringToObject(o, "kind", kinds[a->kind]);
    if (a->path) cJSON_AddStringToObject(o,"path",a->path);
    cJSON_AddNumberToObject(o, "bytes", (double)a->size);
    if (a->width) {cJSON_AddNumberToObject(o, "width", (double)a->width); cJSON_AddNumberToObject(o, "height", (double)a->height);}
    if (a->duration) cJSON_AddNumberToObject(o, "duration_seconds", agent_ticks_to_seconds((int64_t)a->duration));
    cJSON_AddNumberToObject(o, "uses", (double)uses);
    return o;
}

cJSON *agent_anchor_json(const Fl_Scene *s, const Fl_TimeAnchor *a)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    Fl_Tick tick;
    if (fl_anchor_sample(s, a, &tick)) cJSON_AddNumberToObject(o, "time", agent_ticks_to_seconds(tick));
    else cJSON_AddStringToObject(o, "diagnostic", "Unbound time anchor");
    if (a->kind != FL_ANCHOR_ABSOLUTE) {
        cJSON_AddItemToObject(o, a->kind == FL_ANCHOR_SPEECH ? "cue" : "beat", agent_id_json(a->kind == FL_ANCHOR_SPEECH ? a->cue : a->beat));
        if (a->kind == FL_ANCHOR_BEAT_POSITION) cJSON_AddNumberToObject(o, "position", a->position);
        else cJSON_AddStringToObject(o, "edge", a->end ? "end" : "start");
        cJSON_AddNumberToObject(o, "offset", agent_ticks_to_seconds(a->offset));
    }
    return o;
}

/* Track value as exchanged with agents: degrees for rotation, hex for colours. */
static void track_value_json(cJSON *key, Fl_AnimProperty property, const double v[4])
{
    if (property == FL_PROP_FILL || property == FL_PROP_STROKE) {
        unsigned char rgba[4];
        for (int i = 0; i < 4; ++i) rgba[i] = (unsigned char)lround(v[i]);
        agent_color_json(key, "value", rgba);
    } else cJSON_AddNumberToObject(key, "value", property == FL_PROP_RX || property == FL_PROP_RY || property == FL_PROP_RZ ? v[0] * RAD_TO_DEG : v[0]);
}

static cJSON *track_json(const Fl_Scene *s, const Fl_Track *t)
{
    cJSON *o = cJSON_CreateObject(), *keys = o ? cJSON_AddArrayToObject(o, "keys") : NULL;
    if (!keys) {cJSON_Delete(o); return NULL;}
    cJSON_AddItemToObject(o, "id", agent_id_json(t->id));
    cJSON_AddStringToObject(o, "property", fl_property_name(t->property));
    for (size_t k = 0; k < t->key_count; ++k) {
        const Fl_Keyframe *key = &t->keys[k];
        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddItemToObject(item, "id", agent_id_json(key->id));
        cJSON_AddItemToObject(item, "at", agent_anchor_json(s, &key->at));
        track_value_json(item, t->property, key->value);
        cJSON_AddStringToObject(item, "ease", agent_ease_name(key->ease));
        cJSON_AddItemToArray(keys, item);
    }
    return o;
}

static cJSON *entity_json(const Fl_Scene *s, const Fl_Entity *e)
{
    const Fl_WidgetDef *def = fl_widget_for_entity(e);
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddItemToObject(o, "id", agent_id_json(e->id));
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "kind", def ? agent_widget_name(def->kind) : "unknown");
    if (e->parent) cJSON_AddItemToObject(o, "parent", agent_id_json(e->parent));
    cJSON_AddBoolToObject(o, "visible", e->visible);
    Fl_Pose t = fl_pose_identity(); Fl_Bounds box = fl_entity_bounds(e);
    fl_pose_decode(fl_entity_component(e, FL_COMPONENT_POSE), &t);
    cJSON_AddNumberToObject(o, "x", t.x); cJSON_AddNumberToObject(o, "y", t.y); cJSON_AddNumberToObject(o, "z", t.z);
    cJSON_AddNumberToObject(o, "width", box.width); cJSON_AddNumberToObject(o, "height", box.height); cJSON_AddNumberToObject(o, "depth", box.depth);
    cJSON_AddNumberToObject(o, "rotation", t.rz * RAD_TO_DEG);
    cJSON_AddNumberToObject(o, "rx", t.rx * RAD_TO_DEG); cJSON_AddNumberToObject(o, "ry", t.ry * RAD_TO_DEG);
    cJSON_AddNumberToObject(o, "scale_x", t.sx); cJSON_AddNumberToObject(o, "scale_y", t.sy); cJSON_AddNumberToObject(o, "scale_z", t.sz);
    cJSON_AddNumberToObject(o, "opacity", fl_widget_opacity(e));
    Fl_Appearance a = fl_widget_appearance(e);
    if (def && def->has_fill) agent_color_json(o, "fill", a.fill);
    if (def && def->has_stroke) {agent_color_json(o, "stroke", a.stroke); cJSON_AddNumberToObject(o, "stroke_width", a.stroke_width);}
    if (def && def->has_text) {
        const Fl_Component *text = fl_entity_component(e, FL_COMPONENT_TEXT);
        char *copy = malloc((text ? text->size : 0) + 1);
        if (copy) {
            if (text && text->size) memcpy(copy, text->data, text->size);
            copy[text ? text->size : 0] = 0;
            cJSON_AddStringToObject(o, "text", copy); free(copy);
        }
        Fl_Typography ty = fl_widget_typography(e);
        cJSON_AddNumberToObject(o, "font_size", a.font_size);
        cJSON_AddStringToObject(o, "font_family", ty.family);
        cJSON_AddNumberToObject(o, "font_weight", ty.weight);
        cJSON_AddBoolToObject(o, "italic", ty.italic);
    }
    Fl_Id asset;
    if (fl_image_asset_decode(fl_entity_component(e, FL_COMPONENT_IMAGE), &asset)) cJSON_AddItemToObject(o, "asset", agent_id_json(asset));
    Fl_VideoClip clip;
    if (fl_video_clip_decode(fl_entity_component(e, FL_COMPONENT_VIDEO), &clip)) {
        cJSON_AddItemToObject(o, "asset", agent_id_json(clip.asset)); cJSON_AddNumberToObject(o, "start", agent_ticks_to_seconds(clip.start));
        cJSON_AddNumberToObject(o, "offset", agent_ticks_to_seconds(clip.offset)); cJSON_AddNumberToObject(o, "rate", clip.rate);
        cJSON_AddBoolToObject(o, "loop", clip.loop);
    }
    Fl_Style style;
    if (fl_style_decode(fl_entity_component(e, FL_COMPONENT_STYLE), &style)) {
        if (style.radius > 0) cJSON_AddNumberToObject(o, "radius", style.radius);
        if (style.clip) cJSON_AddBoolToObject(o, "clip", true);
        if (style.blur > 0) cJSON_AddNumberToObject(o, "blur", style.blur);
        if (style.gradient) {agent_color_json(o, "fill2", style.gradient_to); cJSON_AddNumberToObject(o, "gradient_angle", style.gradient_angle * RAD_TO_DEG);}
        if (style.shadow_blur > 0) {
            cJSON_AddNumberToObject(o, "shadow_blur", style.shadow_blur); cJSON_AddNumberToObject(o, "shadow_x", style.shadow_x);
            cJSON_AddNumberToObject(o, "shadow_y", style.shadow_y); agent_color_json(o, "shadow_color", style.shadow_color);
        }
        if (style.crop[0] != 0 || style.crop[1] != 0 || style.crop[2] != 1 || style.crop[3] != 1) {
            cJSON *crop = cJSON_AddArrayToObject(o, "crop");
            for (int i = 0; crop && i < 4; ++i) cJSON_AddItemToArray(crop, cJSON_CreateNumber(style.crop[i]));
        }
        if (def && def->has_text) cJSON_AddStringToObject(o, "align", style.align == FL_ALIGN_CENTER ? "center" : style.align == FL_ALIGN_RIGHT ? "right" : "left");
    }
    Fl_Script script;
    const Fl_Asset *source = fl_script_decode(fl_entity_component(e, FL_COMPONENT_SCRIPT), &script) ? fl_project_asset(s->project, script.script) : NULL;
    if (source) {
        cJSON *item = cJSON_AddObjectToObject(o, "script");
        if (item) {
            cJSON_AddItemToObject(item, "id", agent_id_json(source->id));
            cJSON_AddStringToObject(item, "name", source->name);
            cJSON_AddBoolToObject(item, "enabled", script.enabled);
        }
    }
    Fl_Json document={0}; cJSON *components=cJSON_AddArrayToObject(o,"components");
    for (size_t i=0;components && i<e->component_count;++i) fl_json_add(&document,components,NULL,fl_document_component_write(&document,&e->components[i]));
    const Fl_Animation *animation = fl_entity_animation(e);
    cJSON *tracks = animation && animation->track_count ? cJSON_AddArrayToObject(o, "tracks") : NULL;
    for (size_t i = 0; tracks && i < animation->track_count; ++i) cJSON_AddItemToArray(tracks, track_json(s, &animation->tracks[i]));
    return o;
}

cJSON *agent_manuscript_json(const Fl_Scene *s)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    size_t words = 0;
    for (size_t i = 0; i < s->phase_count; ++i) words += fl_manuscript_words(s->phases[i].text);
    cJSON_AddItemToObject(o, "scene", agent_id_json(s->id));
    cJSON_AddStringToObject(o, "voice", s->voice ? s->voice : "Ryan");
    cJSON_AddStringToObject(o, "language", s->language ? s->language : "English");
    cJSON_AddItemToObject(o, "reference", agent_id_json(s->voice_reference));
    cJSON_AddStringToObject(o, "transcript", s->voice_transcript ? s->voice_transcript : "");
    cJSON_AddNumberToObject(o, "words", (double)words);
    cJSON_AddNumberToObject(o, "minutes", (double)words / (double)FL_NARRATION_WPM);
    cJSON *phases = cJSON_AddArrayToObject(o, "phases");
    for (size_t i = 0; phases && i < s->phase_count; ++i) {
        const Fl_Phase *phase = &s->phases[i];
        size_t count = fl_manuscript_words(phase->text);
        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddItemToObject(item, "id", agent_id_json(phase->id));
        cJSON_AddStringToObject(item, "title", phase->title);
        cJSON_AddStringToObject(item, "text", phase->text);
        cJSON_AddItemToObject(item,"alignment_asset",agent_id_json(phase->cue_asset));
        cJSON *cues = cJSON_AddArrayToObject(item,"cues");
        for (size_t j = 0; cues && j < phase->cue_count; ++j) {
            const Fl_SpeechCue *cue = &phase->cues[j]; cJSON *record = cJSON_CreateObject(); Fl_Tick tick;
            cJSON_AddItemToObject(record,"id",agent_id_json(cue->id)); cJSON_AddStringToObject(record,"text",cue->text);
            cJSON_AddStringToObject(record,"kind",cue->kind == FL_CUE_SENTENCE ? "sentence" : "word");
            cJSON_AddNumberToObject(record,"start",agent_ticks_to_seconds(cue->start)); cJSON_AddNumberToObject(record,"end",agent_ticks_to_seconds(cue->end));
            bool resolved = fl_phase_speech_cue_sample(s,phase,cue,false,&tick);
            cJSON_AddBoolToObject(record,"resolved",resolved); cJSON_AddBoolToObject(record,"retired",cue->retired);
            if (resolved) cJSON_AddNumberToObject(record,"time",agent_ticks_to_seconds(tick));
            else cJSON_AddStringToObject(record,"diagnostic",cue->retired ? "Removed or ambiguous cue; rebind explicitly" : "Speech timing unresolved");
            cJSON_AddItemToArray(cues,record);
        }
        cJSON_AddItemToObject(item, "beat", agent_id_json(phase->beat));
        cJSON_AddItemToObject(item, "clip", agent_id_json(phase->clip));
        cJSON_AddNumberToObject(item, "words", (double)count);
        cJSON_AddNumberToObject(item, "seconds", fl_manuscript_seconds(phase->text));
        cJSON_AddNumberToObject(item, "minutes", (double)count / (double)FL_NARRATION_WPM);
        cJSON_AddItemToArray(phases, item);
    }
    return o;
}

cJSON *agent_scene_json(const Fl_Project *p, const Fl_Scene *s, bool with_sources)
{
    (void)p;
    cJSON *o = agent_scene_summary(s);
    if (!o) return NULL;
    if (s->aspect_width) {
        cJSON *aspect = cJSON_AddArrayToObject(o, "aspect");
        if (aspect) {cJSON_AddItemToArray(aspect, cJSON_CreateNumber((double)s->aspect_width)); cJSON_AddItemToArray(aspect, cJSON_CreateNumber((double)s->aspect_height));}
    }
    agent_color_json(o, "background", s->background);
    cJSON *camera = cJSON_AddArrayToObject(o, "camera");
    if (camera) {cJSON_AddItemToArray(camera, cJSON_CreateNumber(s->camera_x)); cJSON_AddItemToArray(camera, cJSON_CreateNumber(s->camera_y));}
    cJSON *beats = cJSON_AddArrayToObject(o, "beat_list");
    Fl_Tick start = 0;
    for (size_t i = 0; beats && i < s->beat_count; ++i) {
        const Fl_Beat *b = &s->beats[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddItemToObject(item, "id", agent_id_json(b->id));
        cJSON_AddStringToObject(item, "name", b->name);
        cJSON_AddNumberToObject(item, "start", agent_ticks_to_seconds(start));
        cJSON_AddNumberToObject(item, "duration", agent_ticks_to_seconds((int64_t)b->duration));
        cJSON_AddBoolToObject(item, "auto_advance", b->auto_advance);
        cJSON_AddItemToArray(beats, item);
        start += (Fl_Tick)b->duration;
    }
    cJSON *clips = cJSON_AddArrayToObject(o, "clips");
    for (size_t i = 0; clips && i < s->clip_count; ++i) {
        const Fl_AudioClip *c = &s->clips[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddItemToObject(item, "id", agent_id_json(c->id));
        cJSON_AddItemToObject(item, "asset", agent_id_json(c->asset));
        cJSON_AddItemToObject(item, "start", agent_anchor_json(s, &c->start));
        cJSON_AddNumberToObject(item, "source_offset", agent_ticks_to_seconds((int64_t)c->source_offset));
        cJSON_AddNumberToObject(item, "duration", agent_ticks_to_seconds((int64_t)c->duration));
        cJSON_AddNumberToObject(item, "gain", c->gain);
        cJSON_AddNumberToObject(item, "fade_in", agent_ticks_to_seconds((int64_t)c->fade_in));
        cJSON_AddNumberToObject(item, "fade_out", agent_ticks_to_seconds((int64_t)c->fade_out));
        cJSON_AddItemToArray(clips, item);
    }
    cJSON *scripts = cJSON_AddArrayToObject(o, "scripts");
    for (size_t i=0; scripts && i<fl_project_asset_count(s->project); ++i) {
        const Fl_Asset *asset=fl_project_asset_at(s->project,i);
        if (asset->kind==FL_ASSET_LUA) cJSON_AddItemToArray(scripts,agent_script_json(p,asset,with_sources));
    }
    cJSON *entities = cJSON_AddArrayToObject(o, "entity_list");
    for (size_t i = 0; entities && i < s->entity_count; ++i) cJSON_AddItemToArray(entities, entity_json(s, &s->entities[i]));
    cJSON_AddItemToObject(o, "manuscript", agent_manuscript_json(s));
    return o;
}
