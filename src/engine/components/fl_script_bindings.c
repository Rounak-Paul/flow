#include "fl_script_bindings.h"
#include <string.h>

bool fl_script_bindings_valid(Fl_Json *context, const cJSON *bindings)
{
    if (!cJSON_IsObject(bindings)) return fl_json_fail(context,"Bindings must be an object");
    size_t count=0;
    for (const cJSON *binding=bindings->child;binding;binding=binding->next) {
        if (++count>FL_SCRIPT_BINDING_COUNT || !binding->string || !binding->string[0] || strlen(binding->string)>63 ||
            !fl_json_object(context,binding,"type id entity")) return fl_json_fail(context,"Invalid binding name or field");
        const cJSON *type=fl_json_member(context,binding,"type",cJSON_String);
        uint64_t id,entity;
        if (!type || !fl_json_uint(context,fl_json_member(context,binding,"id",0),&id) || !id)
            return fl_json_fail(context,"Binding requires an exact nonzero ID");
        bool component=!strcmp(type->valuestring,"component");
        if (!component && strcmp(type->valuestring,"entity") && strcmp(type->valuestring,"region") && strcmp(type->valuestring,"asset"))
            return fl_json_fail(context,"Unknown binding type");
        const cJSON *owner=cJSON_GetObjectItemCaseSensitive(binding,"entity");
        if (component ? !fl_json_uint(context,owner,&entity) || !entity : owner!=NULL)
            return fl_json_fail(context,"Only component bindings require an entity");
    }
    return true;
}
