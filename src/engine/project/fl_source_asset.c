#include "fl_source_asset.h"
#include "fl_prototype.h"
#include "components/fl_script.h"
#include "scripting/fl_script_runtime.h"
#include "schema/fl_widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const Fl_SourceFormat *fl_source_asset_format(Fl_AssetKind kind)
{
    static const Fl_SourceFormat lua={"assets/scripts","lua","lua",FL_SCRIPT_SOURCE_LIMIT};
    static const Fl_SourceFormat prototype={"assets/prototypes","flproto.json","json",FL_PROTOTYPE_SOURCE_LIMIT};
    switch (kind) {case FL_ASSET_LUA: return &lua; case FL_ASSET_PROTOTYPE: return &prototype; default: return NULL;}
}

typedef struct SourceReferences {Fl_SourceInfo *info; size_t capacity;} SourceReferences;
/* Retain every semantic direct reference once per occurrence for deletion checks and dependency compilation. */
static bool append_reference(Fl_Id asset, Fl_AssetKind kind, void *data)
{
    (void)kind; SourceReferences *references=data; Fl_SourceInfo *info=references->info;
    if (info->reference_count==references->capacity) {
        size_t capacity=references->capacity ? references->capacity*2 : 16;
        if (capacity<references->capacity || capacity>SIZE_MAX/sizeof(Fl_Id)) return false;
        Fl_Id *items=realloc(info->references,capacity*sizeof(*items)); if (!items) return false;
        info->references=items; references->capacity=capacity;
    }
    info->references[info->reference_count++]=asset; return true;
}

bool fl_source_asset_compile(Fl_AssetKind kind, const char *source, size_t size, Fl_SourceInfo *info, char *error, size_t capacity)
{
    if (!info) return false;
    *info=(Fl_SourceInfo){0};
    if (error && capacity) error[0]=0;
    const Fl_SourceFormat *format=fl_source_asset_format(kind);
    if (!format || !source || !size || size>format->limit || !fl_json_utf8_valid(source,size)) {
        if (error && capacity) snprintf(error,capacity,"Invalid source asset encoding or size");
        return false;
    }
    if (kind==FL_ASSET_LUA) return fl_script_dependencies(source,size,&info->references,&info->reference_count,error,capacity);
    Fl_Json context={0}; Fl_Prototype prototype={0};
    bool valid=fl_prototype_read(&context,source,size,&prototype);
    SourceReferences references={.info=info};
    for (size_t i=0;valid && i<prototype.scope.clip_count;++i) valid=append_reference(prototype.scope.clips[i].asset,FL_ASSET_AUDIO,&references);
    for (size_t i=0;valid && i<prototype.scope.entity_count;++i) {
        const Fl_Entity *e=&prototype.scope.entities[i];
        for (size_t j=0;valid && j<e->component_count;++j) valid=fl_component_visit_assets(&e->components[j],append_reference,&references);
    }
    if (!valid) {free(info->references); *info=(Fl_SourceInfo){0}; fl_json_fail(&context,"Cannot compile prototype resource dependencies");}
    fl_prototype_clear(&prototype);
    if (!valid && error && capacity) snprintf(error,capacity,"%s",context.error);
    return valid;
}

bool fl_source_asset_validate(Fl_AssetKind kind, const char *source, size_t size, char *error, size_t capacity)
{
    Fl_SourceInfo info={0}; bool valid=fl_source_asset_compile(kind,source,size,&info,error,capacity);
    free(info.references); return valid;
}

bool fl_source_asset_name_valid(const char *name)
{
    size_t size = name ? strlen(name) : 0;
    if (!size || size > FL_SOURCE_NAME_MAX || !fl_json_utf8_valid(name,size)) return false;
    for (size_t i = 0; i < size; ++i) if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f) return false;
    return true;
}


/* Copy at most limit bytes, retaining only complete valid UTF-8 codepoints. */
static size_t name_prefix(char *out, const char *name, size_t limit)
{
    size_t size=strlen(name); if (size>limit) size=limit;
    while (size && !fl_json_utf8_valid(name,size)) --size;
    memcpy(out,name,size); out[size]=0; return size;
}

bool fl_source_asset_unique_name(const Fl_Project *project, Fl_AssetKind kind, const char *base, char out[FL_SOURCE_NAME_MAX+1])
{
    if (!project || !base || !out || !fl_source_asset_format(kind)) return false;
    char stem[FL_SOURCE_NAME_MAX+1]; name_prefix(stem,base,FL_SOURCE_NAME_MAX);
    if (!fl_source_asset_name_valid(stem)) snprintf(stem,sizeof(stem),"%s",kind==FL_ASSET_LUA ? "Script" : "Prototype");
    snprintf(out,FL_SOURCE_NAME_MAX+1,"%s",stem);
    size_t count=fl_project_asset_count(project);
    for (size_t n=2;fl_project_asset_named(project,kind,out);++n) {
        if (n>count+1) return false;
        char suffix[32]; int size=snprintf(suffix,sizeof(suffix)," %zu",n);
        if (size<=0 || (size_t)size>=sizeof(suffix)) return false;
        size_t prefix=name_prefix(out,stem,FL_SOURCE_NAME_MAX-(size_t)size);
        memcpy(out+prefix,suffix,(size_t)size+1);
    }
    return true;
}
