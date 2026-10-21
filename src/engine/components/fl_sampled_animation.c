#include "fl_sampled_animation.h"
#include "schema/fl_properties.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_HEADER 16u
#define SAMPLE_RECORD (24u+FL_PROP_COUNT*32u)

/* Decode canonical words without alignment or host endianness assumptions. */
static uint64_t sample_word(const unsigned char *bytes)
{
    uint64_t value=0; for (unsigned i=0;i<8;++i) value|=(uint64_t)bytes[i]<<(8*i); return value;
}

/* Borrow the checked record table shape before random access. */
static bool sample_shape(const Fl_Component *component, size_t *count, bool *text)
{
    if (!component || component->type!=FL_COMPONENT_SAMPLED || component->version!=FL_SAMPLED_ANIMATION_VERSION || !component->data || component->size<SAMPLE_HEADER || component->size>FL_SAMPLED_ANIMATION_LIMIT) return false;
    uint64_t n=sample_word(component->data), flags=sample_word(component->data+8);
    if (!n || n>FL_SAMPLED_ANIMATION_KEYS || flags>1 || n>(component->size-SAMPLE_HEADER)/SAMPLE_RECORD) return false;
    *count=(size_t)n; *text=flags!=0; return true;
}

/* Decode one immutable pose while retaining component ownership of its text bytes. */
static bool sample_key(const Fl_Component *component, size_t count, size_t index, bool text, Fl_SampledKey *out)
{
    const unsigned char *record=component->data+SAMPLE_HEADER+index*SAMPLE_RECORD;
    uint64_t tick=sample_word(record); if (tick>INT64_MAX) return false;
    Fl_SampledKey key={.tick=(Fl_Tick)tick}; size_t offset=8;
    for (int p=0;p<FL_PROP_COUNT;++p) for (unsigned c=0;c<4;++c,offset+=8) {
        uint64_t bits=sample_word(record+offset); memcpy(&key.values[p][c],&bits,8);
    }
    uint64_t start=sample_word(record+offset), size=sample_word(record+offset+8);
    if (text) {
        if (start<SAMPLE_HEADER+count*SAMPLE_RECORD || start>component->size || size>component->size-start) return false;
        key.text=(const char *)component->data+(size_t)start; key.text_size=(size_t)size;
    } else if (start || size) return false;
    *out=key; return true;
}

bool fl_sampled_animation_valid(const Fl_Component *component)
{
    size_t count; bool text; if (!sample_shape(component,&count,&text)) return false;
    Fl_Tick previous=-1;
    for (size_t i=0;i<count;++i) {
        Fl_SampledKey key;
        if (!sample_key(component,count,i,text,&key) || key.tick<=previous || (text && !fl_json_utf8_valid(key.text,key.text_size))) return false;
        for (int p=0;p<FL_PROP_COUNT;++p) if (!fl_property_value_valid((Fl_AnimProperty)p,key.values[p])) return false;
        previous=key.tick;
    }
    return true;
}

bool fl_sampled_animation_encode(const Fl_SampledKey *keys, size_t count, bool text, Fl_Component *out)
{
    if (!keys || !out || !count || count>FL_SAMPLED_ANIMATION_KEYS) return false;
    Fl_Binary bytes={0}, strings={0}; fl_binary_u64(&bytes,count); fl_binary_u64(&bytes,text);
    size_t previous_start=0; Fl_Tick previous=-1;
    for (size_t i=0;i<count;++i) {
        const Fl_SampledKey *key=&keys[i];
        if (key->tick<0 || key->tick<=previous || (text && (!key->text || !fl_json_utf8_valid(key->text,key->text_size)))) {bytes.failed=true; break;}
        fl_binary_u64(&bytes,(uint64_t)key->tick);
        for (int p=0;p<FL_PROP_COUNT;++p) {
            if (!fl_property_value_valid((Fl_AnimProperty)p,key->values[p])) {bytes.failed=true; break;}
            for (unsigned c=0;c<4;++c) {uint64_t bits; memcpy(&bits,&key->values[p][c],8); fl_binary_u64(&bytes,bits);}
        }
        size_t start=0;
        if (text) {
            bool same=i && key->text_size==keys[i-1].text_size && (!key->text_size || !memcmp(key->text,keys[i-1].text,key->text_size));
            start=same ? previous_start : strings.size;
            if (!same) {
                if (key->text_size>FL_SAMPLED_ANIMATION_LIMIT-strings.size) {bytes.failed=true; break;}
                fl_binary_write(&strings,key->text,key->text_size);
            }
            previous_start=start; start+=SAMPLE_HEADER+count*SAMPLE_RECORD;
        }
        fl_binary_u64(&bytes,start); fl_binary_u64(&bytes,text ? key->text_size : 0); previous=key->tick;
        if (bytes.failed || strings.failed || strings.size>FL_SAMPLED_ANIMATION_LIMIT-bytes.size) {bytes.failed=true; break;}
    }
    if (!bytes.failed && !strings.failed) fl_binary_write(&bytes,strings.data,strings.size);
    free(strings.data);
    if (bytes.failed || bytes.size>FL_SAMPLED_ANIMATION_LIMIT) {free(bytes.data); return false;}
    *out=(Fl_Component){.type=FL_COMPONENT_SAMPLED,.version=FL_SAMPLED_ANIMATION_VERSION,.data=bytes.data,.size=bytes.size}; return true;
}

