#pragma once
#include "project/fl_project.h"

typedef enum Fl_WidgetKind {
    FL_WIDGET_GROUP, FL_WIDGET_RECTANGLE, FL_WIDGET_ELLIPSE,
    FL_WIDGET_LINE, FL_WIDGET_ARROW, FL_WIDGET_TEXT, FL_WIDGET_IMAGE, FL_WIDGET_VIDEO,
    FL_WIDGET_TRIANGLE, FL_WIDGET_DIAMOND, FL_WIDGET_HEXAGON, FL_WIDGET_STAR, FL_WIDGET_COUNT
} Fl_WidgetKind;
#define FL_TYPOGRAPHY_DEFAULT_FAMILY "Inter"
#define FL_TYPOGRAPHY_FAMILY_MAX 255
/* Text face request resolved by family name, CSS weight (1..1000) and italic flag. */
typedef struct Fl_Typography {
    char family[FL_TYPOGRAPHY_FAMILY_MAX + 1];
    unsigned weight;
    bool italic;
} Fl_Typography;
/* Appearance colours are sRGB RGBA8; lengths are local scene pixels. */
typedef struct Fl_Appearance {
    unsigned char fill[4], stroke[4];
    double stroke_width, font_size;
} Fl_Appearance;
/* One registry entry defines a primitive's creation defaults and editable capabilities. */
typedef struct Fl_WidgetDef {
    Fl_WidgetKind kind;
    const char *name, *description;
    Fl_Transform transform;
    Fl_Appearance appearance;
    bool has_fill, has_stroke, has_text, has_image, has_video;
} Fl_WidgetDef;
/* True for the fill-and-stroke polygon shapes (triangle, diamond, hexagon, star). */
bool fl_widget_is_polygon(Fl_WidgetKind kind);
/* True for kinds stored as a geometry component (ellipse, line, arrow and the polygons). */
bool fl_widget_is_geometry(Fl_WidgetKind kind);
#define FL_POLYGON_MAX_VERTICES 10
/*
 * Outline of a polygon kind in coordinates normalized to the box (-1..1 on both axes, y downwards), in drawing order.
 * Writes up to FL_POLYGON_MAX_VERTICES points to out and returns their count; zero for other kinds. This is the
 * single definition of the outline: picking, thumbnails and the GPU shader all derive from it.
 */
size_t fl_widget_polygon(Fl_WidgetKind kind, double out[FL_POLYGON_MAX_VERTICES][2]);
/*
 * Whether the point (nx, ny) lies inside an ellipse or polygon kind, in coordinates normalized to the box:
 * -1..1 on both axes, y downwards. False for other kinds.
 */
bool fl_widget_shape_contains(Fl_WidgetKind kind, double nx, double ny);
/* Borrow the immutable definition for kind; NULL for invalid kinds. */
const Fl_WidgetDef *fl_widget_definition(Fl_WidgetKind kind);
/* Infer entity's primitive from its persisted components; NULL for unknown geometry. */
const Fl_WidgetDef *fl_widget_for_entity(const Fl_Entity *entity);
/* Create a fully initialized primitive atomically at local x/y under parent. */
bool fl_widget_create(Fl_Project *project, Fl_Id scene, Fl_Id parent, Fl_WidgetKind kind,
                      double x, double y, Fl_Id *out_id);
/* Create an image primitive showing an image asset at local x/y with the given size. */
bool fl_widget_create_image(Fl_Project *project, Fl_Id scene, Fl_Id parent, Fl_Id asset,
                            double x, double y, double width, double height, Fl_Id *out_id);
/* Create a video primitive playing a video asset from scene time zero at local x/y with the given size. */
bool fl_widget_create_video(Fl_Project *project, Fl_Id scene, Fl_Id parent, Fl_Id asset,
                            double x, double y, double width, double height, Fl_Id *out_id);
/* Validate a component against its schema; unknown type/version pairs remain opaque. */
bool fl_widget_component_valid(const Fl_Component *component);
/* Encode/decode canonical appearance bytes; decode rejects unsupported versions and invalid values. */
void fl_appearance_encode(const Fl_Appearance *appearance, unsigned char out[24]);
bool fl_appearance_decode(const Fl_Component *component, Fl_Appearance *out);
/* Read appearance, falling back to the entity primitive's defaults and rectangle colour. */
Fl_Appearance fl_widget_appearance(const Fl_Entity *entity);
/* Report the asset a component references and the asset kind it requires; false when none. */
bool fl_component_asset_reference(const Fl_Component *component, Fl_Id *asset, Fl_AssetKind *kind);
/* Encode typography into out (capacity >= 3 + FL_TYPOGRAPHY_FAMILY_MAX); returns the encoded size, 0 when invalid. */
size_t fl_typography_encode(const Fl_Typography *typography, unsigned char *out);
bool fl_typography_decode(const Fl_Component *component, Fl_Typography *out);
/* Read typography, falling back to the default family at regular weight. */
Fl_Typography fl_widget_typography(const Fl_Entity *entity);
/* Encode/decode the image component's referenced asset id. */
void fl_image_asset_encode(Fl_Id asset, unsigned char out[8]);
bool fl_image_asset_decode(const Fl_Component *component, Fl_Id *asset);
/* Encode/decode the base opacity component (LE binary64 in 0..1). */
void fl_opacity_encode(double opacity, unsigned char out[8]);
bool fl_opacity_decode(const Fl_Component *component, double *opacity);
/* Read base opacity, defaulting to fully opaque. */
double fl_widget_opacity(const Fl_Entity *entity);
