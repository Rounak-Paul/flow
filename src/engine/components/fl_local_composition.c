#include "fl_local_composition.h"
#include "project/fl_prototype.h"
#include <stdlib.h>

bool fl_local_composition_decode(const Fl_Component *component, Fl_LocalComposition *out)
{
    if (!component || !out || component->type!=FL_COMPONENT_COMPOSITION || component->version!=FL_LOCAL_COMPOSITION_VERSION || !component->data) return false;
    Fl_Binary bytes={.data=(unsigned char *)component->data,.size=component->size}; Fl_LocalComposition scope={0};
    if (!fl_time_mapping_read(&scope.time,&bytes)) return false;
    uint64_t size=fl_binary_get_u64(&bytes);
    if (bytes.failed || !size || size>FL_LOCAL_COMPOSITION_LIMIT || size>bytes.size-bytes.cursor) return false;
    scope.document=(const char *)bytes.data+bytes.cursor; scope.document_size=(size_t)size; bytes.cursor+=(size_t)size;
    size=fl_binary_get_u64(&bytes);
    if (bytes.failed || size>FL_PROTOTYPE_OVERRIDE_LIMIT || size!=bytes.size-bytes.cursor) return false;
    scope.overrides=(const char *)bytes.data+bytes.cursor; scope.overrides_size=(size_t)size; *out=scope; return true;
}

bool fl_local_composition_valid(const Fl_Component *component)
{
    Fl_LocalComposition scope; if (!fl_local_composition_decode(component,&scope)) return false;
    Fl_Json context={0}; Fl_Prototype document={0};
    bool valid=fl_owned_document_read(&context,scope.document,scope.document_size,&document);
    fl_prototype_clear(&document);
    cJSON *overrides=fl_json_parse(&context,scope.overrides,scope.overrides_size);
    valid=valid && fl_prototype_overrides_valid(&context,overrides); cJSON_Delete(overrides); return valid;
}

bool fl_local_composition_encode(const Fl_LocalComposition *scope, Fl_Component *out)
{
    if (!scope || !out || !scope->document || !scope->document_size || scope->document_size>FL_LOCAL_COMPOSITION_LIMIT || scope->overrides_size>FL_PROTOTYPE_OVERRIDE_LIMIT) return false;
    Fl_TimeMapping time=scope->time; if (!time.rate) time=fl_time_mapping_default();
    if (!fl_time_mapping_valid(&time)) return false;
    Fl_Binary bytes={0}; fl_time_mapping_write(&time,&bytes);
    fl_binary_u64(&bytes,scope->document_size); fl_binary_write(&bytes,scope->document,scope->document_size);
    fl_binary_u64(&bytes,scope->overrides ? scope->overrides_size : 2); fl_binary_write(&bytes,scope->overrides ? scope->overrides : "[]",scope->overrides ? scope->overrides_size : 2);
    Fl_Component candidate={.type=FL_COMPONENT_COMPOSITION,.version=FL_LOCAL_COMPOSITION_VERSION,.data=bytes.data,.size=bytes.size};
    if (bytes.failed || !fl_local_composition_valid(&candidate)) {free(bytes.data); return false;}
    *out=candidate; return true;
}
