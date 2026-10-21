#include "fl_prototype_authoring.h"
#include "fl_prototype.h"
#include "fl_scene_storage.h"
#include "fl_timeline_internal.h"
#include "components/fl_prototype_instance.h"
#include "components/fl_local_composition.h"
#include "doc/fl_document.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Mark the selected hierarchy in linear graph work, independently of entity array ordering. */
static bool subtree(const Fl_Scene *scene, Fl_Id root, unsigned char *membership)
{
    const Fl_Entity *selected=fl_scene_entity(scene,root); if (!selected) return false;
    size_t *path=scene->entity_count ? malloc(scene->entity_count*sizeof(*path)) : NULL;
    if (scene->entity_count && !path) return false;
    membership[selected-scene->entities]=2;
    for (size_t i=0;i<scene->entity_count;++i) if (!membership[i]) {
        const Fl_Entity *entity=&scene->entities[i]; size_t depth=0;
        while (entity && !membership[entity-scene->entities]) {
            size_t index=(size_t)(entity-scene->entities); membership[index]=3; path[depth++]=index;
            entity=fl_scene_entity(scene,entity->parent);
        }
        unsigned char value=entity && membership[entity-scene->entities]==2 ? 2 : 1;
        while (depth) membership[path[--depth]]=value;
    }
    free(path); return true;
}

bool fl_prototype_capture(Fl_Project *project, Fl_Id scene_id, Fl_Id root, const char *name, Fl_Id *asset)
{
    if (asset) *asset=0;
    const Fl_Scene *scene=fl_project_scene(project,scene_id);
    if (!scene || !asset || !name) return false;
    unsigned char *membership=calloc(scene->entity_count,1);
    if (!membership || !subtree(scene,root,membership)) {free(membership); return false;}
    Fl_Json context={0}; cJSON *document=fl_document_scene_write(&context,scene);
    cJSON *entities=document ? cJSON_GetObjectItemCaseSensitive(document,"entities") : NULL;
    size_t index=0;
    for (cJSON *entity=entities ? entities->child : NULL,*next;entity;entity=next,++index) {
        next=entity->next;
        if (membership[index]!=2) cJSON_Delete(cJSON_DetachItemViaPointer(entities,entity));
        else if (scene->entities[index].id==root) cJSON_ReplaceItemInObjectCaseSensitive(entity,"parent",fl_json_u64(0));
    }
    free(membership);
    if (document) cJSON_ReplaceItemInObjectCaseSensitive(document,"clips",cJSON_CreateArray());
    Fl_Prototype prototype={.root=root,.scope={.duration=scene->duration}};
    bool ok=document && !context.error[0] && fl_document_content_read(&context,document,&prototype.scope);
    cJSON_Delete(document);
    Fl_Id max_id=0,max_component=0;
    if (ok) {
        size_t count=fl_timeline_id_count(&prototype.scope);
        Fl_Id *ids=count ? malloc(count*sizeof(*ids)) : NULL;
        if (count && !ids) ok=false;
        else {fl_timeline_ids(&prototype.scope,ids); for (size_t i=0;i<count;++i) if (ids[i]>max_id) max_id=ids[i];}
        free(ids);
        for (size_t i=0;i<prototype.scope.entity_count;++i) {
            Fl_Entity *entity=(Fl_Entity *)&prototype.scope.entities[i];
            if (entity->id>max_id) max_id=entity->id;
            for (size_t j=0;j<entity->component_count;++j) if (entity->components[j].id>max_component) max_component=entity->components[j].id;
            if (entity->id==root) {
                Fl_Component *component=(Fl_Component *)fl_entity_component(entity,FL_COMPONENT_POSE); Fl_Pose pose;
                if (!component || component->size!=FL_POSE_BYTES || !fl_pose_decode(component,&pose)) {ok=false; break;}
                pose.x=pose.y=pose.z=pose.rx=pose.ry=pose.rz=0; pose.sx=pose.sy=pose.sz=1;
                fl_pose_encode(&pose,(unsigned char *)component->data);
            }
        }
    }
    if (max_id>=INT64_MAX-1 || max_component>=INT64_MAX-1) ok=false;
    prototype.next_id=max_id+1; prototype.next_component_id=max_component+1;
    if (ok) ok=fl_prototype_local_valid(&context,&prototype);
    cJSON *encoded=ok ? fl_prototype_write(&context,&prototype) : NULL;
    char *text=encoded ? cJSON_PrintUnformatted(encoded) : NULL; cJSON_Delete(encoded);
    if (ok) ok=text && !context.error[0] && fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_PROTOTYPE,
        .name=name,.source=text,.source_size=text ? strlen(text) : 0},asset);
    free(text); fl_prototype_clear(&prototype); return ok;
}

