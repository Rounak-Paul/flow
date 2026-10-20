#include "fl_properties.h"
#include "fl_widgets.h"
#include <math.h>

static const Fl_PropertyDef properties[FL_PROP_COUNT] = {
    [FL_PROP_X] = {"x", "X", "transform", "x", 1, -1e9, 1e9},
    [FL_PROP_Y] = {"y", "Y", "transform", "y", 1, -1e9, 1e9},
    [FL_PROP_WIDTH] = {"width", "Width", "transform", "width", 1, 0, 1e9},
    [FL_PROP_HEIGHT] = {"height", "Height", "transform", "height", 1, 0, 1e9},
    [FL_PROP_ROTATION] = {"rotation", "Rotation", "transform", "rotation", 1, -1e9, 1e9},
    [FL_PROP_SCALE_X] = {"scale_x", "Scale X", "transform", "scale_x", 1, -1e9, 1e9},
    [FL_PROP_SCALE_Y] = {"scale_y", "Scale Y", "transform", "scale_y", 1, -1e9, 1e9},
    [FL_PROP_OPACITY] = {"opacity", "Opacity", NULL, "opacity", 1, 0, 1},
    [FL_PROP_FILL] = {"fill", "Fill", "appearance", "fill", 4, 0, 255},
    [FL_PROP_STROKE] = {"stroke", "Stroke", "appearance", "stroke", 4, 0, 255},
    [FL_PROP_STROKE_WIDTH] = {"stroke_width", "Stroke width", "appearance", "stroke_width", 1, 0, 65536},
    [FL_PROP_FONT_SIZE] = {"font_size", "Font size", "appearance", "font_size", 1, 1, 2048},
    [FL_PROP_CROP_X] = {"crop_x", "Crop X", "crop", "x", 1, 0, 1},
    [FL_PROP_CROP_Y] = {"crop_y", "Crop Y", "crop", "y", 1, 0, 1},
    [FL_PROP_CROP_W] = {"crop_w", "Crop width", "crop", "width", 1, 0.001, 1},
    [FL_PROP_CROP_H] = {"crop_h", "Crop height", "crop", "height", 1, 0.001, 1},
};

const Fl_PropertyDef *fl_property_definition(Fl_AnimProperty property)
{
    return property >= 0 && property < FL_PROP_COUNT ? &properties[property] : NULL;
}

const char *fl_property_name(Fl_AnimProperty property)
{
    const Fl_PropertyDef *definition = fl_property_definition(property);
    return definition ? definition->name : NULL;
}

bool fl_property_value_valid(Fl_AnimProperty property, const double values[4])
{
    const Fl_PropertyDef *definition = fl_property_definition(property);
    if (!definition || !values) return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(values[i])) return false;
        if (i < definition->channels && (values[i] < definition->minimum || values[i] > definition->maximum)) return false;
    }
    return true;
}

bool fl_property_supported(const Fl_Entity *entity, Fl_AnimProperty property)
{
    if (!entity || !fl_property_definition(property)) return false;
    if (property <= FL_PROP_OPACITY) return fl_entity_component(entity, FL_COMPONENT_TRANSFORM) != NULL;
    const Fl_WidgetDef *definition = fl_widget_for_entity(entity);
    if (!definition) return false;
    if (property == FL_PROP_FILL) return definition->has_fill;
    if (property == FL_PROP_STROKE || property == FL_PROP_STROKE_WIDTH) return definition->has_stroke;
    if (property >= FL_PROP_CROP_X) return definition->has_image || definition->has_video;
    return property == FL_PROP_FONT_SIZE && definition->has_text;
}
