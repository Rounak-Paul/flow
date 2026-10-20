#include "fl_project.h"
#include "components/fl_script.h"

size_t fl_scene_script_uses(const Fl_Scene *scene, Fl_Id script)
{
    size_t uses=0; Fl_Script attached;
    for (size_t i=0;scene && script && i<scene->entity_count;++i)
        for (size_t j=0;j<scene->entities[i].component_count;++j) uses+=fl_script_decode(&scene->entities[i].components[j],&attached) && attached.script==script;
    return uses;
}
