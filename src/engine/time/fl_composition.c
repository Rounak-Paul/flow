#include "fl_composition.h"
#include "fl_scene_state.h"
#include "components/fl_prototype_instance.h"
#include "components/fl_local_composition.h"
#include "project/fl_prototype.h"
#include "project/fl_scene_storage.h"
#include "doc/fl_document.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define COMPOSITION_ENTITIES 100000u
#define COMPOSITION_SCOPES 4096u
#define COMPOSITION_DEPTH 32u

typedef struct Scope {
    Fl_Prototype prototype;
    Fl_SceneState state;
    Fl_ScriptRuntime *builder;
    size_t parent;
    Fl_TimeMapping mapping;
    Fl_Tick tick;
    bool active;
    Fl_Id key, asset;
    cJSON *operations, *canonical;
    Fl_Id owner, generator;
    bool *seen;
    size_t *flat;
} Scope;
typedef struct Layer {size_t scope, offset; Fl_Id path[COMPOSITION_DEPTH];} Layer;
typedef struct Entry {Fl_Origin origin; size_t scope,local; Fl_Id *components;} Entry;

struct Fl_Composition {
    Fl_Scene scene;
    Entry *entries;
    Scope *scopes;
    size_t count, unresolved,compiled_bytes;
    Fl_Json context;
    const atomic_bool *cancel;
};

/* Derive stable scoped identities independently of source order, revisions or frame time. */
static Fl_Id identity(Fl_Id scope, Fl_Id local, Fl_Id domain)
{
    uint64_t value=scope ^ (local*UINT64_C(0x9e3779b97f4a7c15)) ^ domain;
    value^=value>>30; value*=UINT64_C(0xbf58476d1ce4e5b9); value^=value>>27; value*=UINT64_C(0x94d049bb133111eb);
    value^=value>>31; return value ? value : UINT64_MAX;
}

bool fl_composition_required(const Fl_Scene *scene)
{
    for (size_t i=0;scene && i<scene->entity_count;++i) if (fl_entity_component(&scene->entities[i],FL_COMPONENT_PROTOTYPE) || fl_entity_component(&scene->entities[i],FL_COMPONENT_COMPOSITION) || fl_entity_component(&scene->entities[i],FL_COMPONENT_SCRIPT)) return true;
    return false;
}

/* Append a borrowing render record; its component/name storage belongs to the compiled scope. */
static bool append(Fl_Composition *c, const Fl_Entity *entity, Fl_Id id, Fl_Id parent, Fl_Origin origin, size_t scope, size_t local, size_t *out)
{
    Fl_Scene *s=&c->scene;
    if (s->entity_count>=COMPOSITION_ENTITIES || fl_scene_entity(s,id)) return fl_json_fail(&c->context,"Generated entity identity collision or composition entity limit exceeded");
    if (s->entity_count==s->entity_capacity) {
        size_t capacity=s->entity_capacity ? s->entity_capacity*2 : 32;
        Fl_Entity *entities=realloc((void *)s->entities,capacity*sizeof(*entities));
        if (!entities) return fl_json_fail(&c->context,"Cannot allocate evaluated topology");
        s->entities=entities;
        Entry *entries=realloc(c->entries,capacity*sizeof(*entries));
        if (!entries) return fl_json_fail(&c->context,"Cannot allocate composition provenance");
        c->entries=entries; s->entity_capacity=capacity;
    }
    if (!fl_scene_storage_reserve_index(s,s->entity_count+1)) return fl_json_fail(&c->context,"Cannot index evaluated topology");
    Fl_Id *component_ids=scope && entity->component_count ? malloc(entity->component_count*sizeof(*component_ids)) : NULL;
    if (scope && entity->component_count && !component_ids) return fl_json_fail(&c->context,"Cannot retain local component addresses");
    for (size_t i=0;component_ids && i<entity->component_count;++i) component_ids[i]=entity->components[i].id;
    *out=s->entity_count;
    ((Fl_Entity *)s->entities)[s->entity_count]=*entity;
    ((Fl_Entity *)s->entities)[s->entity_count].id=id;
    ((Fl_Entity *)s->entities)[s->entity_count].parent=parent;
    c->entries[s->entity_count]=(Entry){.origin=origin,.scope=scope,.local=local,.components=component_ids};
    ++s->entity_count; fl_scene_storage_index_entity(s,s->entity_count-1); return true;
}

