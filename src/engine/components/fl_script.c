#include "fl_script.h"
#include "doc/fl_json.h"
#include "project/fl_project.h"
#include <stdlib.h>
#include <string.h>

bool fl_script_encode(const Fl_Script *script, Fl_Component *out)
{
    if (!script || !out || !script->script || script->parameters_size>FL_SCRIPT_PARAMETER_LIMIT || script->bindings_size>FL_SCRIPT_BINDINGS_LIMIT) return false;
    const char *parameters=script->parameters ? script->parameters : "{}";
    size_t size=script->parameters ? script->parameters_size : 2;
    Fl_Json context={0}; cJSON *object=fl_json_parse(&context,parameters,size);
    bool valid=cJSON_IsObject(object); cJSON_Delete(object);
    if (!valid) return false;
    const char *bindings=script->bindings ? script->bindings : "{}";
    size_t binding_size=script->bindings ? script->bindings_size : 2;
    object=fl_json_parse(&context,bindings,binding_size);
    valid=fl_script_bindings_valid(&context,object); cJSON_Delete(object);
    if (!valid) return false;
    Fl_TimeMapping time=script->time;
    if (time.rate==0) {
        time.rate=1;
        if (!time.explicit_range && !time.duration && !time.loop && !time.before && !time.after) time.after=FL_TIME_HOLD;
    }
    if (!fl_time_mapping_valid(&time)) return false;
    Fl_Binary bytes={0}; fl_binary_u64(&bytes,script->script); fl_binary_u64(&bytes,script->enabled);
    fl_time_mapping_write(&time,&bytes); fl_binary_u64(&bytes,size); fl_binary_write(&bytes,parameters,size);
    fl_binary_u64(&bytes,binding_size); fl_binary_write(&bytes,bindings,binding_size);
    if (bytes.failed) {free(bytes.data); return false;}
    *out=(Fl_Component){.type=FL_COMPONENT_SCRIPT,.version=FL_SCRIPT_VERSION,.size=bytes.size,.data=bytes.data};
    return true;
}

bool fl_script_decode(const Fl_Component *component, Fl_Script *out)
{
    if (!component || !out || component->type!=FL_COMPONENT_SCRIPT || component->version!=FL_SCRIPT_VERSION || !component->data) return false;
    Fl_Binary bytes={.data=(unsigned char *)component->data,.size=component->size};
    Fl_Script script={0}; script.script=fl_binary_get_u64(&bytes); uint64_t enabled=fl_binary_get_u64(&bytes);
    if (!script.script || enabled>1 || !fl_time_mapping_read(&script.time,&bytes)) return false;
    script.enabled=enabled!=0; uint64_t size=fl_binary_get_u64(&bytes);
    if (bytes.failed || size>FL_SCRIPT_PARAMETER_LIMIT || size>bytes.size-bytes.cursor) return false;
    script.parameters=(const char *)bytes.data+bytes.cursor; script.parameters_size=(size_t)size; bytes.cursor+=(size_t)size;
    size=fl_binary_get_u64(&bytes);
    if (bytes.failed || size>FL_SCRIPT_BINDINGS_LIMIT || size!=bytes.size-bytes.cursor) return false;
    script.bindings=(const char *)bytes.data+bytes.cursor; script.bindings_size=(size_t)size;
    *out=script; return true;
}

bool fl_script_component_valid(const Fl_Component *component)
{
    Fl_Script script;
    if (!fl_script_decode(component,&script)) return false;
    Fl_Json context={0}; cJSON *parameters=fl_json_parse(&context,script.parameters,script.parameters_size);
    bool valid=cJSON_IsObject(parameters); cJSON_Delete(parameters);
    cJSON *bindings=fl_json_parse(&context,script.bindings,script.bindings_size);
    valid=valid && fl_script_bindings_valid(&context,bindings); cJSON_Delete(bindings); return valid;
}

bool fl_script_source_valid(const char *source, size_t size)
{
    return source && size && size <= FL_SCRIPT_SOURCE_LIMIT && fl_json_utf8_valid(source,size);
}

const Fl_Component *fl_entity_script_component(const Fl_Entity *entity, Fl_Id asset)
{
    Fl_Script script;
    for (size_t i=0;entity && asset && i<entity->component_count;++i)
        if (fl_script_decode(&entity->components[i],&script) && script.script==asset) return &entity->components[i];
    return NULL;
}
