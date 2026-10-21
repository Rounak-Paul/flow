#include "components/fl_prototype_instance.h"
#include "fl_document.h"
#include "components/fl_script.h"
#include "components/fl_style.h"
#include "schema/fl_widgets.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Canonical payload fields; offsets address encoded bytes, never native structs. */
typedef enum FieldKind { NUMBER, UNSIGNED, SIGNED, BYTE, FLAG, WORD_FLAG, COLOR } FieldKind;
typedef struct Field { const char *name; size_t offset; FieldKind kind; } Field;
typedef struct Schema { const char *name; size_t size; const Field *fields; } Schema;
static const Field transform[] = {{"x",0,NUMBER},{"y",8,NUMBER},{"width",16,NUMBER},{"height",24,NUMBER},{"rotation",32,NUMBER},{"scale_x",40,NUMBER},{"scale_y",48,NUMBER},{0}};
static const Field rectangle[] = {{"color",0,COLOR},{0}};
static const Field appearance[] = {{"fill",0,COLOR},{"stroke",4,COLOR},{"stroke_width",8,NUMBER},{"font_size",16,NUMBER},{0}};
static const Field geometry[] = {{"shape",0,BYTE},{0}};
static const Field image[] = {{"asset",0,UNSIGNED},{0}};
static const Field opacity[] = {{"value",0,NUMBER},{0}};
static const Field style[] = {{"radius",0,NUMBER},{"gradient_angle",8,NUMBER},{"shadow_blur",16,NUMBER},{"shadow_x",24,NUMBER},{"shadow_y",32,NUMBER},{"gradient_to",40,COLOR},{"shadow_color",44,COLOR},{"gradient",48,FLAG},{"align",49,BYTE},{"crop_x",50,NUMBER},{"crop_y",58,NUMBER},{"crop_width",66,NUMBER},{"crop_height",74,NUMBER},{"clip",82,FLAG},{"blur",83,NUMBER},{0}};
static const Field video[] = {{"asset",0,UNSIGNED},{"start",8,SIGNED},{"offset",16,SIGNED},{"rate",24,NUMBER},{"loop",32,FLAG},{0}};
static const Schema schemas[] = {{0}, {"transform",56,transform},{"rectangle",4,rectangle},{"text",0,NULL},{"appearance",24,appearance},{"geometry",1,geometry},{"typography",0,NULL},{"image",8,image},{"opacity",8,opacity},{"animation",0,NULL},{"script",0,NULL},{"style",91,style},{"video",33,video},{"prototype",0,NULL}};
static const char *const ease_names[] = {"linear","in","out","in_out","hold"};

/* Read/write a canonical little-endian word without alignment assumptions. */
static uint64_t word(const unsigned char *p) {uint64_t n = 0; for (unsigned i=0;i<8;++i) n |= (uint64_t)p[i] << (8*i); return n;}
static void put_word(unsigned char *p, uint64_t n) {for (unsigned i=0;i<8;++i) p[i] = (unsigned char)(n >> (8*i));}

/* Encode a fixed-length numeric array. */
static cJSON *numbers(Fl_Json *c, const double *v, size_t count)
{
    cJSON *a = cJSON_CreateArray();
    if (!a) {fl_json_fail(c,"Cannot allocate array"); return NULL;}
    for (size_t i=0;i<count;++i) fl_json_add(c,a,NULL,cJSON_CreateNumber(v[i]));
    return a;
}

cJSON *fl_document_anchor_write(Fl_Json *c, const Fl_TimeAnchor *a)
{
    cJSON *o = cJSON_CreateObject();
    fl_json_add(c,o,"kind",cJSON_CreateString(a->kind == FL_ANCHOR_BEAT_POSITION ? "position" : a->kind == FL_ANCHOR_BEAT ? "beat" : "absolute"));
    fl_json_add(c,o,"beat",fl_json_u64(a->beat)); fl_json_add(c,o,"end",cJSON_CreateBool(a->end));
    fl_json_add(c,o,"offset",fl_json_i64(a->offset));
    fl_json_add(c,o,"position",cJSON_CreateNumber(a->position));
    return o;
}