/* Account retained canonical JSON storage separately from source bytes and entity/scope budgets. */
static bool retain_document(Fl_Composition *c, const cJSON *document, unsigned depth)
{
    if (!document || depth>64) return fl_json_fail(&c->context,"Canonical scope document exceeds nesting budget");
    size_t bytes=sizeof(*document)+(document->string ? strlen(document->string)+1 : 0)+(document->valuestring ? strlen(document->valuestring)+1 : 0);
    if (bytes>FL_DOCUMENT_LIMIT-c->compiled_bytes) return fl_json_fail(&c->context,"Compiled scope document memory budget exceeded");
    c->compiled_bytes+=bytes;
    for (const cJSON *item=document->child;item;item=item->next) if (!retain_document(c,item,depth+1)) return false;
    return true;
}

/* Compile one hierarchy and recursively expand child instances; layers preserve outer override precedence. */
static bool expand(Fl_Composition *c, size_t scope, Fl_Origin origin, Fl_Id anchor, const Layer *layers, size_t layer_count)
{
    if (c->cancel && atomic_load_explicit(c->cancel,memory_order_relaxed)) return fl_json_fail(&c->context,"Evaluation cancelled");
    if (origin.depth>COMPOSITION_DEPTH) return fl_json_fail(&c->context,"Prototype nesting exceeds 32 scopes");
    size_t operation_capacity=0,operation_count=0;
    for (size_t l=0;l<layer_count;++l) operation_capacity+=(size_t)cJSON_GetArraySize(c->scopes[layers[l].scope].operations);
    const cJSON **operations=operation_capacity ? malloc(operation_capacity*sizeof(*operations)) : NULL;
    if (operation_capacity && !operations) return fl_json_fail(&c->context,"Cannot collect prototype overrides");
    for (size_t l=0;l<layer_count;++l) {
        Scope *owner=&c->scopes[layers[l].scope]; size_t op_index=0;
        for (const cJSON *op=owner->operations ? owner->operations->child : NULL;op;op=op->next,++op_index) {
            const cJSON *path=cJSON_GetObjectItemCaseSensitive(op,"path");
            if ((size_t)cJSON_GetArraySize(path)!=layers[l].offset) continue;
            bool matches=true; size_t k=0;
            for (const cJSON *part=path->child;part;part=part->next,++k) {uint64_t id; if (!fl_json_uint(&c->context,part,&id) || id!=layers[l].path[k]) {matches=false; break;}}
            if (!matches) continue;
            owner->seen[op_index]=true;
            operations[operation_count++]=op;
        }
    }
    bool applied=fl_prototype_apply_overrides(&c->context,&c->scopes[scope].prototype,operations,operation_count,&c->unresolved,c->cancel);
    free(operations); if (!applied) return false;
    Fl_Scene *local=&c->scopes[scope].prototype.scope; local->project=c->scene.project;
    if (scope && !fl_prototype_validate(&c->context,&c->scopes[scope].prototype,c->scene.project,c->cancel)) return false;
    c->scopes[scope].flat=local->entity_count ? malloc(local->entity_count*sizeof(size_t)) : NULL;
    if (local->entity_count && !c->scopes[scope].flat) return fl_json_fail(&c->context,"Cannot allocate scope provenance");
    size_t entity_count=local->entity_count;
    if (!scope) for (size_t i=0;i<entity_count;++i) {
        const Fl_Entity *entity=&local->entities[i];
        Fl_Origin current={.owner=entity->id,.entity=entity->id};
        if (!append(c,entity,entity->id,entity->parent,current,scope,i,&c->scopes[scope].flat[i])) return false;
    }
    for (size_t i=0;i<entity_count;++i) {
        if (c->cancel && atomic_load_explicit(c->cancel,memory_order_relaxed)) return fl_json_fail(&c->context,"Evaluation cancelled");
        local=&c->scopes[scope].prototype.scope;
        const Fl_Entity *entity=&local->entities[i];
        Fl_Id key=c->scopes[scope].key;
        Fl_Id id=scope ? identity(key,entity->id,1) : entity->id;
        Fl_Id parent=scope ? (entity->parent ? identity(key,entity->parent,1) : anchor) : entity->parent;
        Fl_Origin current=origin; current.entity=entity->id; current.generated=scope!=0;
        if (!scope) current.owner=entity->id;
        if (scope && !append(c,entity,id,parent,current,scope,i,&c->scopes[scope].flat[i])) return false;
        size_t component_count=entity->component_count;
        for (size_t component_index=0;component_index<component_count;++component_index) {
            local=&c->scopes[scope].prototype.scope; entity=&local->entities[i];
            const Fl_Component *component=&entity->components[component_index];
            Fl_PrototypeInstance instance={0}; Fl_Prototype generated={0};
            const char *source=NULL; size_t source_size=0; char *built_source=NULL;
            if (component->type==FL_COMPONENT_COMPOSITION) {
                Fl_LocalComposition owned;
                if (!fl_local_composition_decode(component,&owned)) return fl_json_fail(&c->context,"Invalid owned composition");
                source=owned.document; source_size=owned.document_size;
                instance.time=owned.time; instance.overrides=owned.overrides; instance.overrides_size=owned.overrides_size;
            } else if (component->type==FL_COMPONENT_PROTOTYPE) {
                if (!fl_prototype_instance_decode(component,&instance)) return fl_json_fail(&c->context,"Invalid prototype component");
                const Fl_Asset *asset=fl_project_asset(c->scene.project,instance.asset);
                if (!asset || asset->kind!=FL_ASSET_PROTOTYPE) return fl_json_fail(&c->context,"Missing Flow prototype resource");
                source=asset->source; source_size=(size_t)asset->size;
            } else if (component->type==FL_COMPONENT_SCRIPT) {
                Fl_Script script;
                if (!fl_script_decode(component,&script)) return fl_json_fail(&c->context,"Invalid Script component");
                if (!script.enabled) continue;
                if (!c->scopes[scope].builder) c->scopes[scope].builder=fl_script_runtime_create(local);
                if (!c->scopes[scope].builder) return fl_json_fail(&c->context,"Cannot allocate structural behavior evaluator");
                cJSON *document=NULL; char error[256]={0};
                if (!fl_script_runtime_build(c->scopes[scope].builder,local,i,component->id,c->cancel,&document,error,sizeof(error))) return fl_json_fail(&c->context,error[0] ? error : "Cannot build procedural hierarchy");
                if (!document) continue;
                built_source=cJSON_PrintUnformatted(document); cJSON_Delete(document);
                if (!built_source) return fl_json_fail(&c->context,"Cannot allocate generated hierarchy");
                source=built_source; source_size=strlen(source); instance.asset=script.script; instance.time=script.time; instance.overrides=script.overrides; instance.overrides_size=script.overrides_size;
            } else continue;
            bool allowed=origin.depth<COMPOSITION_DEPTH && c->count<COMPOSITION_SCOPES && source_size<=FL_DOCUMENT_LIMIT-c->compiled_bytes;
            for (size_t walk=scope;allowed && walk!=SIZE_MAX;walk=c->scopes[walk].parent) if (instance.asset && c->scopes[walk].asset==instance.asset) allowed=false;
            if (!allowed) {free(built_source); return fl_json_fail(&c->context,"Recursive composition dependency or scope/source budget exceeded");}
            bool parsed=component->type==FL_COMPONENT_COMPOSITION ? fl_owned_document_read(&c->context,source,source_size,&generated) : fl_prototype_read(&c->context,source,source_size,&generated); free(built_source);
            if (!parsed) return false;
            c->compiled_bytes+=source_size;
            size_t child=c->count++; Scope *scopes=realloc(c->scopes,c->count*sizeof(*scopes));
            if (!scopes) {--c->count; fl_prototype_clear(&generated); return fl_json_fail(&c->context,"Cannot allocate prototype scope");}
            c->scopes=scopes; scopes[child]=(Scope){.parent=scope,.owner=entity->id,.generator=component->id,.asset=instance.asset,.mapping=instance.time,.key=identity(key ? key : id,component->id,2)};
            scopes[child].prototype=generated;
            if (!scopes[child].mapping.duration) scopes[child].mapping.duration=(Fl_Tick)scopes[child].prototype.scope.duration;
            scopes[child].operations=fl_json_parse(&c->context,instance.overrides ? instance.overrides : "[]",instance.overrides ? instance.overrides_size : 2);
            if (!scopes[child].operations) return false;
            size_t n=(size_t)cJSON_GetArraySize(scopes[child].operations);
            scopes[child].seen=n ? calloc(n,sizeof(bool)) : NULL;
            if (n && !scopes[child].seen) return fl_json_fail(&c->context,"Cannot allocate override provenance");
            Layer inherited[COMPOSITION_DEPTH+1]; size_t inherited_count=1;
            inherited[0]=(Layer){.scope=child};
            for (size_t l=0;l<layer_count;++l) {
                inherited[inherited_count]=layers[l];
                inherited[inherited_count].path[inherited[inherited_count].offset++]=component->id;
                ++inherited_count;
            }
            Fl_Origin nested=current;
            if (scope) nested.path[nested.depth++]=component->id;
            else nested.component=component->id;
            if (!expand(c,child,nested,id,inherited,inherited_count)) return false;
        }
    }
    local=&c->scopes[scope].prototype.scope;
    c->scopes[scope].canonical=scope ? fl_prototype_write(&c->context,&c->scopes[scope].prototype) : fl_document_scene_write(&c->context,local);
    if (!c->scopes[scope].canonical || c->context.error[0] || !retain_document(c,c->scopes[scope].canonical,0)) return false;
    fl_script_runtime_destroy(c->scopes[scope].builder); c->scopes[scope].builder=NULL;
    if (scope) {
        /* Runtime addresses are scoped too; native tracks keep their scope-local beat clocks. */
        local=&c->scopes[scope].prototype.scope;
        local->id=c->scopes[scope].key; local->revision=c->scene.revision;
        for (size_t i=0;i<local->entity_count;++i) {
            Fl_Entity *entity=(Fl_Entity *)&local->entities[i]; Fl_Id old=entity->id;
            entity->id=identity(c->scopes[scope].key,old,1);
            if (entity->parent) entity->parent=identity(c->scopes[scope].key,entity->parent,1);
            for (size_t j=0;j<entity->component_count;++j) {
                Fl_Component *component=(Fl_Component *)&entity->components[j];
                component->id=identity(c->scopes[scope].key,component->id,3);
                if (component->type==FL_COMPONENT_SCRIPT) {
                    Fl_Script script;
                    if (!fl_script_decode(component,&script)) return fl_json_fail(&c->context,"Invalid scoped Script");
                    cJSON *bindings=fl_json_parse(&c->context,script.bindings,script.bindings_size);
                    if (!bindings) return false;
                    bool valid=true;
                    for (cJSON *binding=bindings->child;valid && binding;binding=binding->next) {
                        const char *type=cJSON_GetObjectItemCaseSensitive(binding,"type")->valuestring;
                        uint64_t target=0;
                        if (!strcmp(type,"entity") || !strcmp(type,"component")) {
                            valid=fl_json_uint(&c->context,cJSON_GetObjectItemCaseSensitive(binding,"id"),&target) &&
                                cJSON_ReplaceItemInObjectCaseSensitive(binding,"id",fl_json_u64(identity(c->scopes[scope].key,target,!strcmp(type,"entity") ? 1 : 3)));
                        }
                        if (valid && !strcmp(type,"component"))
                            valid=fl_json_uint(&c->context,cJSON_GetObjectItemCaseSensitive(binding,"entity"),&target) &&
                                cJSON_ReplaceItemInObjectCaseSensitive(binding,"entity",fl_json_u64(identity(c->scopes[scope].key,target,1)));
                    }
                    char *text=valid ? cJSON_PrintUnformatted(bindings) : NULL;
                    cJSON_Delete(bindings);
                    if (!text) return fl_json_fail(&c->context,"Cannot scope Script bindings");
                    script.bindings=text; script.bindings_size=strlen(text);
                    Fl_Component replacement={0}; valid=fl_script_encode(&script,&replacement); cJSON_free(text);
                    if (!valid) return fl_json_fail(&c->context,"Cannot encode scoped Script bindings");
                    replacement.id=component->id; fl_component_clear(component); *component=replacement;
                }
                if (component->type==FL_COMPONENT_ANIMATION) {
                    Fl_Animation *animation=(Fl_Animation *)component->data;
                    for (size_t k=0;k<animation->track_count;++k) ((Fl_Track *)&animation->tracks[k])->entity=entity->id;
                }
            }
        }
        fl_scene_storage_rebuild_index(local);
    }
    return true;
}