bool fl_sampled_animation_sample(const Fl_Component *component, Fl_Tick tick, Fl_SampledKey *out, bool *text)
{
    size_t count; bool has_text; if (!out || !sample_shape(component,&count,&has_text)) return false;
    size_t lo=0,hi=count;
    while (lo<hi) {size_t mid=lo+(hi-lo)/2; if ((Fl_Tick)sample_word(component->data+SAMPLE_HEADER+mid*SAMPLE_RECORD)<=tick) lo=mid+1; else hi=mid;}
    size_t before=lo ? lo-1 : 0; Fl_SampledKey key;
    if (!sample_key(component,count,before,has_text,&key)) return false;
    if (lo && lo<count) {
        Fl_SampledKey after; if (!sample_key(component,count,lo,has_text,&after) || after.tick<=key.tick) return false;
        double t=(double)(((long double)tick-key.tick)/((long double)after.tick-key.tick));
        for (int p=0;p<FL_PROP_COUNT;++p) for (unsigned c=0;c<4;++c) key.values[p][c]+=(after.values[p][c]-key.values[p][c])*t;
    }
    key.tick=tick; *out=key; if (text) *text=has_text; return true;
}

cJSON *fl_sampled_animation_write(Fl_Json *context, const Fl_Component *component)
{
    size_t count; bool text; if (!sample_shape(component,&count,&text)) {fl_json_fail(context,"Invalid sampled animation"); return NULL;}
    cJSON *samples=cJSON_CreateArray(); const char *previous=NULL; size_t previous_size=0;
    for (size_t i=0;samples && i<count && !context->error[0];++i) {
        Fl_SampledKey key;
        if (!sample_key(component,count,i,text,&key)) {fl_json_fail(context,"Invalid sampled pose"); break;}
        cJSON *sample=cJSON_CreateObject(), *values=cJSON_CreateObject();
        if (!fl_json_add(context,samples,NULL,sample)) {cJSON_Delete(values); break;}
        fl_json_add(context,sample,"tick",fl_json_i64(key.tick));
        if (!fl_json_add(context,sample,"values",values)) break;
        for (int p=0;p<FL_PROP_COUNT && !context->error[0];++p) {
            const Fl_PropertyDef *definition=fl_property_definition((Fl_AnimProperty)p);
            cJSON *value=definition->channels==1 ? cJSON_CreateNumber(key.values[p][0]) : cJSON_CreateDoubleArray(key.values[p],(int)definition->channels);
            fl_json_add(context,values,definition->name,value);
        }
        if (text && (!previous || previous_size!=key.text_size || (key.text_size && memcmp(previous,key.text,key.text_size)))) {
            char *copy=malloc(key.text_size+1);
            if (!copy) {fl_json_fail(context,"Cannot encode sampled text"); break;}
            memcpy(copy,key.text,key.text_size); copy[key.text_size]=0;
            fl_json_add(context,values,"text",cJSON_CreateString(copy)); free(copy); previous=key.text; previous_size=key.text_size;
        }
    }
    if (context->error[0]) {cJSON_Delete(samples); return NULL;} return samples;
}

bool fl_sampled_animation_read(Fl_Json *context, const cJSON *samples, Fl_Component *out)
{
    size_t count=cJSON_IsArray(samples) ? (size_t)cJSON_GetArraySize(samples) : 0;
    if (!count || count>FL_SAMPLED_ANIMATION_KEYS) return fl_json_fail(context,"Invalid sampled animation count");
    Fl_SampledKey *keys=calloc(count,sizeof(*keys)); if (!keys) return fl_json_fail(context,"Cannot allocate sampled poses");
    char fields[2048]="text";
    for (int p=0;p<FL_PROP_COUNT;++p) {strcat(fields," "); strcat(fields,fl_property_name((Fl_AnimProperty)p));}
    bool valid=true,has_text=false; const char *text=NULL; size_t index=0;
    for (const cJSON *sample=samples->child;valid && sample;sample=sample->next,++index) {
        const cJSON *values=fl_json_member(context,sample,"values",cJSON_Object);
        valid=fl_json_object(context,sample,"tick values") && fl_json_int(context,fl_json_member(context,sample,"tick",0),&keys[index].tick) && fl_json_object(context,values,fields);
        for (int p=0;valid && p<FL_PROP_COUNT;++p) {
            const Fl_PropertyDef *definition=fl_property_definition((Fl_AnimProperty)p); const cJSON *value=cJSON_GetObjectItemCaseSensitive(values,definition->name);
            if (definition->channels==1) valid=fl_json_number(context,value,&keys[index].values[p][0]);
            else {
                valid=cJSON_IsArray(value) && cJSON_GetArraySize(value)==(int)definition->channels;
                size_t c=0; for (const cJSON *v=valid ? value->child : NULL;valid && v;v=v->next,++c) valid=fl_json_number(context,v,&keys[index].values[p][c]);
            }
        }
        const cJSON *value=values ? cJSON_GetObjectItemCaseSensitive(values,"text") : NULL;
        if (value) {valid=valid && cJSON_IsString(value) && (!index || has_text); if (valid) {text=value->valuestring; has_text=true;}}
        keys[index].text=text; keys[index].text_size=text ? strlen(text) : 0;
    }
    valid=valid && fl_sampled_animation_encode(keys,count,has_text,out); free(keys);
    return valid || fl_json_fail(context,"Invalid sampled animation data or budget");
}
