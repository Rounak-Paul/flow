#include "fl_scene_frame.h"

Fl_SceneFrame fl_scene_frame(const Fl_Scene *scene)
{
    if (!scene || !scene->width || !scene->height) return (Fl_SceneFrame){0};
    double width = (double)scene->width, height = (double)scene->height;
    if (scene->aspect_width && scene->aspect_height) {
        double ratio = (double)scene->aspect_width / scene->aspect_height;
        if (width / height > ratio) width = height * ratio;
        else height = width / ratio;
    }
    return (Fl_SceneFrame){(scene->width - width) * .5 + scene->camera_x, (scene->height - height) * .5 + scene->camera_y, width, height};
}
