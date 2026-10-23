#include "fl_document.h"
#include "components/fl_script.h"
#include "project/fl_manuscript.h"
#include <stdlib.h>
#include <string.h>

/* Append an exact unsigned field to an owned JSON object. */
static void uint_field(Fl_Json *c, cJSON *o, const char *key, uint64_t n) {fl_json_add(c,o,key,fl_json_u64(n));}
/* Decode a required exact unsigned field. */
static bool read_uint(Fl_Json *c, const cJSON *o, const char *key, uint64_t *n) {return fl_json_uint(c,fl_json_member(c,o,key,0),n);}
static bool read_manuscript(Fl_Json *c, const cJSON *o, Fl_Scene *s);
/* Allocate a required array's contiguous record storage, checked against document and address limits. */
static void *records(Fl_Json *c, const cJSON *o, const char *key, size_t stride, size_t *count)
{
    const cJSON *a=fl_json_member(c,o,key,cJSON_Array); *count=0;
    if (!a) return NULL;
    size_t n=(size_t)cJSON_GetArraySize(a);
    if (n>FL_DOCUMENT_LIMIT/stride || n>SIZE_MAX/stride) {fl_json_fail(c,"Document record limit exceeded"); return NULL;}
    void *storage=n ? calloc(n,stride) : NULL;
    if (n && !storage) {fl_json_fail(c,"Cannot allocate document records"); return NULL;}
    *count=n; return storage;
}

bool fl_document_content_write(Fl_Json *c, cJSON *o, const Fl_Scene *s)
{
    cJSON *entities=cJSON_CreateArray(), *beats=cJSON_CreateArray(), *clips=cJSON_CreateArray();
    for (size_t i=0;i<s->entity_count;++i) {
        const Fl_Entity *e=&s->entities[i]; cJSON *item=cJSON_CreateObject(), *components=cJSON_CreateArray();
        uint_field(c,item,"id",e->id); uint_field(c,item,"parent",e->parent); fl_json_add(c,item,"name",cJSON_CreateString(e->name));
        fl_json_add(c,item,"visible",cJSON_CreateBool(e->visible));
        for (size_t j=0;j<e->component_count;++j) fl_json_add(c,components,NULL,fl_document_component_write(c,&e->components[j]));
        fl_json_add(c,item,"components",components); fl_json_add(c,entities,NULL,item);
    }
    for (size_t i=0;i<s->beat_count;++i) {
        const Fl_Beat *b=&s->beats[i]; cJSON *item=cJSON_CreateObject(); uint_field(c,item,"id",b->id);
        fl_json_add(c,item,"name",cJSON_CreateString(b->name)); uint_field(c,item,"duration",b->duration); fl_json_add(c,item,"auto_advance",cJSON_CreateBool(b->auto_advance));
        fl_json_add(c,beats,NULL,item);
    }
    for (size_t i=0;i<s->clip_count;++i) {
        const Fl_AudioClip *clip=&s->clips[i]; cJSON *item=cJSON_CreateObject();
        uint_field(c,item,"id",clip->id); uint_field(c,item,"asset",clip->asset); fl_json_add(c,item,"start",fl_document_anchor_write(c,&clip->start));
        uint_field(c,item,"source_offset",clip->source_offset); uint_field(c,item,"duration",clip->duration); fl_json_add(c,item,"gain",cJSON_CreateNumber(clip->gain));
        uint_field(c,item,"fade_in",clip->fade_in); uint_field(c,item,"fade_out",clip->fade_out); fl_json_add(c,clips,NULL,item);
    }
    fl_json_add(c,o,"entities",entities); fl_json_add(c,o,"beats",beats); fl_json_add(c,o,"clips",clips);
    return !c->error[0];

}

