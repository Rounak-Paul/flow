#include "fl_properties.h"
#include "components/fl_component.h"
#include "fl_widgets.h"
#include <math.h>

static const Fl_PropertyDef properties[FL_PROP_COUNT] = {
    [FL_PROP_X] = {"x", "X", "pose", FL_COMPONENT_POSE, "pose", "x", 1, -1e9, 1e9},
    [FL_PROP_Y] = {"y", "Y", "pose", FL_COMPONENT_POSE, "pose", "y", 1, -1e9, 1e9},
    [FL_PROP_Z] = {"z", "Z", "pose", FL_COMPONENT_POSE, "pose", "z", 1, -1e9, 1e9},
    [FL_PROP_RX] = {"rx", "Rotate X", "pose", FL_COMPONENT_POSE, "pose", "rx", 1, -1e9, 1e9},
    [FL_PROP_RY] = {"ry", "Rotate Y", "pose", FL_COMPONENT_POSE, "pose", "ry", 1, -1e9, 1e9},
    [FL_PROP_RZ] = {"rz", "Rotate Z", "pose", FL_COMPONENT_POSE, "pose", "rz", 1, -1e9, 1e9},
    [FL_PROP_SCALE_X] = {"scale_x", "Scale X", "pose", FL_COMPONENT_POSE, "pose", "scale_x", 1, -1e9, 1e9},
    [FL_PROP_SCALE_Y] = {"scale_y", "Scale Y", "pose", FL_COMPONENT_POSE, "pose", "scale_y", 1, -1e9, 1e9},
    [FL_PROP_SCALE_Z] = {"scale_z", "Scale Z", "pose", FL_COMPONENT_POSE, "pose", "scale_z", 1, -1e9, 1e9},
    [FL_PROP_PIVOT_X] = {"pivot_x", "Pivot X", "pose", FL_COMPONENT_POSE, "pose", "pivot_x", 1, -1e9, 1e9},
    [FL_PROP_PIVOT_Y] = {"pivot_y", "Pivot Y", "pose", FL_COMPONENT_POSE, "pose", "pivot_y", 1, -1e9, 1e9},
    [FL_PROP_PIVOT_Z] = {"pivot_z", "Pivot Z", "pose", FL_COMPONENT_POSE, "pose", "pivot_z", 1, -1e9, 1e9},
    [FL_PROP_WIDTH] = {"width", "Width", "bounds", FL_COMPONENT_BOUNDS, "bounds", "width", 1, 0, 1e9},
    [FL_PROP_HEIGHT] = {"height", "Height", "bounds", FL_COMPONENT_BOUNDS, "bounds", "height", 1, 0, 1e9},
    [FL_PROP_DEPTH] = {"depth", "Depth", "bounds", FL_COMPONENT_BOUNDS, "bounds", "depth", 1, 0, 1e9},
    [FL_PROP_OPACITY] = {"opacity", "Opacity", "opacity", FL_COMPONENT_OPACITY, NULL, "opacity", 1, 0, 1},
    [FL_PROP_FILL] = {"fill", "Fill", "appearance", FL_COMPONENT_APPEARANCE, "appearance", "fill", 4, 0, 255},
    [FL_PROP_STROKE] = {"stroke", "Stroke", "appearance", FL_COMPONENT_APPEARANCE, "appearance", "stroke", 4, 0, 255},
    [FL_PROP_STROKE_WIDTH] = {"stroke_width", "Stroke width", "appearance", FL_COMPONENT_APPEARANCE, "appearance", "stroke_width", 1, 0, 65536},
    [FL_PROP_FONT_SIZE] = {"font_size", "Font size", "appearance", FL_COMPONENT_APPEARANCE, "appearance", "font_size", 1, 1, 2048},
    [FL_PROP_CROP_X] = {"crop_x", "Crop X", "style", FL_COMPONENT_STYLE, "crop", "x", 1, 0, 1},
    [FL_PROP_CROP_Y] = {"crop_y", "Crop Y", "style", FL_COMPONENT_STYLE, "crop", "y", 1, 0, 1},
    [FL_PROP_CROP_W] = {"crop_w", "Crop width", "style", FL_COMPONENT_STYLE, "crop", "width", 1, 0.001, 1},
    [FL_PROP_CROP_H] = {"crop_h", "Crop height", "style", FL_COMPONENT_STYLE, "crop", "height", 1, 0.001, 1},
    [FL_PROP_RADIUS] = {"radius", "Corner radius", "style", FL_COMPONENT_STYLE, "style", "radius", 1, 0, 65536},
    [FL_PROP_BLUR] = {"blur", "Blur", "style", FL_COMPONENT_STYLE, "style", "blur", 1, 0, 512},
    [FL_PROP_GRADIENT_ANGLE] = {"gradient_angle", "Gradient angle", "style", FL_COMPONENT_STYLE, "style", "gradient_angle", 1, -1000, 1000},
    [FL_PROP_SHADOW_BLUR] = {"shadow_blur", "Shadow blur", "style", FL_COMPONENT_STYLE, "style", "shadow_blur", 1, 0, 4096},
    [FL_PROP_SHADOW_X] = {"shadow_x", "Shadow X", "style", FL_COMPONENT_STYLE, "style", "shadow_x", 1, -65536, 65536},
    [FL_PROP_SHADOW_Y] = {"shadow_y", "Shadow Y", "style", FL_COMPONENT_STYLE, "style", "shadow_y", 1, -65536, 65536},
    [FL_PROP_GRADIENT_TO] = {"gradient_to", "Gradient colour", "style", FL_COMPONENT_STYLE, "style", "gradient_to", 4, 0, 255},
    [FL_PROP_SHADOW_COLOR] = {"shadow_color", "Shadow colour", "style", FL_COMPONENT_STYLE, "style", "shadow_color", 4, 0, 255},
    [FL_PROP_MEDIA_TIME] = {"media_time", "Video source time", "video", FL_COMPONENT_VIDEO, "media", "seconds", 1, -1, 1e9},
    [FL_PROP_FOV] = {"fov", "Field of view", "camera", FL_COMPONENT_CAMERA, "camera", "fov", 1, 0.05, 3},
    [FL_PROP_NEAR] = {"near", "Near", "camera", FL_COMPONENT_CAMERA, "camera", "near", 1, 1e-4, 1e6},
    [FL_PROP_FAR] = {"far", "Far", "camera", FL_COMPONENT_CAMERA, "camera", "far", 1, 1e-3, 1e7},
    [FL_PROP_EXPOSURE] = {"exposure", "Exposure", "camera", FL_COMPONENT_CAMERA, "camera", "exposure", 1, 1e-3, 64},
    [FL_PROP_ORTHO] = {"ortho_height", "Ortho height", "camera", FL_COMPONENT_CAMERA, "camera", "ortho_height", 1, 0, 1e9},
    [FL_PROP_ROUGHNESS] = {"roughness", "Roughness", "material", FL_COMPONENT_MATERIAL, "material", "roughness", 1, 0, 1},
    [FL_PROP_METALLIC] = {"metallic", "Metallic", "material", FL_COMPONENT_MATERIAL, "material", "metallic", 1, 0, 1},
    [FL_PROP_EMISSIVE] = {"emissive", "Emissive", "material", FL_COMPONENT_MATERIAL, "material", "emissive", 1, 0, 64},
    [FL_PROP_INTENSITY] = {"intensity", "Intensity", "light", FL_COMPONENT_LIGHT, "light", "intensity", 1, 0, 100000},
    [FL_PROP_RANGE] = {"range", "Range", "light", FL_COMPONENT_LIGHT, "light", "range", 1, 0, 1e7},
    [FL_PROP_SPOT_ANGLE] = {"spot_angle", "Spot angle", "light", FL_COMPONENT_LIGHT, "light", "spot_angle", 1, 0.01, 1.5},
    [FL_PROP_LIGHT_COLOR] = {"light_color", "Colour", "light", FL_COMPONENT_LIGHT, "light", "color", 4, 0, 255},
    [FL_PROP_EMIT_RATE] = {"rate", "Rate", "particles", FL_COMPONENT_PARTICLES, "particles", "rate", 1, 0, 10000},
    [FL_PROP_LIFE] = {"life", "Life", "particles", FL_COMPONENT_PARTICLES, "particles", "life", 1, 1e-3, 120},
    [FL_PROP_PARTICLE_SIZE] = {"size", "Size", "particles", FL_COMPONENT_PARTICLES, "particles", "size", 1, 1e-3, 4096},
    [FL_PROP_SPEED] = {"speed", "Speed", "particles", FL_COMPONENT_PARTICLES, "particles", "speed", 1, 0, 100000},
    [FL_PROP_PARTICLE_COLOR] = {"particle_color", "Colour", "particles", FL_COMPONENT_PARTICLES, "particles", "color", 4, 0, 255},
    [FL_PROP_BLOOM] = {"bloom", "Bloom", "post", FL_COMPONENT_POST, "post", "bloom", 1, 0, 8},
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
    const Fl_PropertyDef *channel = fl_property_definition(property);
    if (!entity || !channel) return false;
    if (property <= FL_PROP_PIVOT_Z || property == FL_PROP_OPACITY) return fl_entity_component(entity, FL_COMPONENT_POSE) != NULL;
    if (property >= FL_PROP_WIDTH && property <= FL_PROP_DEPTH) return fl_entity_component(entity, FL_COMPONENT_BOUNDS) != NULL;
    if (property >= FL_PROP_FOV && property <= FL_PROP_ORTHO) return fl_entity_component(entity, FL_COMPONENT_CAMERA) != NULL;
    if (property >= FL_PROP_ROUGHNESS && property <= FL_PROP_EMISSIVE) return fl_entity_component(entity, FL_COMPONENT_MATERIAL) != NULL;
    if (property >= FL_PROP_INTENSITY && property <= FL_PROP_LIGHT_COLOR) return fl_entity_component(entity, FL_COMPONENT_LIGHT) != NULL;
    if (property >= FL_PROP_EMIT_RATE && property <= FL_PROP_PARTICLE_COLOR) return fl_entity_component(entity, FL_COMPONENT_PARTICLES) != NULL;
    if (property == FL_PROP_BLOOM) return fl_entity_component(entity, FL_COMPONENT_POST) != NULL;
    const Fl_WidgetDef *definition = fl_widget_for_entity(entity);
    if (!definition) return false;
    if (property == FL_PROP_FILL) return definition->has_fill;
    if (property == FL_PROP_STROKE || property == FL_PROP_STROKE_WIDTH) return definition->has_stroke;
    if (property >= FL_PROP_RADIUS && property < FL_PROP_MEDIA_TIME) return true;
    if (property == FL_PROP_MEDIA_TIME) return definition->has_video;
    if (property >= FL_PROP_CROP_X && property <= FL_PROP_CROP_H) return definition->has_image || definition->has_video;
    return property == FL_PROP_FONT_SIZE && definition->has_text;
}