/* Sort exact IDs once at compilation; component addresses must remain unique across all scopes. */
static int compare_identity(const void *a, const void *b)
{
    Fl_Id left=*(const Fl_Id *)a,right=*(const Fl_Id *)b; return (left>right)-(left<right);
}

/* Reject generated/authored component collisions before any runtime can resolve ambiguous output owners. */
static bool component_identities_valid(Fl_Composition *c)
{
    size_t count=0;
    for (size_t i=0;i<c->scene.entity_count;++i) {
        size_t n=c->scene.entities[i].component_count;
        if (n>SIZE_MAX-count) return fl_json_fail(&c->context,"Composition component count overflow");
        count+=n;
    }
    if (count>SIZE_MAX/sizeof(Fl_Id)) return fl_json_fail(&c->context,"Composition component allocation overflow");
    Fl_Id *ids=count ? malloc(count*sizeof(*ids)) : NULL;
    if (count && !ids) return fl_json_fail(&c->context,"Cannot validate composition component identities");
    size_t at=0;
    for (size_t i=0;i<c->scene.entity_count;++i)
        for (size_t j=0;j<c->scene.entities[i].component_count;++j) ids[at++]=c->scene.entities[i].components[j].id;
    if (count>1) qsort(ids,count,sizeof(*ids),compare_identity);
    bool ok=true;
    for (size_t i=0;i<count;++i) if (!ids[i] || (i && ids[i]==ids[i-1])) {ok=false; break;}
    free(ids); return ok || fl_json_fail(&c->context,"Generated component identity collision");
}