bool fl_prototype_instantiate(Fl_Project *project, Fl_Id scene, Fl_Id parent, Fl_Id asset_id, double x, double y, Fl_Id *entity)
{
    if (entity) *entity=0;
    const Fl_Asset *asset=fl_project_asset(project,asset_id);
    if (!asset || asset->kind!=FL_ASSET_PROTOTYPE || !entity || !isfinite(x) || !isfinite(y)) return false;
    Fl_Json context={0}; Fl_Prototype prototype={0};
    if (!fl_prototype_read(&context,asset->source,(size_t)asset->size,&prototype)) return false;
    const Fl_Entity *root=fl_scene_entity(&prototype.scope,prototype.root); Fl_Bounds box={0};
    bool ok=root && fl_entity_component(root,FL_COMPONENT_POSE);
    if (ok) box=fl_entity_bounds(root);
    unsigned char bytes[FL_POSE_BYTES], bound_bytes[FL_BOUNDS_BYTES]; Fl_Component components[3]={0};
    if (ok) {
        Fl_Pose pose=fl_pose_identity(); pose.x=x; pose.y=y;
        fl_pose_encode(&pose,bytes); fl_bounds_encode(&box,bound_bytes);
        components[0]=(Fl_Component){.type=FL_COMPONENT_POSE,.version=1,.size=FL_POSE_BYTES,.data=bytes};
        components[1]=(Fl_Component){.type=FL_COMPONENT_BOUNDS,.version=1,.size=FL_BOUNDS_BYTES,.data=bound_bytes};
        ok=fl_prototype_instance_encode(&(Fl_PrototypeInstance){.asset=asset_id},&components[2]);
    }
    if (ok) ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.parent=parent,.name=asset->name,.components=components,.component_count=3},entity);
    fl_component_clear(&components[2]); fl_prototype_clear(&prototype); return ok;
}

bool fl_prototype_set_overrides(Fl_Project *project, Fl_Id scene_id, Fl_Id entity_id, const char *overrides, size_t size)
{
    const Fl_Scene *scene=fl_project_scene(project,scene_id); const Fl_Entity *entity=fl_scene_entity(scene,entity_id);
    const Fl_Component *component=fl_entity_component(entity,FL_COMPONENT_PROTOTYPE); Fl_PrototypeInstance instance;
    if (!fl_prototype_instance_decode(component,&instance)) return false;
    instance.overrides=overrides; instance.overrides_size=size;
    Fl_Component replacement={0}; if (!fl_prototype_instance_encode(&instance,&replacement)) return false;
    replacement.id=component->id;
    bool ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity_id,.component=replacement},NULL);
    fl_component_clear(&replacement); return ok;
}

bool fl_prototype_detach(Fl_Project *project, Fl_Id scene_id, Fl_Id entity_id)
{
    const Fl_Entity *entity=fl_scene_entity(fl_project_scene(project,scene_id),entity_id);
    const Fl_Component *component=fl_entity_component(entity,FL_COMPONENT_PROTOTYPE);
    Fl_PrototypeInstance instance; if (!fl_prototype_instance_decode(component,&instance)) return false;
    const Fl_Asset *asset=fl_project_asset(project,instance.asset);
    if (!asset || asset->kind!=FL_ASSET_PROTOTYPE || fl_entity_component(entity,FL_COMPONENT_COMPOSITION)) return false;
    Fl_Component owned={0};
    if (!fl_local_composition_encode(&(Fl_LocalComposition){.time=instance.time,.document=asset->source,.document_size=(size_t)asset->size,.overrides=instance.overrides,.overrides_size=instance.overrides_size},&owned)) return false;
    owned.id=component->id;
    bool ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene_id,.entity=entity_id,.component=owned},NULL);
    fl_component_clear(&owned); return ok;
}
