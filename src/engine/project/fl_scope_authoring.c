#include "fl_scope_authoring.h"
#include "fl_prototype.h"
#include "fl_timeline_internal.h"
#include "components/fl_local_composition.h"
#include "components/fl_script.h"
#include "doc/fl_document.h"
#include <stdlib.h>
#include <string.h>

/* Retained operations reserve extension identities even after their targets disappear. */
static bool extension_cursor(const cJSON *value, Fl_Id *next, size_t depth)
{
    if (depth>64) return false;
    if (value && value->string && (!strcmp(value->string,"id") || !strcmp(value->string,"component") || !strcmp(value->string,"entity"))) {
        Fl_Json context={0}; uint64_t id;
        if (fl_json_uint(&context,value,&id) && (id>>63) && id>=*next) {
            *next=id>=UINT64_MAX-1 ? UINT64_MAX : id+1;
        }
    }
    for (const cJSON *item=value ? value->child : NULL;item;item=item->next) if (!extension_cursor(item,next,depth+1)) return false;
    return true;
}

/* Consecutive patch edits commute by stable address; merge fields without crossing structural edits. */
static bool append_edit(Fl_Json *context, cJSON *operations, cJSON *edit)
{
    const cJSON *kind=cJSON_GetObjectItemCaseSensitive(edit,"op");
    if (cJSON_IsString(kind) && (!strcmp(kind->valuestring,"component.patch") || !strcmp(kind->valuestring,"entity.patch") || !strcmp(kind->valuestring,"timeline.patch"))) {
        for (cJSON *previous=operations->child ? operations->child->prev : NULL;previous;previous=previous==operations->child ? NULL : previous->prev) {
            const cJSON *old=cJSON_GetObjectItemCaseSensitive(previous,"op");
            if (!cJSON_IsString(old) || (strcmp(old->valuestring,"component.patch") && strcmp(old->valuestring,"entity.patch") && strcmp(old->valuestring,"timeline.patch"))) break;
            if (strcmp(old->valuestring,kind->valuestring) || !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(previous,"path"),cJSON_GetObjectItemCaseSensitive(edit,"path"),true) ||
                !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(previous,"entity"),cJSON_GetObjectItemCaseSensitive(edit,"entity"),true)) continue;
            const cJSON *left=cJSON_GetObjectItemCaseSensitive(previous,"component"), *right=cJSON_GetObjectItemCaseSensitive(edit,"component");
            if ((left || right) && (!left || !right || !cJSON_Compare(left,right,true))) continue;
            cJSON *target=cJSON_GetObjectItemCaseSensitive(previous,"value");
            const cJSON *value=cJSON_GetObjectItemCaseSensitive(edit,"value");
            if (!cJSON_IsObject(target) || !cJSON_IsObject(value)) break;
            for (const cJSON *field=value->child;field;field=field->next) {
                cJSON_DeleteItemFromObjectCaseSensitive(target,field->string);
                if (!fl_json_add(context,target,field->string,cJSON_Duplicate(field,true))) {cJSON_Delete(edit); return false;}
            }
            cJSON_Delete(edit); return true;
        }
    }
    return fl_json_add(context,operations,NULL,edit);
}