Fl_Composition *fl_composition_create(const Fl_Scene *source, const atomic_bool *cancel, char *error, size_t capacity)
{
    if (error && capacity) error[0]=0;
    if (!source) return NULL;
    Fl_Composition *c=calloc(1,sizeof(*c)); if (!c) return NULL;
    c->cancel=cancel;
    c->scopes=calloc(1,sizeof(*c->scopes));
    if (!c->scopes) {free(c); return NULL;}
    c->count=1; c->scopes[0].parent=SIZE_MAX;
    cJSON *document=fl_document_scene_write(&c->context,source);
    bool ok=document && fl_document_scene_read(&c->context,document,&c->scopes[0].prototype.scope);
    cJSON_Delete(document);
    if (ok) ok=fl_scene_storage_reserve_index(&c->scopes[0].prototype.scope,source->entity_count);
    if (ok) {
        fl_scene_storage_rebuild_index(&c->scopes[0].prototype.scope);
        c->scene=*source; c->scene.entities=NULL; c->scene.entity_count=c->scene.entity_capacity=0;
        c->scene.identity_index=NULL; c->scene.identity_capacity=0;
        c->scene.name=c->scopes[0].prototype.scope.name;
        c->scene.beats=c->scopes[0].prototype.scope.beats; c->scene.clips=c->scopes[0].prototype.scope.clips;
        c->scene.phases=c->scopes[0].prototype.scope.phases;
        c->scene.voice=c->scopes[0].prototype.scope.voice; c->scene.language=c->scopes[0].prototype.scope.language;
        c->scene.voice_transcript=c->scopes[0].prototype.scope.voice_transcript;
        ok=expand(c,0,(Fl_Origin){0},0,NULL,0);
        if (ok) ok=component_identities_valid(c);
    }
    for (size_t i=1;ok && i<c->count;++i) {
        Scope *scope=&c->scopes[i]; size_t n=(size_t)cJSON_GetArraySize(scope->operations);
        for (size_t j=0;j<n;++j) c->unresolved+=!scope->seen[j];
    }
    if (!ok) {
        if (error && capacity) snprintf(error,capacity,"%s",c->context.error[0] ? c->context.error : "Cannot compile composition");
        fl_composition_destroy(c); return NULL;
    }
    c->cancel=NULL;
    return c;
}

