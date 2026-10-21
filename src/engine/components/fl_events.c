#include "fl_events.h"
#include <stdlib.h>
#include <string.h>

bool fl_events_valid(const cJSON *events)
{
    Fl_Json context={0}; uint64_t next;
    const cJSON *points=cJSON_GetObjectItemCaseSensitive(events,"points");
    if (!fl_json_object(&context,events,"next_id points") || !fl_json_uint(&context,cJSON_GetObjectItemCaseSensitive(events,"next_id"),&next) || !next || !cJSON_IsArray(points) || cJSON_GetArraySize(points)>(int)FL_EVENTS_COUNT) return false;
    Fl_Id ids[FL_EVENTS_COUNT]; size_t count=0;
    for (const cJSON *event=points->child;event;event=event->next) {
        const cJSON *name=cJSON_GetObjectItemCaseSensitive(event,"name"); Fl_TimeAnchor at;
        if (!fl_json_object(&context,event,"id at name value") || !fl_json_uint(&context,cJSON_GetObjectItemCaseSensitive(event,"id"),&ids[count]) || !ids[count] || ids[count]>=next ||
            !fl_document_anchor_read(&context,cJSON_GetObjectItemCaseSensitive(event,"at"),&at) || !cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring)>63 || !cJSON_HasObjectItem(event,"value")) return false;
        for (size_t i=0;i<count;++i) if (ids[i]==ids[count]) return false;
        ++count;
    }
    return true;
}

bool fl_events_encode(const cJSON *events, Fl_Component *out)
{
    if (!out || !fl_events_valid(events)) return false;
    char *text=cJSON_PrintUnformatted(events); size_t size=text ? strlen(text) : 0;
    Fl_Json context={0}; cJSON *check=text && size<=FL_EVENTS_LIMIT ? fl_json_parse(&context,text,size) : NULL;
    bool valid=check!=NULL; cJSON_Delete(check);
    if (!valid) {free(text); return false;}
    *out=(Fl_Component){.type=FL_COMPONENT_EVENTS,.version=1,.data=(unsigned char *)text,.size=size}; return true;
}

cJSON *fl_events_read(const Fl_Component *component)
{
    if (!component || component->type!=FL_COMPONENT_EVENTS || component->version!=1 || !component->data || component->size>FL_EVENTS_LIMIT) return NULL;
    Fl_Json context={0}; cJSON *events=fl_json_parse(&context,(const char *)component->data,component->size);
    if (!fl_events_valid(events)) {cJSON_Delete(events); return NULL;} return events;
}
