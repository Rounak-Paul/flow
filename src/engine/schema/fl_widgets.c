#include "fl_widgets.h"
#include "components/fl_animation.h"
#include "components/fl_script.h"
#include "components/fl_style.h"
#include "components/fl_video_clip.h"
#include <math.h>
#include <string.h>

static const Fl_WidgetDef definitions[FL_WIDGET_COUNT] = {
    {FL_WIDGET_GROUP, "Group", "Organize related entities", {0,0,320,180,0,1,1}, {{91,156,255,255},{230,233,238,255},0,48}, false,false,false,false,false},
    {FL_WIDGET_RECTANGLE, "Rectangle", "Filled or outlined rectangle", {0,0,320,180,0,1,1}, {{91,156,255,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_ELLIPSE, "Ellipse", "Circle or ellipse", {0,0,240,240,0,1,1}, {{76,195,138,255},{230,233,238,255},0,48}, true,true,false,false,false},
    {FL_WIDGET_LINE, "Line", "Straight connector", {0,0,360,32,0,1,1}, {{230,233,238,255},{230,233,238,255},6,48}, false,true,false,false,false},
    {FL_WIDGET_ARROW, "Arrow", "Directional connector", {0,0,360,72,0,1,1}, {{232,176,74,255},{232,176,74,255},6,48}, false,true,false,false,false},
    {FL_WIDGET_TEXT, "Text", "Editable text block", {0,0,480,160,0,1,1}, {{230,233,238,255},{0,0,0,0},0,48}, true,false,true,false,false},
    {FL_WIDGET_IMAGE, "Image", "Picture from the project's assets", {0,0,320,180,0,1,1}, {{255,255,255,255},{0,0,0,0},0,48}, false,false,false,true,false},
    {FL_WIDGET_VIDEO, "Video", "Video clip from the project's assets", {0,0,320,180,0,1,1}, {{255,255,255,255},{0,0,0,0},0,48}, false,false,false,false,true},
};
static const Fl_Typography default_typography = {FL_TYPOGRAPHY_DEFAULT_FAMILY, 400, false};

const Fl_WidgetDef *fl_widget_definition(Fl_WidgetKind kind)
{
    return kind >= 0 && kind < FL_WIDGET_COUNT ? &definitions[kind] : NULL;
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
        return kind >= FL_WIDGET_ELLIPSE && kind <= FL_WIDGET_ARROW ? fl_widget_definition(kind) : NULL;
    }
    if (fl_entity_component(e, FL_COMPONENT_VIDEO)) return &definitions[FL_WIDGET_VIDEO];
    if (fl_entity_component(e, FL_COMPONENT_IMAGE)) return &definitions[FL_WIDGET_IMAGE];
    if (fl_entity_component(e, FL_COMPONENT_TEXT)) return &definitions[FL_WIDGET_TEXT];
    if (fl_entity_component(e, FL_COMPONENT_RECTANGLE)) return &definitions[FL_WIDGET_RECTANGLE];
    return &definitions[FL_WIDGET_GROUP];
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
    if (!c || !c->type || !c->version || (c->size && !c->data)) return false;
    if (c->type <= FL_COMPONENT_SCRIPT && c->version != (c->type == FL_COMPONENT_ANIMATION ? FL_ANIMATION_VERSION : c->type == FL_COMPONENT_SCRIPT ? FL_SCRIPT_VERSION : 1u)) return false;
    switch (c->type) {
    case FL_COMPONENT_ANIMATION: return c->size == sizeof(Fl_Animation) && fl_animation_valid((const Fl_Animation *)c->data);
    case FL_COMPONENT_SCRIPT: {Fl_Script script; return fl_script_decode(c, &script);}
    case FL_COMPONENT_TRANSFORM: { Fl_Transform t; return fl_transform_decode(c, &t); }
    case FL_COMPONENT_RECTANGLE: return c->size == 4;
    case FL_COMPONENT_TEXT: return !c->size || !memchr(c->data, 0, c->size);
    case FL_COMPONENT_APPEARANCE: { Fl_Appearance a; return fl_appearance_decode(c, &a); }
    case FL_COMPONENT_GEOMETRY: return c->size == 1 && c->data[0] >= FL_WIDGET_ELLIPSE && c->data[0] <= FL_WIDGET_ARROW;
    case FL_COMPONENT_TYPOGRAPHY: { Fl_Typography t; return fl_typography_decode(c, &t); }
    case FL_COMPONENT_IMAGE: { Fl_Id asset; return fl_image_asset_decode(c, &asset); }
    case FL_COMPONENT_OPACITY: { double opacity; return fl_opacity_decode(c, &opacity); }
    case FL_COMPONENT_STYLE: { Fl_Style style; return fl_style_decode(c, &style); }
    case FL_COMPONENT_VIDEO: { Fl_VideoClip clip; return fl_video_clip_decode(c, &clip); }
    default: return true;
    }
}

bool fl_widget_create(Fl_Project *p, Fl_Id scene, Fl_Id parent, Fl_WidgetKind kind,
                      double x, double y, Fl_Id *out_id)
{
    const Fl_WidgetDef *def = fl_widget_definition(kind);
    if (!def || !isfinite(x) || !isfinite(y)) return false;
    Fl_Transform t = def->transform; t.x = x; t.y = y;
    unsigned char transform[56], appearance[24], geometry = (unsigned char)kind;
    fl_transform_encode(&t, transform); fl_appearance_encode(&def->appearance, appearance);
    if (kind == FL_WIDGET_IMAGE || kind == FL_WIDGET_VIDEO) return false;
    unsigned char typography[3 + FL_TYPOGRAPHY_FAMILY_MAX];
    Fl_Component components[4] = {{FL_COMPONENT_TRANSFORM,1,56,transform}};
    size_t count = 1;
    if (kind != FL_WIDGET_GROUP) components[count++] = (Fl_Component){FL_COMPONENT_APPEARANCE,1,24,appearance};
    if (kind == FL_WIDGET_RECTANGLE) components[count++] = (Fl_Component){FL_COMPONENT_RECTANGLE,1,4,def->appearance.fill};
    else if (kind == FL_WIDGET_TEXT) {
        components[count++] = (Fl_Component){FL_COMPONENT_TEXT,1,4,(const unsigned char *)"Text"};
        components[count++] = (Fl_Component){FL_COMPONENT_TYPOGRAPHY,1,fl_typography_encode(&default_typography, typography),typography};
    }
    else if (kind != FL_WIDGET_GROUP) components[count++] = (Fl_Component){FL_COMPONENT_GEOMETRY,1,1,&geometry};
    return fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent,
        .name = def->name, .components = components, .component_count = count}, out_id);
}