void fl_composition_destroy(Fl_Composition *c)
{
    if (!c) return;
    for (size_t i=0;i<c->count;++i) {
        fl_script_runtime_destroy(c->scopes[i].builder);
        fl_scene_state_clear(&c->scopes[i].state); fl_prototype_clear(&c->scopes[i].prototype);
        free(c->scopes[i].flat); free(c->scopes[i].seen); cJSON_Delete(c->scopes[i].operations); cJSON_Delete(c->scopes[i].canonical);
    }
    for (size_t i=0;i<c->scene.entity_count;++i) free(c->entries[i].components);
    free((void *)c->scene.entities); free((void *)c->scene.identity_index);
    free(c->scopes); free(c->entries); free(c);
}
const Fl_Scene *fl_composition_scene(const Fl_Composition *c) {return c ? &c->scene : NULL;}
const Fl_Origin *fl_composition_origin(const Fl_Composition *c, size_t i) {return c && i<c->scene.entity_count ? &c->entries[i].origin : NULL;}
size_t fl_composition_unresolved(const Fl_Composition *c) {return c ? c->unresolved : 0;}

bool fl_composition_sample(Fl_Composition *c, Fl_Tick tick, Fl_SceneState *destination, const atomic_bool *cancel)
{
    for (size_t i=0;i<c->count;++i) {
        Scope *scope=&c->scopes[i]; scope->active=true; scope->tick=tick;
        if (i) {
            Scope *parent=&c->scopes[scope->parent]; Fl_TimeSample time;
            scope->active=parent->active && fl_time_mapping_sample(&parent->prototype.scope,&scope->mapping,parent->tick,&time) && time.active;
            scope->tick=scope->active ? time.tick : 0;
        }
        Fl_Scene *local=&scope->prototype.scope;
        if (!scope->active) {
            for (size_t j=0;j<local->entity_count;++j) {
                if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) return false;
                fl_entity_state_sample(local,j,scope->tick,&destination->entities[scope->flat[j]]);
                destination->entities[scope->flat[j]].opacity=0;
            }
            continue;
        }
        if (!fl_scene_state_update_native(&scope->state,local,scope->tick,cancel)) return false;
        for (size_t j=0;j<local->entity_count;++j) {
            destination->entities[scope->flat[j]]=scope->state.entities[j];
            if (!scope->active) destination->entities[scope->flat[j]].opacity=0;
        }
        for (size_t j=0;j<local->entity_count;++j) {
            const char *error=fl_script_runtime_error(scope->state.scripts,j);
            if (error[0]) {snprintf(destination->error,sizeof(destination->error),"%s",error); break;}
        }
    }
    return true;
}