cJSON *fl_document_scene_write(Fl_Json *c, const Fl_Scene *s)
{
    cJSON *o=cJSON_CreateObject();
    uint_field(c,o,"version",1); uint_field(c,o,"id",s->id); uint_field(c,o,"revision",s->revision);
    fl_json_add(c,o,"name",cJSON_CreateString(s->name)); uint_field(c,o,"width",s->width); uint_field(c,o,"height",s->height); uint_field(c,o,"duration",s->duration);
    uint_field(c,o,"aspect_width",s->aspect_width); uint_field(c,o,"aspect_height",s->aspect_height);
    fl_json_add(c,o,"camera_x",cJSON_CreateNumber(s->camera_x)); fl_json_add(c,o,"camera_y",cJSON_CreateNumber(s->camera_y));
    cJSON *background=cJSON_CreateArray();
    for (size_t i=0;i<4;++i) fl_json_add(c,background,NULL,cJSON_CreateNumber(s->background[i]));
    fl_json_add(c,o,"background",background);
    fl_document_content_write(c,o,s);
    fl_json_add(c,o,"voice",cJSON_CreateString(s->voice ? s->voice : "Ryan"));
    fl_json_add(c,o,"language",cJSON_CreateString(s->language ? s->language : "English"));
    uint_field(c,o,"voice_reference",s->voice_reference);
    fl_json_add(c,o,"voice_transcript",cJSON_CreateString(s->voice_transcript ? s->voice_transcript : ""));
    cJSON *phases=cJSON_CreateArray();
    for (size_t i=0;i<s->phase_count;++i) {
        const Fl_Phase *phase=&s->phases[i]; cJSON *item=cJSON_CreateObject();
        uint_field(c,item,"id",phase->id); uint_field(c,item,"beat",phase->beat); uint_field(c,item,"clip",phase->clip);
        fl_json_add(c,item,"title",cJSON_CreateString(phase->title ? phase->title : ""));
        fl_json_add(c,item,"text",cJSON_CreateString(phase->text ? phase->text : ""));
        fl_json_add(c,phases,NULL,item);
    }
    fl_json_add(c,o,"phases",phases);
    return o;
}

bool fl_document_scene_read(Fl_Json *c, const cJSON *o, Fl_Scene *s)
{
    if (!fl_json_object(c,o,"version id revision name width height duration aspect_width aspect_height camera_x camera_y background entities beats clips voice language voice_reference voice_transcript phases")) return false;
    uint64_t version;
    if (!read_uint(c,o,"version",&version) || version!=1 || !read_uint(c,o,"id",&s->id) || !read_uint(c,o,"revision",&s->revision) ||
        !read_uint(c,o,"width",&s->width) || !read_uint(c,o,"height",&s->height) || !read_uint(c,o,"duration",&s->duration) ||
        !read_uint(c,o,"aspect_width",&s->aspect_width) || !read_uint(c,o,"aspect_height",&s->aspect_height) ||
        !fl_json_number(c,fl_json_member(c,o,"camera_x",0),&s->camera_x) || !fl_json_number(c,fl_json_member(c,o,"camera_y",0),&s->camera_y)) return fl_json_fail(c,"Invalid scene settings");
    s->name=fl_json_string(c,fl_json_member(c,o,"name",cJSON_String));
    if (!s->name || !s->name[0] || !s->id || !s->revision || !s->width || s->width>65536 || !s->height || s->height>65536 ||
        !s->duration || s->duration>INT64_MAX || s->aspect_width>65536 || s->aspect_height>65536 || (!s->aspect_width!=!s->aspect_height)) return fl_json_fail(c,"Scene settings out of range");
    const cJSON *background=fl_json_member(c,o,"background",cJSON_Array);
    if (!background || cJSON_GetArraySize(background)!=4) return fl_json_fail(c,"Invalid background");
    size_t i=0;
    for (const cJSON *v=background->child;v;v=v->next,++i) {uint64_t n; if (!fl_json_uint(c,v,&n) || n>255) return fl_json_fail(c,"Invalid background channel"); s->background[i]=(unsigned char)n;}
    if (s->background[3]!=255) return fl_json_fail(c,"Background must be opaque");
    if (!fl_document_content_read(c,o,s)) return false;
    return read_manuscript(c,o,s);
}

/* Optional on older scenes: a missing voice is the built-in English speaker. */
static char *own_text(const char *text)
{
    size_t n=strlen(text)+1; char *copy=malloc(n);
    if (copy) memcpy(copy,text,n);
    return copy;
}
static bool read_manuscript(Fl_Json *c, const cJSON *o, Fl_Scene *s)
{
    const cJSON *voice=cJSON_GetObjectItemCaseSensitive(o,"voice");
    const cJSON *language=cJSON_GetObjectItemCaseSensitive(o,"language");
    const cJSON *transcript=cJSON_GetObjectItemCaseSensitive(o,"voice_transcript");
    const cJSON *reference=cJSON_GetObjectItemCaseSensitive(o,"voice_reference");
    const cJSON *phases=cJSON_GetObjectItemCaseSensitive(o,"phases");
    if ((voice && !cJSON_IsString(voice)) || (language && !cJSON_IsString(language)) || (transcript && !cJSON_IsString(transcript)) ||
        (phases && !cJSON_IsArray(phases))) return fl_json_fail(c,"Invalid manuscript");
    s->voice=own_text(voice ? voice->valuestring : "Ryan");
    s->language=own_text(language ? language->valuestring : "English");
    if (!s->voice || !s->language) return fl_json_fail(c,"Cannot allocate manuscript");
    if (transcript && transcript->valuestring[0]) {
        s->voice_transcript=own_text(transcript->valuestring);
        if (!s->voice_transcript) return fl_json_fail(c,"Cannot allocate manuscript");
    }
    if (reference && !fl_json_uint(c,reference,&s->voice_reference)) return false;
    if (!phases) return true;
    size_t count=0; Fl_Phase *storage=records(c,o,"phases",sizeof(*storage),&count);
    if (c->error[0]) return false;
    s->phases=storage; s->phase_count=count;
    size_t i=0;
    for (const cJSON *v=phases->child;v;v=v->next,++i) {
        Fl_Phase *phase=&storage[i];
        if (!fl_json_object(c,v,"id beat clip title text") || !read_uint(c,v,"id",&phase->id) || !read_uint(c,v,"beat",&phase->beat) || !read_uint(c,v,"clip",&phase->clip)) return false;
        phase->title=fl_json_string(c,fl_json_member(c,v,"title",cJSON_String));
        phase->text=fl_json_string(c,fl_json_member(c,v,"text",cJSON_String));
        if (!phase->title || !phase->text) return fl_json_fail(c,"Invalid phase");
    }
    return !c->error[0];
}