bool fl_scope_edit(Fl_Project *project, Fl_Id scene, const Fl_Origin *address, const cJSON *operation)
{
    if (!address || !address->generated || address->depth>32 || (!cJSON_IsObject(operation) && !cJSON_IsArray(operation))) return false;
    const Fl_Entity *owner=fl_scene_entity(fl_project_scene(project,scene),address->owner);
    const Fl_Component *generator=fl_entity_component_instance(owner,address->component);
    Fl_PrototypeInstance instance={0}; Fl_LocalComposition owned={0}; Fl_Script script={0};
    const char *bytes; size_t size;
    if (fl_prototype_instance_decode(generator,&instance)) {bytes=instance.overrides; size=instance.overrides_size;}
    else if (fl_local_composition_decode(generator,&owned)) {bytes=owned.overrides; size=owned.overrides_size;}
    else if (fl_script_decode(generator,&script)) {bytes=script.overrides; size=script.overrides_size;}
    else return false;
    Fl_Json context={0}; cJSON *operations=fl_json_parse(&context,bytes,size); Fl_Id next_id=UINT64_C(1)<<63;
    bool ok=operations && extension_cursor(operations,&next_id,0); bool array=cJSON_IsArray(operation);
    for (const cJSON *item=array ? operation->child : operation;item && ok;item=array ? item->next : NULL) {
        cJSON *next=cJSON_Duplicate(item,true), *path=cJSON_CreateArray();
        ok=next && path && fl_json_object(&context,next,"op entity component value");
        for (size_t i=0;i<address->depth && ok;++i) ok=fl_json_add(&context,path,NULL,fl_json_u64(address->path[i]));
        if (ok) {
            if (!cJSON_HasObjectItem(next,"entity")) ok=fl_json_add(&context,next,"entity",fl_json_u64(address->entity));
            if (ok) {ok=fl_json_add(&context,next,"path",path); path=NULL;}
            const cJSON *kind=cJSON_GetObjectItemCaseSensitive(next,"op");
            cJSON *value=cJSON_GetObjectItemCaseSensitive(next,"value"); uint64_t id=0;
            if (ok && cJSON_IsString(kind) && !strcmp(kind->valuestring,"component.set") && cJSON_IsObject(value) &&
                fl_json_uint(&context,cJSON_GetObjectItemCaseSensitive(value,"id"),&id) && !id) {
                if (next_id==UINT64_MAX) ok=false;
                else {cJSON_DeleteItemFromObjectCaseSensitive(value,"id"); ok=fl_json_add(&context,value,"id",fl_json_u64(next_id++));}
            }
            if (ok) ok=extension_cursor(next,&next_id,0);
            if (ok) {ok=append_edit(&context,operations,next); next=NULL;}
        }
        cJSON_Delete(next); cJSON_Delete(path);
    }
    char *text=ok ? cJSON_PrintUnformatted(operations) : NULL; Fl_Component replacement={0};
    if (text) {
        if (generator->type==FL_COMPONENT_PROTOTYPE) {instance.overrides=text; instance.overrides_size=strlen(text); ok=fl_prototype_instance_encode(&instance,&replacement);}
        else if (generator->type==FL_COMPONENT_COMPOSITION) {owned.overrides=text; owned.overrides_size=strlen(text); ok=fl_local_composition_encode(&owned,&replacement);}
        else {script.overrides=text; script.overrides_size=strlen(text); ok=fl_script_encode(&script,&replacement);}
        replacement.id=generator->id;
        if (ok) ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=address->owner,.component=replacement},NULL);
    } else ok=false;
    fl_component_clear(&replacement); cJSON_free(text); cJSON_Delete(operations); return ok;
}