bool fl_composition_scope(const Fl_Composition *c, size_t index, const Fl_Scene **scene,
                           size_t *parent, const Fl_TimeMapping **mapping)
{
    if (!c || index>=c->count || !scene || !parent || !mapping) return false;
    *scene=&c->scopes[index].prototype.scope; *parent=c->scopes[index].parent;
    *mapping=index ? &c->scopes[index].mapping : NULL; return true;
}

const Fl_ScriptRuntime *fl_composition_scripts(const Fl_Composition *composition)
{
    return composition && composition->count ? composition->scopes[0].state.scripts : NULL;
}

const char *fl_composition_entity_error(const Fl_Composition *composition, size_t index)
{
    if (!composition || index>=composition->scene.entity_count) return "";
    const Entry *entry=&composition->entries[index];
    return fl_script_runtime_error(composition->scopes[entry->scope].state.scripts,entry->local);
}

bool fl_composition_component_local(const Fl_Composition *c, size_t entity, Fl_Id runtime, Fl_Id *local)
{
    if (!c || !local || entity>=c->scene.entity_count) return false;
    const Fl_Entity *value=&c->scene.entities[entity];
    for (size_t i=0;i<value->component_count;++i) if (value->components[i].id==runtime) {
        *local=c->entries[entity].components ? c->entries[entity].components[i] : runtime; return true;
    }
    return false;
}