bool fl_document_content_read(Fl_Json *c, const cJSON *o, Fl_Scene *s)
{
    Fl_Entity *entities=records(c,o,"entities",sizeof(*entities),&s->entity_count); s->entities=entities; s->entity_capacity=s->entity_count;
    if (c->error[0]) return false;
    size_t i=0;
    for (const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"entities")->child;v;v=v->next,++i) {
        Fl_Entity *e=&entities[i];
        if (!fl_json_object(c,v,"id parent name visible components") || !read_uint(c,v,"id",&e->id) || !read_uint(c,v,"parent",&e->parent)) return false;
        e->name=fl_json_string(c,fl_json_member(c,v,"name",cJSON_String)); const cJSON *visible=fl_json_member(c,v,"visible",0);
        if (!e->name || !e->name[0] || !e->id || !cJSON_IsBool(visible)) return fl_json_fail(c,"Invalid entity");
        e->visible=cJSON_IsTrue(visible);
        Fl_Component *components=records(c,v,"components",sizeof(*components),&e->component_count); e->components=components;
        if (c->error[0] || e->component_count>64) return fl_json_fail(c,"Component count limit exceeded");
        size_t j=0;
        for (const cJSON *component=cJSON_GetObjectItemCaseSensitive(v,"components")->child;component;component=component->next,++j) {
            if (!fl_document_component_read(c,component,e->id,&components[j])) return false;
            for (size_t k=0;k<j;++k) if (components[k].id==components[j].id || (components[k].type==components[j].type && components[j].type!=FL_COMPONENT_SCRIPT)) return fl_json_fail(c,"Duplicate component type");
        }
        if (!fl_entity_component(e,FL_COMPONENT_POSE)) return fl_json_fail(c,"Pose is required");
    }
    Fl_Beat *beats=records(c,o,"beats",sizeof(*beats),&s->beat_count); s->beats=beats;
    if (c->error[0]) return false;
    i=0;
    for (const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"beats")->child;v;v=v->next,++i) {
        Fl_Beat *b=&beats[i];
        if (!fl_json_object(c,v,"id name duration auto_advance") || !read_uint(c,v,"id",&b->id) || !read_uint(c,v,"duration",&b->duration)) return false;
        b->name=fl_json_string(c,fl_json_member(c,v,"name",cJSON_String)); const cJSON *automatic=fl_json_member(c,v,"auto_advance",0);
        if (!b->name || !cJSON_IsBool(automatic)) return fl_json_fail(c,"Invalid beat");
        b->auto_advance=cJSON_IsTrue(automatic);
    }
    Fl_AudioClip *clips=records(c,o,"clips",sizeof(*clips),&s->clip_count); s->clips=clips;
    if (c->error[0]) return false;
    i=0;
    for (const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"clips")->child;v;v=v->next,++i) {
        Fl_AudioClip *clip=&clips[i];
        if (!fl_json_object(c,v,"id asset start source_offset duration gain fade_in fade_out") || !read_uint(c,v,"id",&clip->id) || !read_uint(c,v,"asset",&clip->asset) ||
            !fl_document_anchor_read(c,fl_json_member(c,v,"start",cJSON_Object),&clip->start) || !read_uint(c,v,"source_offset",&clip->source_offset) || !read_uint(c,v,"duration",&clip->duration) ||
            !fl_json_number(c,fl_json_member(c,v,"gain",0),&clip->gain) || !read_uint(c,v,"fade_in",&clip->fade_in) || !read_uint(c,v,"fade_out",&clip->fade_out)) return false;
    }
    return !c->error[0];
}