bool fl_document_anchor_read(Fl_Json *c, const cJSON *o, Fl_TimeAnchor *a)
{
    if (!fl_json_object(c,o,"kind beat end offset position")) return false;
    const cJSON *kind=fl_json_member(c,o,"kind",cJSON_String), *end=fl_json_member(c,o,"end",0);
    if (!kind || !cJSON_IsBool(end) || (strcmp(kind->valuestring,"beat") && strcmp(kind->valuestring,"absolute") && strcmp(kind->valuestring,"position"))) return fl_json_fail(c,"Invalid time anchor");
    a->kind = !strcmp(kind->valuestring,"position") ? FL_ANCHOR_BEAT_POSITION : !strcmp(kind->valuestring,"beat") ? FL_ANCHOR_BEAT : FL_ANCHOR_ABSOLUTE; a->end=cJSON_IsTrue(end);
    return fl_json_uint(c,fl_json_member(c,o,"beat",0),&a->beat) && fl_json_int(c,fl_json_member(c,o,"offset",0),&a->offset) &&
        fl_json_number(c,fl_json_member(c,o,"position",0),&a->position) && (fl_time_anchor_valid(a) || fl_json_fail(c,"Invalid anchor structure"));
}

/* Encode native animation as property-named tracks and exact anchors. */
static cJSON *animation_write(Fl_Json *c, const Fl_Animation *animation)
{
    cJSON *tracks=cJSON_CreateArray();
    for (size_t i=0;i<animation->track_count;++i) {
        const Fl_Track *t=&animation->tracks[i]; cJSON *o=cJSON_CreateObject(), *keys=cJSON_CreateArray();
        fl_json_add(c,o,"id",fl_json_u64(t->id)); fl_json_add(c,o,"property",cJSON_CreateString(fl_property_name(t->property)));
        for (size_t k=0;k<t->key_count;++k) {
            const Fl_Keyframe *key=&t->keys[k]; cJSON *item=cJSON_CreateObject();
            fl_json_add(c,item,"id",fl_json_u64(key->id)); fl_json_add(c,item,"at",fl_document_anchor_write(c,&key->at));
            fl_json_add(c,item,"value",numbers(c,key->value,4)); fl_json_add(c,item,"ease",cJSON_CreateString(ease_names[key->ease]));
            fl_json_add(c,keys,NULL,item);
        }
        fl_json_add(c,o,"keys",keys); fl_json_add(c,tracks,NULL,o);
    }
    if (!tracks) fl_json_fail(c,"Cannot allocate animation");
    return tracks;
}

/* Decode an animation into individually owned tracks; all partial storage is caller-owned. */
static bool animation_read(Fl_Json *c, const cJSON *array, Fl_Id entity, Fl_Component *out)
{
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array)>FL_PROP_COUNT) return fl_json_fail(c,"Invalid animation tracks");
    Fl_Animation *a=calloc(1,sizeof(*a)); if (!a) return fl_json_fail(c,"Cannot allocate animation");
    out->data=(unsigned char *)a; out->size=sizeof(*a);
    for (const cJSON *o=array->child;o;o=o->next) {
        if (!fl_json_object(c,o,"id property keys")) return false;
        Fl_Track *t=&a->tracks[a->track_count++]; t->entity=entity; t->property=FL_PROP_COUNT;
        const cJSON *property=fl_json_member(c,o,"property",cJSON_String), *keys=fl_json_member(c,o,"keys",cJSON_Array);
        if (!property || !keys || !fl_json_uint(c,fl_json_member(c,o,"id",0),&t->id)) return false;
        for (int p=0;p<FL_PROP_COUNT;++p) if (!strcmp(property->valuestring,fl_property_name((Fl_AnimProperty)p))) t->property=(Fl_AnimProperty)p;
        if (t->property==FL_PROP_COUNT) return fl_json_fail(c,"Unknown animated property");
        t->key_count=t->key_capacity=(size_t)cJSON_GetArraySize(keys);
        t->keys=t->key_count ? calloc(t->key_count,sizeof(*t->keys)) : NULL;
        if (t->key_count && !t->keys) {t->key_count=0; return fl_json_fail(c,"Cannot allocate keys");}
        size_t k=0;
        for (const cJSON *v=keys->child;v;v=v->next,++k) {
            Fl_Keyframe *key=&t->keys[k];
            if (!fl_json_object(c,v,"id at value ease") || !fl_json_uint(c,fl_json_member(c,v,"id",0),&key->id) ||
                !fl_document_anchor_read(c,fl_json_member(c,v,"at",cJSON_Object),&key->at)) return false;
            const cJSON *values=fl_json_member(c,v,"value",cJSON_Array), *ease=fl_json_member(c,v,"ease",cJSON_String);
            if (!values || cJSON_GetArraySize(values)!=4 || !ease) return fl_json_fail(c,"Invalid key value/ease");
            size_t j=0; for (const cJSON *n=values->child;n;n=n->next,++j) if (!fl_json_number(c,n,&key->value[j])) return false;
            key->ease=FL_EASE_COUNT;
            for (int j2=0;j2<FL_EASE_COUNT;++j2) if (!strcmp(ease->valuestring,ease_names[j2])) key->ease=(Fl_Ease)j2;
        }
    }
    return fl_animation_valid(a) || fl_json_fail(c,"Invalid animation schema");
}