const Fl_ScriptRuntime *fl_composition_entity_scripts(const Fl_Composition *c, size_t entity, size_t *local)
{
    if (!c || entity>=c->scene.entity_count) return NULL;
    const Entry *entry=&c->entries[entity]; if (local) *local=entry->local;
    return c->scopes[entry->scope].state.scripts;
}

const Fl_Scene *fl_composition_entity_scope(const Fl_Composition *c, size_t entity)
{
    return c && entity<c->scene.entity_count ? &c->scopes[c->entries[entity].scope].prototype.scope : NULL;
}

cJSON *fl_composition_scope_document(const Fl_Composition *c, size_t index, Fl_Id *owner, Fl_Id *generator)
{
    if (!c || index>=c->count) return NULL;
    if (owner) *owner=c->scopes[index].owner;
    if (generator) *generator=c->scopes[index].generator;
    return cJSON_Duplicate(c->scopes[index].canonical,true);
}

cJSON *fl_composition_entity_document(const Fl_Composition *c, size_t entity)
{
    return c && entity<c->scene.entity_count ? fl_composition_scope_document(c,c->entries[entity].scope,NULL,NULL) : NULL;
}

bool fl_composition_entity_root_tick(const Fl_Composition *c, size_t entity, Fl_Tick local_tick, Fl_Tick reference_root, Fl_Tick *root_tick)
{
    if (!c || entity>=c->scene.entity_count || !root_tick || local_tick<0) return false;
    size_t chain[COMPOSITION_DEPTH],count=0,scope=c->entries[entity].scope;
    while (scope) {
        if (count==COMPOSITION_DEPTH) return false;
        chain[count++]=scope; scope=c->scopes[scope].parent;
    }
    int64_t cycles[COMPOSITION_DEPTH]; Fl_Tick tick=reference_root;
    for (size_t i=count;i>0;--i) {
        const Scope *child=&c->scopes[chain[i-1]]; Fl_TimeSample time;
        if (!fl_time_mapping_sample(&c->scopes[child->parent].prototype.scope,&child->mapping,tick,&time)) return false;
        cycles[i-1]=time.cycle; tick=time.tick;
    }
    tick=local_tick;
    for (size_t i=0;i<count;++i) {
        const Scope *child=&c->scopes[chain[i]];
        if (!fl_time_mapping_composition(&c->scopes[child->parent].prototype.scope,&child->mapping,tick,cycles[i],&tick)) return false;
    }
    *root_tick=tick; return true;
}
