#include "fl_scene_frame.h"
#include "components/fl_component.h"
#include "components/fl_spatial.h"

Fl_SceneFrame fl_scene_frame(const Fl_Scene *scene)
{
    if (!scene || !scene->width || !scene->height) return (Fl_SceneFrame){0};
    double width = (double)scene->width, height = (double)scene->height;
    uint64_t aspect_w = scene->aspect_width, aspect_h = scene->aspect_height;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        const Fl_Entity *entity = &scene->entities[i];
        Fl_Camera camera;
        if (!entity->visible || !fl_camera_decode(fl_entity_component(entity, FL_COMPONENT_CAMERA), &camera)) continue;
        if (camera.aspect_w && camera.aspect_h) { aspect_w = camera.aspect_w; aspect_h = camera.aspect_h; }
        break;
    }
    if (aspect_w && aspect_h) {
        double ratio = (double)aspect_w / (double)aspect_h;
        if (width / height > ratio) width = height * ratio;
        else height = width / ratio;
    }
    return (Fl_SceneFrame){(scene->width - width) * .5, (scene->height - height) * .5, width, height};
}
