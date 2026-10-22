#include "components/fl_prototype_instance.h"
#include "components/fl_local_composition.h"
#include "components/fl_sampled_animation.h"
#include "components/fl_events.h"
#include "fl_widgets.h"
#include "doc/fl_json.h"
#include "components/fl_animation.h"
#include "components/fl_script.h"
#include "components/fl_style.h"
#include "components/fl_video_clip.h"
#include "components/fl_spatial.h"
#include "components/fl_path.h"
#include "components/fl_effect.h"
#include "components/fl_motion.h"
#include <math.h>
#include <string.h>

static const Fl_Pose k_pose = {.sx = 1, .sy = 1, .sz = 1};
static const Fl_WidgetDef definitions[FL_WIDGET_COUNT] = {
    {FL_WIDGET_ENTITY, "Entity", "Empty entity. Children follow its transform", k_pose, {320,180,0}, {{91,156,255,255},{230,233,238,255},0,48}, false,false,false,false,false},
    {FL_WIDGET_RECTANGLE, "Rectangle", "Filled or outlined rectangle", k_pose, {320,180,0}, {{91,156,255,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_ELLIPSE, "Ellipse", "Circle or ellipse", k_pose, {240,240,0}, {{76,195,138,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_LINE, "Line", "Straight connector", k_pose, {360,32,0}, {{230,233,238,255},{230,233,238,255},6,48}, false,true,false,false,false},
    {FL_WIDGET_ARROW, "Arrow", "Directional connector", k_pose, {360,72,0}, {{232,176,74,255},{232,176,74,255},6,48}, false,true,false,false,false},
    {FL_WIDGET_TEXT, "Text", "Editable text block", k_pose, {480,160,0}, {{230,233,238,255},{0,0,0,0},0,48}, true,false,true,false,false},
    {FL_WIDGET_IMAGE, "Image", "Picture from the project's assets", k_pose, {320,180,0}, {{255,255,255,255},{0,0,0,0},0,48}, false,false,false,true,false},
    {FL_WIDGET_VIDEO, "Video", "Video clip from the project's assets", k_pose, {320,180,0}, {{255,255,255,255},{0,0,0,0},0,48}, false,false,false,false,true},
    {FL_WIDGET_TRIANGLE, "Triangle", "Triangle pointing up", k_pose, {240,208,0}, {{232,112,92,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_DIAMOND, "Diamond", "Rhombus, for decisions and highlights", k_pose, {240,240,0}, {{168,124,232,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_HEXAGON, "Hexagon", "Regular six-sided shape", k_pose, {260,225,0}, {{72,190,200,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_STAR, "Star", "Five-pointed star", k_pose, {240,240,0}, {{245,200,66,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_PATH, "Path", "Cubic curve. Stroke paints the trim; fill paints the interior when the curve closes", k_pose, {360,180,0}, {{230,233,238,0},{230,233,238,255},4,48}, true,true,false,false,false},
    {FL_WIDGET_EFFECT, "Effect", "Procedural shader fill", k_pose, {640,360,0}, {{91,156,255,255},{232,112,92,255},0,48}, true,true,false,false,false},
};
static const Fl_Typography default_typography = {FL_TYPOGRAPHY_DEFAULT_FAMILY, 400, false};

const Fl_WidgetDef *fl_widget_definition(Fl_WidgetKind kind)
{
    return kind >= 0 && kind < FL_WIDGET_COUNT ? &definitions[kind] : NULL;
}

bool fl_widget_is_polygon(Fl_WidgetKind kind)
{
    return kind == FL_WIDGET_TRIANGLE || kind == FL_WIDGET_DIAMOND || kind == FL_WIDGET_HEXAGON || kind == FL_WIDGET_STAR;
}

bool fl_widget_is_geometry(Fl_WidgetKind kind)
{
    return (kind >= FL_WIDGET_ELLIPSE && kind <= FL_WIDGET_ARROW) || fl_widget_is_polygon(kind) || kind == FL_WIDGET_PATH || kind == FL_WIDGET_EFFECT;
}

size_t fl_widget_polygon(Fl_WidgetKind kind, double out[FL_POLYGON_MAX_VERTICES][2])
{
    static const double pi = 3.14159265358979323846;
    size_t count = 0;
    switch (kind) {
    case FL_WIDGET_TRIANGLE:
        count = 3;
        out[0][0] = 0; out[0][1] = -1; out[1][0] = 1; out[1][1] = 1; out[2][0] = -1; out[2][1] = 1;
        break;
    case FL_WIDGET_DIAMOND:
        count = 4;
        for (size_t k = 0; k < count; ++k) {out[k][0] = cos(-pi / 2 + k * pi / 2); out[k][1] = sin(-pi / 2 + k * pi / 2);}
        break;
    case FL_WIDGET_HEXAGON:
        count = 6;
        for (size_t k = 0; k < count; ++k) {out[k][0] = cos(k * pi / 3); out[k][1] = sin(k * pi / 3) / 0.86602540378;}
        break;
    case FL_WIDGET_STAR:
        count = 10;
        for (size_t k = 0; k < count; ++k) {
            double radius = k % 2 ? 0.4 : 1, angle = -pi / 2 + k * pi / 5;
            out[k][0] = radius * cos(angle); out[k][1] = radius * sin(angle) + 0.0955;
        }
        break;
    default: break;
    }
    return count;
}

bool fl_widget_shape_contains(Fl_WidgetKind kind, double nx, double ny)
{
    if (kind == FL_WIDGET_ELLIPSE) return nx * nx + ny * ny <= 1;
    double v[FL_POLYGON_MAX_VERTICES][2];
    const size_t count = fl_widget_polygon(kind, v);
    bool inside = false;
    for (size_t i = 0, j = count - 1; i < count; j = i++) {
        if ((v[i][1] > ny) != (v[j][1] > ny) && nx < (v[j][0] - v[i][0]) * (ny - v[i][1]) / (v[j][1] - v[i][1]) + v[i][0]) inside = !inside;
    }
    return inside;
}

/* Read/write IEEE754 doubles without native layout or alignment assumptions. */
static double read_double(const unsigned char *bytes)
{
    uint64_t bits = 0; double value;
    for (unsigned i = 0; i < 8; ++i) bits |= (uint64_t)bytes[i] << (8 * i);
    memcpy(&value, &bits, 8); return value;
}
static void write_double(unsigned char *bytes, double value)
{
    uint64_t bits; memcpy(&bits, &value, 8);
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(bits >> (8 * i));
}

void fl_appearance_encode(const Fl_Appearance *a, unsigned char out[24])
{
    memcpy(out, a->fill, 4); memcpy(out + 4, a->stroke, 4);
    write_double(out + 8, a->stroke_width); write_double(out + 16, a->font_size);
}
bool fl_appearance_decode(const Fl_Component *c, Fl_Appearance *out)
{
    if (!c || !out || c->type != FL_COMPONENT_APPEARANCE || c->version != 1 || c->size != 24 || !c->data) return false;
    Fl_Appearance a;
    memcpy(a.fill, c->data, 4); memcpy(a.stroke, c->data + 4, 4);
    a.stroke_width = read_double(c->data + 8); a.font_size = read_double(c->data + 16);
    if (!isfinite(a.stroke_width) || a.stroke_width < 0 || a.stroke_width > 65536 ||
        !isfinite(a.font_size) || a.font_size < 1 || a.font_size > 2048) return false;
    *out = a; return true;
}

const Fl_WidgetDef *fl_widget_for_entity(const Fl_Entity *e)
{
    if (!e) return NULL;
    const Fl_Component *geometry = fl_entity_component(e, FL_COMPONENT_GEOMETRY);
    if (geometry) {
        if (geometry->version != 1 || geometry->size != 1 || !geometry->data) return NULL;
        Fl_WidgetKind kind = (Fl_WidgetKind)geometry->data[0];
        return fl_widget_is_geometry(kind) ? fl_widget_definition(kind) : NULL;
    }
    if (fl_entity_component(e, FL_COMPONENT_VIDEO)) return &definitions[FL_WIDGET_VIDEO];
    if (fl_entity_component(e, FL_COMPONENT_IMAGE)) return &definitions[FL_WIDGET_IMAGE];
    if (fl_entity_component(e, FL_COMPONENT_TEXT)) return &definitions[FL_WIDGET_TEXT];
    if (fl_entity_component(e, FL_COMPONENT_RECTANGLE)) return &definitions[FL_WIDGET_RECTANGLE];
    return &definitions[FL_WIDGET_ENTITY];
}

Fl_Appearance fl_widget_appearance(const Fl_Entity *e)
{
    const Fl_WidgetDef *def = fl_widget_for_entity(e);
    Fl_Appearance a = def ? def->appearance : definitions[0].appearance;
    const Fl_Component *rectangle = fl_entity_component(e, FL_COMPONENT_RECTANGLE);
    if (rectangle && rectangle->version == 1 && rectangle->size == 4) memcpy(a.fill, rectangle->data, 4);
    fl_appearance_decode(fl_entity_component(e, FL_COMPONENT_APPEARANCE), &a);
    return a;
}

bool fl_widget_component_valid(const Fl_Component *c)
{
    if (c && c->type==FL_COMPONENT_EVENTS) {cJSON *events=fl_events_read(c); bool valid=events!=NULL; cJSON_Delete(events); return valid;}
    if (!c || !c->type || !c->version || (c->size && !c->data)) return false;
    if (c->type <= FL_COMPONENT_SCRIPT && c->version != (c->type == FL_COMPONENT_ANIMATION ? FL_ANIMATION_VERSION : c->type == FL_COMPONENT_SCRIPT ? FL_SCRIPT_VERSION : 1u)) return false;
    switch (c->type) {
    case FL_COMPONENT_ANIMATION: return c->size == sizeof(Fl_Animation) && fl_animation_valid((const Fl_Animation *)c->data);
    case FL_COMPONENT_SCRIPT: return fl_script_component_valid(c);
    case FL_COMPONENT_PROTOTYPE: return fl_prototype_instance_valid(c);
    case FL_COMPONENT_COMPOSITION: return fl_local_composition_valid(c);
    case FL_COMPONENT_SAMPLED: return fl_sampled_animation_valid(c);
    case FL_COMPONENT_POSE: { Fl_Pose t; return fl_pose_decode(c, &t); }
    case FL_COMPONENT_BOUNDS: { Fl_Bounds b; return fl_bounds_decode(c, &b); }
    case FL_COMPONENT_CAMERA: { Fl_Camera camera; return fl_camera_decode(c, &camera); }
    case FL_COMPONENT_MESH: { Fl_MeshShape shape; return fl_mesh_decode(c, &shape); }
    case FL_COMPONENT_MATERIAL: { Fl_Material material; return fl_material_decode(c, &material); }
    case FL_COMPONENT_LIGHT: { Fl_Light light; return fl_light_decode(c, &light); }
    case FL_COMPONENT_PARTICLES: { Fl_Particles particles; return fl_particles_decode(c, &particles); }
    case FL_COMPONENT_POST: { Fl_Post post; return fl_post_decode(c, &post); }
    case FL_COMPONENT_RECTANGLE: return c->size == 4;
    case FL_COMPONENT_TEXT: return !c->size || fl_json_utf8_valid((const char *)c->data,c->size);
    case FL_COMPONENT_APPEARANCE: { Fl_Appearance a; return fl_appearance_decode(c, &a); }
    case FL_COMPONENT_GEOMETRY: return c->size == 1 && fl_widget_is_geometry((Fl_WidgetKind)c->data[0]);
    case FL_COMPONENT_TYPOGRAPHY: { Fl_Typography t; return fl_typography_decode(c, &t); }
    case FL_COMPONENT_IMAGE: { Fl_Id asset; return fl_image_asset_decode(c, &asset); }
    case FL_COMPONENT_OPACITY: { double opacity; return fl_opacity_decode(c, &opacity); }
    case FL_COMPONENT_STYLE: { Fl_Style style; return fl_style_decode(c, &style); }
    case FL_COMPONENT_VIDEO: { Fl_VideoClip clip; return fl_video_clip_decode(c, &clip); }
    case FL_COMPONENT_PATH: { Fl_Path path; return fl_path_decode(c, &path); }
    case FL_COMPONENT_TEXT_MOTION: { Fl_TextMotion motion; return fl_motion_decode(c, &motion); }
    case FL_COMPONENT_EFFECT: { Fl_Effect effect; return fl_effect_decode(c, &effect); }
    default: return true;
    }
}

bool fl_widget_create(Fl_Project *p, Fl_Id scene, Fl_Id parent, Fl_WidgetKind kind,
                      double x, double y, Fl_Id *out_id)
{
    const Fl_WidgetDef *def = fl_widget_definition(kind);
    if (!def || !isfinite(x) || !isfinite(y)) return false;
    Fl_Pose t = def->pose; t.px = def->bounds.width * 0.5; t.py = def->bounds.height * 0.5; t.x = x + t.px; t.y = y + t.py;
    unsigned char pose[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], appearance[24], geometry = (unsigned char)kind;
    fl_pose_encode(&t, pose); fl_bounds_encode(&def->bounds, bounds_bytes); fl_appearance_encode(&def->appearance, appearance);
    if (kind == FL_WIDGET_IMAGE || kind == FL_WIDGET_VIDEO) return false;
    unsigned char typography[3 + FL_TYPOGRAPHY_FAMILY_MAX];
    unsigned char path_bytes[24 + FL_CURVE_MAX * 49], effect_bytes[FL_EFFECT_HEADER];
    Fl_Component components[6] = {{.type=FL_COMPONENT_POSE, .version=1, .size=FL_POSE_BYTES, .data=pose}, {.type=FL_COMPONENT_BOUNDS, .version=1, .size=FL_BOUNDS_BYTES, .data=bounds_bytes}};
    size_t count = 2;
    if (kind != FL_WIDGET_ENTITY) components[count++] = (Fl_Component){.type = FL_COMPONENT_APPEARANCE, .version = 1, .size = 24, .data = appearance};
    if (kind == FL_WIDGET_RECTANGLE) components[count++] = (Fl_Component){.type = FL_COMPONENT_RECTANGLE, .version = 1, .size = 4, .data = def->appearance.fill};
    else if (kind == FL_WIDGET_TEXT) {
        components[count++] = (Fl_Component){.type = FL_COMPONENT_TEXT, .version = 1, .size = 4, .data = (const unsigned char *)"Text"};
        components[count++] = (Fl_Component){.type = FL_COMPONENT_TYPOGRAPHY, .version = 1, .size = fl_typography_encode(&default_typography, typography), .data = typography};
    }
    else if (kind != FL_WIDGET_ENTITY) components[count++] = (Fl_Component){.type = FL_COMPONENT_GEOMETRY, .version = 1, .size = 1, .data = &geometry};
    if (kind == FL_WIDGET_PATH) {
        Fl_Path path = fl_path_default(def->bounds.width, def->bounds.height);
        size_t size = fl_path_encode(&path, path_bytes, sizeof(path_bytes));
        if (!size) return false;
        components[count++] = (Fl_Component){.type = FL_COMPONENT_PATH, .version = 1, .size = size, .data = path_bytes};
    } else if (kind == FL_WIDGET_EFFECT) {
        Fl_Effect effect = fl_effect_default();
        size_t size = fl_effect_encode(&effect, effect_bytes, sizeof(effect_bytes));
        if (!size) return false;
        components[count++] = (Fl_Component){.type = FL_COMPONENT_EFFECT, .version = 1, .size = size, .data = effect_bytes};
    }
    return fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent,
        .name = def->name, .components = components, .component_count = count}, out_id);
}

bool fl_widget_create_image(Fl_Project *p, Fl_Id scene, Fl_Id parent, Fl_Id asset,
                            double x, double y, double width, double height, Fl_Id *out_id)
{
    if (out_id) *out_id = 0;
    if (!asset || !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) || width <= 0 || height <= 0) return false;
    const Fl_WidgetDef *def = &definitions[FL_WIDGET_IMAGE];
    Fl_Pose t = def->pose; t.px = width * 0.5; t.py = height * 0.5; t.x = x + t.px; t.y = y + t.py;
    Fl_Bounds bounds = {width, height, 0};
    unsigned char pose[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], image[8];
    fl_pose_encode(&t, pose); fl_bounds_encode(&bounds, bounds_bytes); fl_image_asset_encode(asset, image);
    const Fl_Asset *record = fl_project_asset(p, asset);
    const Fl_Component components[3] = {{.type=FL_COMPONENT_POSE, .version=1, .size=FL_POSE_BYTES, .data=pose}, {.type=FL_COMPONENT_BOUNDS, .version=1, .size=FL_BOUNDS_BYTES, .data=bounds_bytes}, {.type=FL_COMPONENT_IMAGE, .version=1, .size=8, .data=image}};
    return fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent,
        .name = record && record->name[0] ? record->name : def->name, .components = components, .component_count = 3}, out_id);
}

bool fl_widget_create_video(Fl_Project *p, Fl_Id scene, Fl_Id parent, Fl_Id asset,
                            double x, double y, double width, double height, Fl_Id *out_id)
{
    if (out_id) *out_id = 0;
    if (!asset || !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) || width <= 0 || height <= 0) return false;
    const Fl_WidgetDef *def = &definitions[FL_WIDGET_VIDEO];
    Fl_Pose t = def->pose; t.px = width * 0.5; t.py = height * 0.5; t.x = x + t.px; t.y = y + t.py;
    Fl_Bounds bounds = {width, height, 0};
    unsigned char pose[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], clip_bytes[FL_VIDEO_CLIP_BYTES];
    fl_pose_encode(&t, pose); fl_bounds_encode(&bounds, bounds_bytes);
    fl_video_clip_encode(&(Fl_VideoClip){.asset = asset, .rate = 1}, clip_bytes);
    const Fl_Asset *record = fl_project_asset(p, asset);
    const Fl_Component components[3] = {{.type=FL_COMPONENT_POSE, .version=1, .size=FL_POSE_BYTES, .data=pose}, {.type=FL_COMPONENT_BOUNDS, .version=1, .size=FL_BOUNDS_BYTES, .data=bounds_bytes}, {.type=FL_COMPONENT_VIDEO, .version=1, .size=FL_VIDEO_CLIP_BYTES, .data=clip_bytes}};
    return fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent,
        .name = record && record->name[0] ? record->name : def->name, .components = components, .component_count = 3}, out_id);
}

bool fl_component_asset_reference(const Fl_Component *c, Fl_Id *asset, Fl_AssetKind *kind)
{
    Fl_PrototypeInstance instance;
    if (fl_prototype_instance_decode(c,&instance)) {if (asset) *asset=instance.asset; if (kind) *kind=FL_ASSET_PROTOTYPE; return true;}
    Fl_Script script;
    if (fl_script_decode(c,&script)) {if (asset) *asset=script.script; if (kind) *kind=FL_ASSET_LUA; return true;}

    Fl_Id id; Fl_VideoClip clip;
    if (c && fl_image_asset_decode(c, &id)) {if (asset) *asset = id; if (kind) *kind = FL_ASSET_IMAGE; return true;}
    if (c && fl_video_clip_decode(c, &clip)) {if (asset) *asset = clip.asset; if (kind) *kind = FL_ASSET_VIDEO; return true;}
    return false;
}

size_t fl_typography_encode(const Fl_Typography *t, unsigned char *out)
{
    const char *end = t ? memchr(t->family, 0, sizeof(t->family)) : NULL;
    size_t length = end ? (size_t)(end - t->family) : 0;
    if (!t || !out || !length || length > FL_TYPOGRAPHY_FAMILY_MAX || t->weight < 1 || t->weight > 1000) return 0;
    out[0] = (unsigned char)(t->weight & 0xff); out[1] = (unsigned char)(t->weight >> 8); out[2] = t->italic ? 1 : 0;
    memcpy(out + 3, t->family, length);
    return length + 3;
}

bool fl_typography_decode(const Fl_Component *c, Fl_Typography *out)
{
    if (!c || !out || c->type != FL_COMPONENT_TYPOGRAPHY || c->version != 1 || !c->data ||
        c->size < 4 || c->size > 3 + FL_TYPOGRAPHY_FAMILY_MAX) return false;
    unsigned weight = (unsigned)c->data[0] | (unsigned)c->data[1] << 8;
    size_t length = c->size - 3;
    if (weight < 1 || weight > 1000 || c->data[2] > 1 || !fl_json_utf8_valid((const char *)c->data+3,length)) return false;
    memcpy(out->family, c->data + 3, length); out->family[length] = 0;
    out->weight = weight; out->italic = c->data[2] != 0;
    return true;
}

Fl_Typography fl_widget_typography(const Fl_Entity *e)
{
    Fl_Typography t = default_typography;
    fl_typography_decode(fl_entity_component(e, FL_COMPONENT_TYPOGRAPHY), &t);
    return t;
}

void fl_image_asset_encode(Fl_Id asset, unsigned char out[8])
{
    for (unsigned i = 0; i < 8; ++i) out[i] = (unsigned char)(asset >> (8 * i));
}

bool fl_image_asset_decode(const Fl_Component *c, Fl_Id *asset)
{
    if (!c || !asset || c->type != FL_COMPONENT_IMAGE || c->version != 1 || c->size != 8 || !c->data) return false;
    Fl_Id id = 0;
    for (unsigned i = 0; i < 8; ++i) id |= (Fl_Id)c->data[i] << (8 * i);
    if (!id) return false;
    *asset = id; return true;
}

void fl_opacity_encode(double opacity, unsigned char out[8]) { write_double(out, opacity); }

bool fl_opacity_decode(const Fl_Component *c, double *opacity)
{
    if (!c || !opacity || c->type != FL_COMPONENT_OPACITY || c->version != 1 || c->size != 8 || !c->data) return false;
    double value = read_double(c->data);
    if (!isfinite(value) || value < 0 || value > 1) return false;
    *opacity = value; return true;
}

double fl_widget_opacity(const Fl_Entity *e)
{
    double opacity = 1;
    fl_opacity_decode(fl_entity_component(e, FL_COMPONENT_OPACITY), &opacity);
    return opacity;
}
