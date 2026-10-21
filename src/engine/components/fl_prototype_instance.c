#include "fl_prototype_instance.h"
#include <stdlib.h>
#include <string.h>

bool fl_prototype_overrides_valid(Fl_Json *c, const cJSON *operations)
{
    if (!cJSON_IsArray(operations) || cJSON_GetArraySize(operations)>4096) return fl_json_fail(c,"Invalid prototype override array");
    for (const cJSON *op=operations->child;op;op=op->next) {
        if (!fl_json_object(c,op,"op path entity component value")) return false;
        const cJSON *kind=fl_json_member(c,op,"op",cJSON_String), *path=fl_json_member(c,op,"path",cJSON_Array);
        uint64_t entity=0;
        if (!kind || !path || cJSON_GetArraySize(path)>32 || !fl_json_uint(c,fl_json_member(c,op,"entity",0),&entity) || !entity) return fl_json_fail(c,"Invalid prototype override address");
        for (const cJSON *v=path->child;v;v=v->next) {uint64_t id; if (!fl_json_uint(c,v,&id) || !id) return fl_json_fail(c,"Invalid nested prototype path");}
        bool component=!strcmp(kind->valuestring,"component.patch") || !strcmp(kind->valuestring,"component.remove");
        bool value=!strcmp(kind->valuestring,"timeline.patch") || !strcmp(kind->valuestring,"entity.patch") || !strcmp(kind->valuestring,"entity.add") || !strcmp(kind->valuestring,"component.patch") || !strcmp(kind->valuestring,"component.set");
        if (!component && !value && strcmp(kind->valuestring,"entity.remove")) return fl_json_fail(c,"Unknown prototype override operation");
        const cJSON *target=cJSON_GetObjectItemCaseSensitive(op,"component"), *body=cJSON_GetObjectItemCaseSensitive(op,"value");
        uint64_t id;
        if (component ? (!fl_json_uint(c,target,&id) || !id) : target!=NULL) return fl_json_fail(c,"Invalid component override address");
        if (value ? !cJSON_IsObject(body) : body!=NULL) return fl_json_fail(c,"Invalid prototype override value");
        if (!strcmp(kind->valuestring,"timeline.patch") && !fl_json_object(c,body,"duration beats clips")) return false;
        if (!strcmp(kind->valuestring,"entity.patch") && !fl_json_object(c,body,"name visible parent")) return false;
    }
    return true;
}

bool fl_prototype_instance_encode(const Fl_PrototypeInstance *instance, Fl_Component *out)
{
    if (!instance || !out || !instance->asset || instance->overrides_size>FL_PROTOTYPE_OVERRIDE_LIMIT) return false;
    const char *text=instance->overrides ? instance->overrides : "[]";
    size_t size=instance->overrides ? instance->overrides_size : 2;
    Fl_Json context={0}; cJSON *operations=fl_json_parse(&context,text,size);
    bool valid=operations && fl_prototype_overrides_valid(&context,operations); cJSON_Delete(operations);
    if (!valid) return false;
    Fl_TimeMapping time=instance->time;
    if (time.rate==0) time=fl_time_mapping_default();
    if (!fl_time_mapping_valid(&time)) return false;
    Fl_Binary bytes={0}; fl_binary_u64(&bytes,instance->asset); fl_time_mapping_write(&time,&bytes);
    fl_binary_u64(&bytes,size); fl_binary_write(&bytes,text,size);
    if (bytes.failed) {free(bytes.data); return false;}
    *out=(Fl_Component){.type=FL_COMPONENT_PROTOTYPE,.version=FL_PROTOTYPE_INSTANCE_VERSION,.data=bytes.data,.size=bytes.size}; return true;
}

bool fl_prototype_instance_decode(const Fl_Component *component, Fl_PrototypeInstance *out)
{
    if (!component || !out || component->type!=FL_COMPONENT_PROTOTYPE || component->version!=FL_PROTOTYPE_INSTANCE_VERSION || !component->data) return false;
    Fl_Binary bytes={.data=(unsigned char *)component->data,.size=component->size}; Fl_PrototypeInstance value={0};
    value.asset=fl_binary_get_u64(&bytes);
    if (!value.asset || !fl_time_mapping_read(&value.time,&bytes)) return false;
    uint64_t size=fl_binary_get_u64(&bytes);
    if (bytes.failed || size>FL_PROTOTYPE_OVERRIDE_LIMIT || size!=bytes.size-bytes.cursor) return false;
    value.overrides=(const char *)bytes.data+bytes.cursor; value.overrides_size=(size_t)size; *out=value; return true;
}

bool fl_prototype_instance_valid(const Fl_Component *component)
{
    Fl_PrototypeInstance value; if (!fl_prototype_instance_decode(component,&value)) return false;
    Fl_Json context={0}; cJSON *operations=fl_json_parse(&context,value.overrides,value.overrides_size);
    bool valid=operations && fl_prototype_overrides_valid(&context,operations); cJSON_Delete(operations); return valid;
}