bool fl_scope_configure(Fl_Project *project, Fl_Id scene_id, const Fl_Composition *composition, size_t index,
                         const char *name, bool visible, const Fl_Component *components, size_t count, Fl_Id remove_script, Fl_Tick tick)
{
    const Fl_Scene *scene=fl_project_scene(project,scene_id), *flat=fl_composition_scene(composition);
    const Fl_Origin *origin=fl_composition_origin(composition,index);
    if (!scene || !flat || !origin || !origin->generated || flat->revision!=scene->revision || count>64 || (!components && count)) return false;
    const Fl_Entity *entity=&flat->entities[index]; Fl_Origin address=*origin; Fl_Json context={0};
    const Fl_Scene *clock=fl_composition_entity_scope(composition,index);
    Fl_Component keyed[64]; if (count) memcpy(keyed,components,count*sizeof(*keyed));
    unsigned char transform[56],appearance[24],opacity[8],style[91]; Fl_KeyWrite writes[FL_PROP_COUNT];
    size_t write_count=fl_keyed_components(clock,entity,keyed,count,transform,appearance,opacity,style,writes,tick);
    if (write_count==SIZE_MAX) return false;
    components=keyed;
    cJSON *operations=cJSON_CreateArray(), *edit=cJSON_CreateObject(), *value=cJSON_CreateObject();
    bool ok=operations && edit && value;
    if (ok) {
        ok=fl_json_add(&context,edit,"op",cJSON_CreateString("entity.patch"));
        if (ok && name) ok=fl_json_add(&context,value,"name",cJSON_CreateString(name));
        if (ok) ok=fl_json_add(&context,value,"visible",cJSON_CreateBool(visible));
        if (ok) {ok=fl_json_add(&context,edit,"value",value); value=NULL;}
        if (ok) {ok=fl_json_add(&context,operations,NULL,edit); edit=NULL;}
    }
    for (size_t i=0;i<count && ok;++i) {
        const Fl_Component *component=&components[i];
        const Fl_Component *previous=component->id ? fl_entity_component_instance(entity,component->id) : component->type==FL_COMPONENT_SCRIPT ? NULL : fl_entity_component(entity,component->type);
        Fl_Id local=0;
        if (previous && !fl_composition_component_local(composition,index,previous->id,&local)) {ok=false; break;}
        edit=cJSON_CreateObject(); value=fl_document_component_write(&context,component);
        ok=edit && value && !context.error[0]; if (!ok) break;
        cJSON_DeleteItemFromObjectCaseSensitive(value,"id");
        if (previous) {
            cJSON_DeleteItemFromObjectCaseSensitive(value,"type"); cJSON_DeleteItemFromObjectCaseSensitive(value,"version");
            Fl_Script old_script,new_script;
            if (fl_script_decode(previous,&old_script) && fl_script_decode(component,&new_script) && old_script.script==new_script.script && old_script.bindings_size==new_script.bindings_size && !memcmp(old_script.bindings,new_script.bindings,old_script.bindings_size))
                cJSON_DeleteItemFromObjectCaseSensitive(value,"bindings");
            ok=fl_json_add(&context,edit,"op",cJSON_CreateString("component.patch")) && fl_json_add(&context,edit,"component",fl_json_u64(local));
        } else ok=fl_json_add(&context,value,"id",fl_json_u64(0)) && fl_json_add(&context,edit,"op",cJSON_CreateString("component.set"));
        if (ok) {ok=fl_json_add(&context,edit,"value",value); value=NULL;}
        if (ok) {ok=fl_json_add(&context,operations,NULL,edit); edit=NULL;}
    }
    if (ok && remove_script) {
        Fl_Id local;
        ok=fl_composition_component_local(composition,index,remove_script,&local);
        if (ok) {
            edit=cJSON_CreateObject(); ok=edit && fl_json_add(&context,edit,"op",cJSON_CreateString("component.remove")) && fl_json_add(&context,edit,"component",fl_json_u64(local));
            if (ok) {ok=fl_json_add(&context,operations,NULL,edit); edit=NULL;}
        }
    }
    if (ok && write_count) {
        cJSON *document=fl_composition_entity_document(composition,index);
        char *source=document ? cJSON_PrintUnformatted(document) : NULL; Fl_Prototype local={0};
        Fl_Id next=UINT64_C(1)<<63;
        const Fl_Entity *owner=fl_scene_entity(scene,address.owner);
        const Fl_Component *generator=fl_entity_component_instance(owner,address.component);
        cJSON *configuration=generator ? fl_document_component_write(&context,generator) : NULL;
        ok=source && fl_owned_document_read(&context,source,strlen(source),&local) && extension_cursor(document,&next,0) && configuration && extension_cursor(configuration,&next,0);
        local.scope.project=project;
        for (size_t w=0;w<write_count && ok;++w) {
            Fl_Id created=0;
            Fl_Op op={.kind=FL_OP_KEY_SET,.entity=address.entity,.property=writes[w].property,.anchor=fl_anchor_at(&local.scope,tick),.ease=FL_EASE_IN_OUT};
            memcpy(op.value,writes[w].value,sizeof(op.value));
            ok=fl_timeline_apply(project,&local.scope,&op,&next,&created);
        }
        const Fl_Entity *target=fl_scene_entity(&local.scope,address.entity);
        const Fl_Component *animation=fl_entity_component(target,FL_COMPONENT_ANIMATION);
        if (ok && animation) {
            edit=cJSON_CreateObject(); value=fl_document_component_write(&context,animation);
            ok=edit && value && fl_json_add(&context,edit,"op",cJSON_CreateString(animation->id ? "component.patch" : "component.set"));
            if (ok && animation->id) {
                cJSON_DeleteItemFromObjectCaseSensitive(value,"id"); cJSON_DeleteItemFromObjectCaseSensitive(value,"type"); cJSON_DeleteItemFromObjectCaseSensitive(value,"version");
                ok=fl_json_add(&context,edit,"component",fl_json_u64(animation->id));
            }
            if (ok) {ok=fl_json_add(&context,edit,"value",value); value=NULL;}
            if (ok) {ok=fl_json_add(&context,operations,NULL,edit); edit=NULL;}
        } else ok=false;
        cJSON_Delete(configuration); cJSON_Delete(document); free(source); fl_prototype_clear(&local);
    }
    if (ok) ok=fl_scope_edit(project,scene_id,&address,operations);
    cJSON_Delete(edit); cJSON_Delete(value); cJSON_Delete(operations); return ok;
}

