#include "fl_widgets.h"
#include "components/fl_script.h"
#include "components/fl_local_composition.h"
#include "project/fl_prototype.h"
#include "doc/fl_document.h"
#include <string.h>

static bool visit_component(const Fl_Component *component, Fl_AssetVisitor visitor, void *data, size_t depth);
static bool visit_overrides(Fl_Json *context, const cJSON *operations, Fl_AssetVisitor visitor, void *data, size_t depth);

/* Decode an override's typed component and enumerate its complete resource closure. */
static bool visit_json(Fl_Json *context, const cJSON *value, Fl_Id entity, Fl_AssetVisitor visitor, void *data, size_t depth)
{
    Fl_Component component={0};
    bool ok=fl_document_component_read(context,value,entity,&component) && visit_component(&component,visitor,data,depth);
    fl_component_clear(&component); return ok;
}

/* Retain explicit asset inputs even in unresolved prototype patch overrides. */
static bool visit_binding_assets(Fl_Json *context, const cJSON *bindings, Fl_AssetVisitor visitor, void *data)
{
    if (!fl_script_bindings_valid(context,bindings)) return false;
    for (const cJSON *binding=bindings->child;binding;binding=binding->next)
        if (!strcmp(cJSON_GetObjectItemCaseSensitive(binding,"type")->valuestring,"asset")) {
            uint64_t asset;
            if (!fl_json_uint(context,cJSON_GetObjectItemCaseSensitive(binding,"id"),&asset) || !visitor(asset,0,data)) return false;
        }
    return true;
}

/* Owned native documents carry resource references through ordinary components and audio clips. */
static bool visit_document(Fl_Json *context, const char *text, size_t size, Fl_AssetVisitor visitor, void *data, size_t depth)
{
    if (depth>32) return false;
    Fl_Prototype document={0}; bool valid=fl_owned_document_read(context,text,size,&document);
    for (size_t i=0;i<document.scope.entity_count && valid;++i)
        for (size_t j=0;j<document.scope.entities[i].component_count && valid;++j)
            valid=visit_component(&document.scope.entities[i].components[j],visitor,data,depth+1);
    for (size_t i=0;i<document.scope.clip_count && valid;++i) valid=visitor(document.scope.clips[i].asset,FL_ASSET_AUDIO,data);
    fl_prototype_clear(&document); return valid;
}

/* Retained edits own their semantic resource references even while their targets are missing. */
static bool visit_overrides(Fl_Json *context, const cJSON *operations, Fl_AssetVisitor visitor, void *data, size_t depth)
{
    if (depth>32 || !fl_prototype_overrides_valid(context,operations)) return false;
    for (const cJSON *op=operations->child;op;op=op->next) {
        const char *name=cJSON_GetObjectItemCaseSensitive(op,"op")->valuestring;
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(op,"value"); uint64_t entity;
        if (!fl_json_uint(context,cJSON_GetObjectItemCaseSensitive(op,"entity"),&entity)) return false;
        if (!strcmp(name,"timeline.patch")) {
            const cJSON *clips=cJSON_GetObjectItemCaseSensitive(value,"clips");
            for (const cJSON *clip=clips ? clips->child : NULL;clip;clip=clip->next) {
                uint64_t asset; if (!fl_json_uint(context,cJSON_GetObjectItemCaseSensitive(clip,"asset"),&asset) || !visitor(asset,FL_ASSET_AUDIO,data)) return false;
            }
        } else if (!strcmp(name,"component.patch")) {
            const cJSON *reference=cJSON_GetObjectItemCaseSensitive(value,"asset"), *bindings=cJSON_GetObjectItemCaseSensitive(value,"bindings"),
                *nested=cJSON_GetObjectItemCaseSensitive(value,"overrides"), *document=cJSON_GetObjectItemCaseSensitive(value,"document");
            uint64_t asset;
            if (reference && (!fl_json_uint(context,reference,&asset) || !asset || !visitor(asset,0,data))) return false;
            if (bindings && !visit_binding_assets(context,bindings,visitor,data)) return false;
            if (nested && !visit_overrides(context,nested,visitor,data,depth+1)) return false;
            if (document) {
                char *text=cJSON_PrintUnformatted(document);
                bool valid=text && visit_document(context,text,strlen(text),visitor,data,depth+1); cJSON_free(text);
                if (!valid) return false;
            }
        } else if (!strcmp(name,"component.set")) {
            if (!visit_json(context,value,entity,visitor,data,depth+1)) return false;
        } else if (!strcmp(name,"entity.add")) {
            const cJSON *components=fl_json_member(context,value,"components",cJSON_Array);
            if (!components) return false;
            for (const cJSON *component=components->child;component;component=component->next)
                if (!visit_json(context,component,entity,visitor,data,depth+1)) return false;
        }
    }
    return true;
}

/* Enumerate semantic component fields, never arbitrary parameter/user JSON. */
static bool visit_component(const Fl_Component *component, Fl_AssetVisitor visitor, void *data, size_t depth)
{
    if (depth>32) return false;
    Fl_Id asset; Fl_AssetKind kind;
    if (fl_component_asset_reference(component,&asset,&kind) && !visitor(asset,kind,data)) return false;
    const char *bytes=NULL; size_t size=0; Fl_Json context={0};
    if (component->type==FL_COMPONENT_SCRIPT) {
        Fl_Script script; if (!fl_script_decode(component,&script)) return false;
        cJSON *bindings=fl_json_parse(&context,script.bindings,script.bindings_size);
        bool valid=bindings && visit_binding_assets(&context,bindings,visitor,data); cJSON_Delete(bindings);
        if (!valid) return false;
        bytes=script.overrides; size=script.overrides_size;
    } else if (component->type==FL_COMPONENT_COMPOSITION) {
        Fl_LocalComposition scope;
        if (!fl_local_composition_decode(component,&scope) || !visit_document(&context,scope.document,scope.document_size,visitor,data,depth)) return false;
        bytes=scope.overrides; size=scope.overrides_size;
    } else if (component->type==FL_COMPONENT_PROTOTYPE) {
        Fl_PrototypeInstance instance; if (!fl_prototype_instance_decode(component,&instance)) return false;
        bytes=instance.overrides; size=instance.overrides_size;
    } else return true;
    cJSON *operations=fl_json_parse(&context,bytes,size);
    bool valid=operations && visit_overrides(&context,operations,visitor,data,depth); cJSON_Delete(operations); return valid;
}

bool fl_component_visit_assets(const Fl_Component *component, Fl_AssetVisitor visitor, void *data)
{
    return component && visitor && visit_component(component,visitor,data,0);
}