cJSON *fl_document_component_write(Fl_Json *c, const Fl_Component *component)
{
    cJSON *o=cJSON_CreateObject();
    const Schema *s=component->type<sizeof(schemas)/sizeof(*schemas) ? &schemas[component->type] : NULL;
    fl_json_add(c,o,"id",fl_json_u64(component->id));
    fl_json_add(c,o,"type",s ? cJSON_CreateString(s->name) : fl_json_u64(component->type));
    fl_json_add(c,o,"version",fl_json_u64(component->version));
    if (component->type==FL_COMPONENT_PROTOTYPE) {
        Fl_PrototypeInstance instance;
        if (!fl_prototype_instance_decode(component,&instance)) fl_json_fail(c,"Invalid prototype instance");
        else {fl_json_add(c,o,"asset",fl_json_u64(instance.asset)); fl_json_add(c,o,"time",fl_document_time_write(c,&instance.time)); fl_json_add(c,o,"overrides",fl_json_parse(c,instance.overrides,instance.overrides_size));}
    } else if (component->type==FL_COMPONENT_SCRIPT) {
        Fl_Script script;
        if (!fl_script_decode(component,&script)) fl_json_fail(c,"Invalid Script component");
        else {
            fl_json_add(c,o,"asset",fl_json_u64(script.script)); fl_json_add(c,o,"enabled",cJSON_CreateBool(script.enabled));
            fl_json_add(c,o,"time",fl_document_time_write(c,&script.time));
            fl_json_add(c,o,"parameters",fl_json_parse(c,script.parameters,script.parameters_size));
            fl_json_add(c,o,"bindings",fl_json_parse(c,script.bindings,script.bindings_size));
        }
    } else if (component->type==FL_COMPONENT_ANIMATION) fl_json_add(c,o,"tracks",animation_write(c,(const Fl_Animation *)component->data));
    else if (component->type==FL_COMPONENT_TEXT) {
        char *text=malloc(component->size+1);
        if (text) {if (component->size) memcpy(text,component->data,component->size); text[component->size]=0; fl_json_add(c,o,"text",cJSON_CreateString(text)); free(text);}
        else fl_json_fail(c,"Cannot allocate text");
    } else if (component->type==FL_COMPONENT_TYPOGRAPHY) {
        Fl_Typography t;
        if (!fl_typography_decode(component,&t)) fl_json_fail(c,"Invalid typography");
        else {fl_json_add(c,o,"family",cJSON_CreateString(t.family)); fl_json_add(c,o,"weight",cJSON_CreateNumber(t.weight)); fl_json_add(c,o,"italic",cJSON_CreateBool(t.italic));}
    } else if (s && s->fields) {
        for (const Field *f=s->fields;f->name;++f) {
            const unsigned char *p=component->data+f->offset; cJSON *value=NULL;
            if (f->kind==NUMBER) {uint64_t bits=word(p); double n; memcpy(&n,&bits,8); value=cJSON_CreateNumber(n);}
            else if (f->kind==UNSIGNED) value=fl_json_u64(word(p));
            else if (f->kind==SIGNED) {uint64_t bits=word(p); int64_t n; memcpy(&n,&bits,8); value=fl_json_i64(n);}
            else if (f->kind==FLAG) value=cJSON_CreateBool(*p);
            else if (f->kind==WORD_FLAG) value=cJSON_CreateBool(word(p)!=0);
            else if (f->kind==BYTE) value=cJSON_CreateNumber(*p);
            else {double rgba[4]={p[0],p[1],p[2],p[3]}; value=numbers(c,rgba,4);}
            fl_json_add(c,o,f->name,value);
        }
    } else {
        cJSON *bytes=cJSON_CreateArray();
        for (size_t i=0;i<component->size;++i) fl_json_add(c,bytes,NULL,cJSON_CreateNumber(component->data[i]));
        fl_json_add(c,o,"bytes",bytes);
    }
    return o;
}