/* Emit a scope-addressed component replacement while preserving retained extension allocation. */
static bool timeline_component(Fl_Json *c, cJSON *operations, Fl_Id entity, const Fl_Component *component, const Fl_Component *previous)
{
    cJSON *edit=cJSON_CreateObject(); bool ok=edit && fl_json_add(c,edit,"entity",fl_json_u64(entity));
    bool patch=component && component->id;
    if (ok) ok=fl_json_add(c,edit,"op",cJSON_CreateString(component ? patch ? "component.patch" : "component.set" : "component.remove"));
    if (ok && component) {
        cJSON *value=fl_document_component_write(c,component);
        if (patch && value) {
            cJSON_DeleteItemFromObjectCaseSensitive(value,"id"); cJSON_DeleteItemFromObjectCaseSensitive(value,"type"); cJSON_DeleteItemFromObjectCaseSensitive(value,"version");
            ok=fl_json_add(c,edit,"component",fl_json_u64(component->id));
        }
        if (ok) ok=fl_json_add(c,edit,"value",value); else cJSON_Delete(value);
    }
    if (ok && !component) ok=fl_json_add(c,edit,"component",fl_json_u64(previous->id));
    if (ok) return fl_json_add(c,operations,NULL,edit);
    cJSON_Delete(edit); return false;
}

