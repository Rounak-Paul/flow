#include "fl_document.h"
#include "time/fl_time_mapping.h"
#include <string.h>

cJSON *fl_document_time_write(Fl_Json *c, const Fl_TimeMapping *m)
{
    cJSON *o=cJSON_CreateObject();
    fl_json_add(c,o,"start",m->explicit_range ? fl_document_anchor_write(c,&m->start) : cJSON_CreateNull());
    fl_json_add(c,o,"end",m->explicit_range ? fl_document_anchor_write(c,&m->end) : cJSON_CreateNull());
    fl_json_add(c,o,"duration",fl_json_i64(m->duration));
    fl_json_add(c,o,"mode",cJSON_CreateString(m->mode==FL_RETIME_STRETCH ? "stretch" : "fixed"));
    fl_json_add(c,o,"rate",cJSON_CreateNumber(m->rate)); fl_json_add(c,o,"loop",cJSON_CreateBool(m->loop));
    fl_json_add(c,o,"before",cJSON_CreateString(m->before==FL_TIME_HOLD ? "hold" : "inactive"));
    fl_json_add(c,o,"after",cJSON_CreateString(m->after==FL_TIME_HOLD ? "hold" : "inactive"));
    return o;
}

bool fl_document_time_read(Fl_Json *c, const cJSON *o, Fl_TimeMapping *m)
{
    *m=fl_time_mapping_default();
    if (!fl_json_object(c,o,"start end duration mode rate loop before after")) return false;
    const cJSON *start=fl_json_member(c,o,"start",0), *end=fl_json_member(c,o,"end",0), *mode=fl_json_member(c,o,"mode",cJSON_String),
        *loop=fl_json_member(c,o,"loop",0), *before=fl_json_member(c,o,"before",cJSON_String), *after=fl_json_member(c,o,"after",cJSON_String);
    if (!start || !end || !mode || !cJSON_IsBool(loop) || !before || !after) return fl_json_fail(c,"Invalid local clock");
    if (cJSON_IsNull(start)!=cJSON_IsNull(end)) return fl_json_fail(c,"Clock endpoints must both bind or both use the scene");
    m->explicit_range=!cJSON_IsNull(start); m->loop=cJSON_IsTrue(loop);
    if (m->explicit_range && (!fl_document_anchor_read(c,start,&m->start) || !fl_document_anchor_read(c,end,&m->end))) return false;
    if (strcmp(mode->valuestring,"stretch") && strcmp(mode->valuestring,"fixed")) return fl_json_fail(c,"Unknown retiming mode");
    if ((strcmp(before->valuestring,"inactive") && strcmp(before->valuestring,"hold")) || (strcmp(after->valuestring,"inactive") && strcmp(after->valuestring,"hold"))) return fl_json_fail(c,"Unknown clock boundary policy");
    m->mode=!strcmp(mode->valuestring,"fixed") ? FL_RETIME_FIXED : FL_RETIME_STRETCH;
    m->before=!strcmp(before->valuestring,"hold") ? FL_TIME_HOLD : FL_TIME_INACTIVE;
    m->after=!strcmp(after->valuestring,"hold") ? FL_TIME_HOLD : FL_TIME_INACTIVE;
    return fl_json_int(c,fl_json_member(c,o,"duration",0),&m->duration) && fl_json_number(c,fl_json_member(c,o,"rate",0),&m->rate) &&
        (fl_time_mapping_valid(m) || fl_json_fail(c,"Invalid clock mapping"));
}
