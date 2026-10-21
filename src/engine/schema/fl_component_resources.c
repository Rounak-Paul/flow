#include "fl_widgets.h"
#include "components/fl_script.h"
#include "components/fl_prototype_instance.h"
#include "doc/fl_document.h"
#include <string.h>

/* Decode an override's typed component and enumerate its complete resource closure. */
static bool visit_component(const Fl_Component *component, Fl_AssetVisitor visitor, void *data, size_t depth);
static bool visit_json(Fl_Json *c, const cJSON *value, Fl_Id entity, Fl_AssetVisitor visitor, void *data, size_t depth)
{
    Fl_Component component={0};
    bool ok=fl_document_component_read(c,value,entity,&component) && visit_component(&component,visitor,data,depth);
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

/* Enumerate references from semantic component fields, never arbitrary parameter/user JSON. */
static bool visit_component(const Fl_Component *component, Fl_AssetVisitor visitor, void *data, size_t depth)
{
    if (depth>32) return false;
    Fl_Id asset; Fl_AssetKind kind;
    if (fl_component_asset_reference(component,&asset,&kind) && !visitor(asset,kind,data)) return false;
    if (component->type==FL_COMPONENT_SCRIPT) {
        Fl_Script script; Fl_Json context={0};
        if (!fl_script_decode(component,&script)) return false;
        cJSON *bindings=fl_json_parse(&context,script.bindings,script.bindings_size);
        bool valid=bindings && visit_binding_assets(&context,bindings,visitor,data);
        cJSON_Delete(bindings); return valid;
    }
    if (component->type!=FL_COMPONENT_PROTOTYPE) return true;
    Fl_PrototypeInstance instance; if (!fl_prototype_instance_decode(component,&instance)) return false;
    Fl_Json c={0}; cJSON *operations=fl_json_parse(&c,instance.overrides,instance.overrides_size);
    bool ok=operations && fl_prototype_overrides_valid(&c,operations);
    for (const cJSON *op=operations ? operations->child : NULL;ok && op;op=op->next) {
        const char *name=cJSON_GetObjectItemCaseSensitive(op,"op")->valuestring;
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(op,"value"); uint64_t entity;
        ok=fl_json_uint(&c,cJSON_GetObjectItemCaseSensitive(op,"entity"),&entity);
        if (!ok) break;
        if (!strcmp(name,"component.patch")) {
            const cJSON *reference=cJSON_GetObjectItemCaseSensitive(value,"asset");
            if (reference) ok=fl_json_uint(&c,reference,&asset) && asset && visitor(asset,0,data);
            const cJSON *bindings=cJSON_GetObjectItemCaseSensitive(value,"bindings");
            if (ok && bindings) ok=visit_binding_assets(&c,bindings,visitor,data);
        } else if (!strcmp(name,"component.set")) ok=visit_json(&c,value,entity,visitor,data,depth+1);
        else if (!strcmp(name,"entity.add")) {
            const cJSON *components=fl_json_member(&c,value,"components",cJSON_Array);
            if (!components) {ok=false; break;}
            for (const cJSON *v=components->child;ok && v;v=v->next) ok=visit_json(&c,v,entity,visitor,data,depth+1);
        }
    }
    cJSON_Delete(operations); return ok;
}

bool fl_component_visit_assets(const Fl_Component *component, Fl_AssetVisitor visitor, void *data)
{
    return component && visitor && visit_component(component,visitor,data,0);
}