bool fl_scope_timeline(Fl_Project *project, Fl_Id scene_id, const Fl_Composition *composition, size_t index,
                         const Fl_Op *operation, Fl_Id *created)
{
    const Fl_Scene *scene=fl_project_scene(project,scene_id), *flat=fl_composition_scene(composition);
    const Fl_Origin *origin=fl_composition_origin(composition,index);
    if (!scene || !flat || !origin || !origin->generated || flat->revision!=scene->revision || !operation) return false;
    Fl_Origin address=*origin; Fl_Json c={0}; cJSON *document=fl_composition_entity_document(composition,index);
    char *source=document ? cJSON_PrintUnformatted(document) : NULL; Fl_Prototype local={0};
    bool ok=source && fl_owned_document_read(&c,source,strlen(source),&local); free(source);
    const Fl_Entity *owner=fl_scene_entity(scene,address.owner);
    const Fl_Component *generator=fl_entity_component_instance(owner,address.component);
    cJSON *configuration=generator ? fl_document_component_write(&c,generator) : NULL;
    Fl_Id next=UINT64_C(1)<<63,id=0;
    ok=ok && configuration && extension_cursor(configuration,&next,0) && extension_cursor(document,&next,0);
    cJSON_Delete(configuration);
    Fl_Op op=*operation; op.scene=local.scope.id;
    if (op.entity) {
        const Fl_Entity *target=fl_scene_entity(flat,op.entity);
        const Fl_Origin *target_origin=target ? fl_composition_origin(composition,(size_t)(target-flat->entities)) : NULL;
        if (!target_origin || target_origin->owner!=address.owner || target_origin->component!=address.component || target_origin->depth!=address.depth || memcmp(target_origin->path,address.path,address.depth*sizeof(Fl_Id))) ok=false;
        else op.entity=target_origin->entity;
    }
    local.scope.project=project;
    if (ok) ok=fl_timeline_apply(project,&local.scope,&op,&next,&id);
    cJSON *operations=cJSON_CreateArray();
    for (size_t i=0;i<local.scope.entity_count && ok;++i) {
        const Fl_Entity *entity=&local.scope.entities[i];
        const cJSON *old_entities=cJSON_GetObjectItemCaseSensitive(document,"entities"), *old=NULL;
        for (const cJSON *e=old_entities ? old_entities->child : NULL;e;e=e->next) {
            uint64_t value; if (fl_json_uint(&c,cJSON_GetObjectItemCaseSensitive(e,"id"),&value) && value==entity->id) {old=e; break;}
        }
        const cJSON *old_components=cJSON_GetObjectItemCaseSensitive(old,"components");
        for (size_t j=0;j<entity->component_count && ok;++j) {
            const Fl_Component *component=&entity->components[j]; cJSON *encoded=fl_document_component_write(&c,component); const cJSON *previous=NULL;
            for (const cJSON *item=old_components ? old_components->child : NULL;item;item=item->next) {
                uint64_t value; if (fl_json_uint(&c,cJSON_GetObjectItemCaseSensitive(item,"id"),&value) && value==component->id) {previous=item; break;}
            }
            if (!previous || !cJSON_Compare(encoded,previous,true)) ok=timeline_component(&c,operations,entity->id,component,NULL);
            cJSON_Delete(encoded);
        }
        for (const cJSON *item=old_components ? old_components->child : NULL;item && ok;item=item->next) {
            uint64_t value;
            if (!fl_json_uint(&c,cJSON_GetObjectItemCaseSensitive(item,"id"),&value)) {ok=false; break;}
            if (!fl_entity_component_instance(entity,value)) ok=timeline_component(&c,operations,entity->id,NULL,&(Fl_Component){.id=value});
        }
    }
    cJSON *updated=ok ? fl_prototype_write(&c,&local) : NULL, *patch=cJSON_CreateObject();
    const char *fields[]={"duration","beats","clips"}; bool changed=false;
    for (unsigned i=0;i<3 && ok;++i) {
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(updated,fields[i]);
        if (!cJSON_Compare(value,cJSON_GetObjectItemCaseSensitive(document,fields[i]),true)) {changed=true; ok=fl_json_add(&c,patch,fields[i],cJSON_Duplicate(value,true));}
    }
    if (ok && changed) {
        cJSON *edit=cJSON_CreateObject(); ok=edit && fl_json_add(&c,edit,"op",cJSON_CreateString("timeline.patch"));
        if (ok) {ok=fl_json_add(&c,edit,"value",patch); patch=NULL;}
        if (ok) ok=fl_json_add(&c,operations,NULL,edit); else cJSON_Delete(edit);
    }
    if (ok) ok=fl_scope_edit(project,scene_id,&address,operations);
    if (ok && created) *created=id;
    cJSON_Delete(patch); cJSON_Delete(updated); cJSON_Delete(document); cJSON_Delete(operations); fl_prototype_clear(&local); return ok;
}