bool fl_widget_create_image(Fl_Project *p, Fl_Id scene, Fl_Id parent, Fl_Id asset,
                            double x, double y, double width, double height, Fl_Id *out_id)
{
    if (out_id) *out_id = 0;
    if (!asset || !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) || width <= 0 || height <= 0) return false;
    const Fl_WidgetDef *def = &definitions[FL_WIDGET_IMAGE];
    Fl_Transform t = def->transform; t.x = x; t.y = y; t.width = width; t.height = height;
    unsigned char transform[56], image[8];
    fl_transform_encode(&t, transform); fl_image_asset_encode(asset, image);
    const Fl_Asset *record = fl_project_asset(p, asset);
    const Fl_Component components[2] = {{FL_COMPONENT_TRANSFORM,1,56,transform}, {FL_COMPONENT_IMAGE,1,8,image}};
    return fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent,
        .name = record && record->name[0] ? record->name : def->name, .components = components, .component_count = 2}, out_id);
}

bool fl_widget_create_video(Fl_Project *p, Fl_Id scene, Fl_Id parent, Fl_Id asset,
                            double x, double y, double width, double height, Fl_Id *out_id)
{
    if (out_id) *out_id = 0;
    if (!asset || !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) || width <= 0 || height <= 0) return false;
    const Fl_WidgetDef *def = &definitions[FL_WIDGET_VIDEO];
    Fl_Transform t = def->transform; t.x = x; t.y = y; t.width = width; t.height = height;
    unsigned char transform[56], clip_bytes[FL_VIDEO_CLIP_BYTES];
    fl_transform_encode(&t, transform);
    fl_video_clip_encode(&(Fl_VideoClip){.asset = asset, .rate = 1}, clip_bytes);
    const Fl_Asset *record = fl_project_asset(p, asset);
    const Fl_Component components[2] = {{FL_COMPONENT_TRANSFORM,1,56,transform}, {FL_COMPONENT_VIDEO,1,FL_VIDEO_CLIP_BYTES,clip_bytes}};
    return fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent,
        .name = record && record->name[0] ? record->name : def->name, .components = components, .component_count = 2}, out_id);
}

bool fl_component_asset_reference(const Fl_Component *c, Fl_Id *asset, Fl_AssetKind *kind)
{
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
    if (weight < 1 || weight > 1000 || c->data[2] > 1 || memchr(c->data + 3, 0, length)) return false;
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
