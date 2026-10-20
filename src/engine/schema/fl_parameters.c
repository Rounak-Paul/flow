#include "fl_parameters.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Retain an actionable field-specific diagnostic without replacing an earlier allocation failure. */
static bool invalid(Fl_Json *c, const char *name, const char *message)
{
    if (c && !c->error[0]) snprintf(c->error,sizeof(c->error),"Parameter %s: %s",name,message);
    return false;
}

/* Validate a supported type and its optional numeric bounds. */
static bool value_valid(Fl_Json *c, const char *name, const cJSON *spec, const cJSON *value)
{
    if (!fl_json_object(c,spec,"type default min max label description")) return false;
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(spec,"type"), *min=cJSON_GetObjectItemCaseSensitive(spec,"min"), *max=cJSON_GetObjectItemCaseSensitive(spec,"max");
    if (!cJSON_IsString(type)) return invalid(c,name,"requires a declared type");
    const char *text_fields[]={"label","description"};
    for (size_t i=0;i<2;++i) {const cJSON *v=cJSON_GetObjectItemCaseSensitive(spec,text_fields[i]); if (v && !cJSON_IsString(v)) return invalid(c,name,"label and description must be strings");}
    if (!value) return invalid(c,name,"requires a value or default");
    if (!strcmp(type->valuestring,"number") || !strcmp(type->valuestring,"integer")) {
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) return invalid(c,name,"must be a finite number");
        double n=value->valuedouble;
        if (!strcmp(type->valuestring,"integer") && (floor(n)!=n || fabs(n)>9007199254740991.0)) return invalid(c,name,"must be an exact integer");
        if ((min && (!cJSON_IsNumber(min) || !isfinite(min->valuedouble))) || (max && (!cJSON_IsNumber(max) || !isfinite(max->valuedouble))) ||
            (min && max && min->valuedouble>max->valuedouble)) return invalid(c,name,"has invalid bounds");
        if ((min && n<min->valuedouble) || (max && n>max->valuedouble)) return invalid(c,name,"is outside its declared bounds");
        return true;
    }
    if (min || max) return invalid(c,name,"numeric bounds require a number or integer type");
    if (!strcmp(type->valuestring,"boolean")) return cJSON_IsBool(value) || invalid(c,name,"must be boolean");
    if (!strcmp(type->valuestring,"string")) return (cJSON_IsString(value) && strlen(value->valuestring)<=32768 && fl_json_utf8_valid(value->valuestring,strlen(value->valuestring))) || invalid(c,name,"must be a bounded UTF-8 string");
    if (!strcmp(type->valuestring,"vec2") || !strcmp(type->valuestring,"color")) {
        int count=!strcmp(type->valuestring,"vec2") ? 2 : 4;
        if (!cJSON_IsArray(value) || cJSON_GetArraySize(value)!=count) return invalid(c,name,"has an invalid vector size");
        for (const cJSON *v=value->child;v;v=v->next)
            if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || (count==4 && (v->valuedouble<0 || v->valuedouble>255))) return invalid(c,name,"has an invalid vector channel");
        return true;
    }
    return invalid(c,name,"has an unknown type");
}

bool fl_parameters_resolve(Fl_Json *c, const cJSON *schema, const cJSON *values, cJSON **effective)
{
    if (effective) *effective=NULL;
    if (!cJSON_IsObject(schema) || !cJSON_IsObject(values)) return fl_json_fail(c,"Parameter schema and values must be objects");
    for (const cJSON *v=values->child;v;v=v->next) if (!cJSON_GetObjectItemCaseSensitive(schema,v->string)) return invalid(c,v->string,"is not declared by the behavior");
    cJSON *result=effective ? cJSON_CreateObject() : NULL;
    if (effective && !result) return fl_json_fail(c,"Cannot allocate effective parameters");
    size_t count=0;
    for (const cJSON *spec=schema->child;spec;spec=spec->next) {
        if (++count>64 || !spec->string || !spec->string[0] || strlen(spec->string)>64) {cJSON_Delete(result); return fl_json_fail(c,"Parameter schema budget exceeded");}
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(values,spec->string);
        if (!value) value=cJSON_GetObjectItemCaseSensitive(spec,"default");
        if (!value_valid(c,spec->string,spec,value)) {cJSON_Delete(result); return false;}
        if (effective && !fl_json_add(c,result,spec->string,cJSON_Duplicate(value,true))) {cJSON_Delete(result); return false;}
    }
    if (effective) *effective=result;
    return true;
}