bool fl_document_component_read(Fl_Json *c, const cJSON *o, Fl_Id entity, Fl_Component *out)
{
    *out=(Fl_Component){0};
    if (!cJSON_IsObject(o)) return fl_json_fail(c,"Expected component object");
    const cJSON *type=fl_json_member(c,o,"type",0); const Schema *s=NULL;
    if (cJSON_IsString(type)) for (size_t i=1;i<sizeof(schemas)/sizeof(*schemas);++i)
        if (!strcmp(type->valuestring,schemas[i].name)) {out->type=i; s=&schemas[i]; break;}
    if (!s && !fl_json_uint(c,type,&out->type)) return false;
    if (!s && out->type<sizeof(schemas)/sizeof(*schemas)) return fl_json_fail(c,"Native components require named types");
    if (!fl_json_uint(c,fl_json_member(c,o,"id",0),&out->id)) return false;
    if (!fl_json_uint(c,fl_json_member(c,o,"version",0),&out->version)) return false;
    char fields[512]="id type version";
    if (out->type==FL_COMPONENT_PROTOTYPE) strcat(fields," asset time overrides");
    else if (out->type==FL_COMPONENT_SCRIPT) strcat(fields," asset enabled time parameters bindings");
    else if (out->type==FL_COMPONENT_ANIMATION) strcat(fields," tracks");
    else if (out->type==FL_COMPONENT_TEXT) strcat(fields," text");
    else if (out->type==FL_COMPONENT_TYPOGRAPHY) strcat(fields," family weight italic");
    else if (s && s->fields) for (const Field *f=s->fields;f->name;++f) {strcat(fields," "); strcat(fields,f->name);}
    else strcat(fields," bytes");
    if (!fl_json_object(c,o,fields)) return false;
    if (out->type==FL_COMPONENT_PROTOTYPE) {
        Fl_PrototypeInstance instance={0}; const cJSON *overrides=fl_json_member(c,o,"overrides",cJSON_Array);
        if (!overrides || !fl_json_uint(c,fl_json_member(c,o,"asset",0),&instance.asset) || !fl_document_time_read(c,fl_json_member(c,o,"time",cJSON_Object),&instance.time)) return false;
        char *text=cJSON_PrintUnformatted(overrides); if (!text) return fl_json_fail(c,"Cannot allocate prototype overrides");
        instance.overrides=text; instance.overrides_size=strlen(text); Fl_Id id=out->id;
        bool ok=out->version==FL_PROTOTYPE_INSTANCE_VERSION && fl_prototype_instance_encode(&instance,out); free(text); out->id=id;
        return ok || fl_json_fail(c,"Invalid prototype instance schema");
    }
    if (out->type==FL_COMPONENT_SCRIPT) {
        Fl_Script script={0}; const cJSON *enabled=fl_json_member(c,o,"enabled",0), *parameters=fl_json_member(c,o,"parameters",cJSON_Object);
        if (!cJSON_IsBool(enabled) || !parameters || !fl_json_uint(c,fl_json_member(c,o,"asset",0),&script.script) ||
            !fl_document_time_read(c,fl_json_member(c,o,"time",cJSON_Object),&script.time)) return fl_json_fail(c,"Invalid Script configuration");
        const cJSON *bindings=fl_json_member(c,o,"bindings",cJSON_Object);
        if (!bindings || !fl_script_bindings_valid(c,bindings)) return false;
        script.enabled=cJSON_IsTrue(enabled);
        char *text=cJSON_PrintUnformatted(parameters); if (!text) return fl_json_fail(c,"Cannot encode parameters");
        char *binding_text=cJSON_PrintUnformatted(bindings);
        if (!binding_text) {cJSON_free(text); return fl_json_fail(c,"Cannot encode bindings");}
        script.bindings=binding_text; script.bindings_size=strlen(binding_text);
        script.parameters=text; script.parameters_size=strlen(text); Fl_Id id=out->id; uint64_t version=out->version;
        bool ok=version==FL_SCRIPT_VERSION && fl_script_encode(&script,out); cJSON_free(text); cJSON_free(binding_text); out->id=id;
        return ok || fl_json_fail(c,"Invalid Script schema or parameter budget");
    }
    if (out->type==FL_COMPONENT_ANIMATION) return animation_read(c,fl_json_member(c,o,"tracks",cJSON_Array),entity,out) && fl_widget_component_valid(out);
    if (out->type==FL_COMPONENT_TEXT) {
        char *text=fl_json_string(c,fl_json_member(c,o,"text",cJSON_String));
        out->data=(unsigned char *)text; out->size=text ? strlen(text) : 0;
        return text && fl_widget_component_valid(out);
    }
    if (out->type==FL_COMPONENT_TYPOGRAPHY) {
        const cJSON *family=fl_json_member(c,o,"family",cJSON_String), *italic=fl_json_member(c,o,"italic",0); uint64_t weight;
        if (!family || strlen(family->valuestring)>FL_TYPOGRAPHY_FAMILY_MAX || !cJSON_IsBool(italic) ||
            !fl_json_uint(c,fl_json_member(c,o,"weight",0),&weight) || weight>1000) return fl_json_fail(c,"Invalid typography");
        Fl_Typography t={.weight=(unsigned)weight,.italic=cJSON_IsTrue(italic)}; strcpy(t.family,family->valuestring);
        unsigned char bytes[3+FL_TYPOGRAPHY_FAMILY_MAX]; size_t size=fl_typography_encode(&t,bytes);
        Fl_Component value={.type = out->type, .version = out->version, .size = size, .data = bytes, .id=out->id};
        return size && fl_widget_component_valid(&value) && fl_component_clone(&value,out);
    }
    const cJSON *raw=NULL;
    if (!s) {raw=fl_json_member(c,o,"bytes",cJSON_Array); if (!raw) return false;}
    out->size=s ? s->size : (size_t)cJSON_GetArraySize(raw);
    unsigned char *bytes=calloc(out->size ? out->size : 1,1); out->data=bytes;
    if (!bytes) return fl_json_fail(c,"Cannot allocate component");
    if (!s) {
        size_t i=0; for (const cJSON *v=raw->child;v;v=v->next,++i) {uint64_t n; if (!fl_json_uint(c,v,&n) || n>255) return fl_json_fail(c,"Invalid byte"); bytes[i]=(unsigned char)n;}
    } else for (const Field *f=s->fields;f->name;++f) {
        const cJSON *v=fl_json_member(c,o,f->name,0); unsigned char *p=bytes+f->offset; uint64_t n=0;
        if (f->kind==NUMBER) {double d; if (!fl_json_number(c,v,&d)) return false; memcpy(&n,&d,8); put_word(p,n);}
        else if (f->kind==UNSIGNED) {if (!fl_json_uint(c,v,&n)) return false; put_word(p,n);}
        else if (f->kind==SIGNED) {int64_t d; if (!fl_json_int(c,v,&d)) return false; memcpy(&n,&d,8); put_word(p,n);}
        else if (f->kind==WORD_FLAG) {if (!cJSON_IsBool(v)) return fl_json_fail(c,"Expected boolean"); put_word(p,cJSON_IsTrue(v));}
        else if (f->kind==FLAG) {if (!cJSON_IsBool(v)) return fl_json_fail(c,"Expected boolean"); *p=cJSON_IsTrue(v);}
        else if (f->kind==BYTE) {if (!fl_json_uint(c,v,&n) || n>255) return fl_json_fail(c,"Invalid byte"); *p=(unsigned char)n;}
        else {
            if (!cJSON_IsArray(v) || cJSON_GetArraySize(v)!=4) return fl_json_fail(c,"Expected RGBA array");
            size_t i=0; for (const cJSON *channel=v->child;channel;channel=channel->next,++i) {if (!fl_json_uint(c,channel,&n) || n>255) return fl_json_fail(c,"Invalid RGBA channel"); p[i]=(unsigned char)n;}
        }
    }
    return fl_widget_component_valid(out) || fl_json_fail(c,"Invalid component schema");
}
