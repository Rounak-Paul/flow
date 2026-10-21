#include "fl_prototype.h"
#include "fl_scene_storage.h"
#include "components/fl_prototype_instance.h"
#include "doc/fl_document.h"
#include <stdlib.h>
#include <string.h>

/* Find a record by its exact local ID without depending on authored array order. */
static cJSON *record(Fl_Json *c, cJSON *array, Fl_Id id)
{
    for (cJSON *v=array->child;v;v=v->next) {uint64_t candidate; if (!fl_json_uint(c,fl_json_member(c,v,"id",0),&candidate)) return NULL; if (candidate==id) return v;}
    return NULL;
}

/* Apply only named fields; schema validation after merging rejects unknown or mistyped values. */
static bool patch(Fl_Json *c, cJSON *target, const cJSON *value)
{
    for (const cJSON *v=value->child;v;v=v->next) {
        if (!strcmp(v->string,"id") || !strcmp(v->string,"type") || !strcmp(v->string,"version")) return fl_json_fail(c,"Overrides cannot change an existing identity or schema");
        cJSON *copy=cJSON_Duplicate(v,true);
        if (!copy) return fl_json_fail(c,"Cannot allocate prototype override");
        if (cJSON_HasObjectItem(target,v->string)) {
            if (!cJSON_ReplaceItemInObjectCaseSensitive(target,v->string,copy)) {cJSON_Delete(copy); return fl_json_fail(c,"Cannot replace prototype field");}
        } else if (!fl_json_add(c,target,v->string,copy)) return false;
    }
    return true;
}

typedef struct Removal {Fl_Id id,parent; cJSON *node; unsigned char state;} Removal;
/* Sort identity records without subtraction overflow. */
static int compare_removal(const void *a, const void *b)
{
    const Removal *x=a,*y=b; return (x->id>y->id)-(x->id<y->id);
}
/* Resolve a parent in a sorted identity table. */
static size_t removal_find(const Removal *items, size_t count, Fl_Id id)
{
    size_t lo=0,hi=count;
    while (lo<hi) {size_t mid=lo+(hi-lo)/2; if (items[mid].id<id) lo=mid+1; else hi=mid;}
    return lo<count && items[lo].id==id ? lo : SIZE_MAX;
}
/* Remove a subtree using bounded graph traversal, including children preceding their parents in source order. */
static bool remove_subtree(Fl_Json *c, cJSON *entities, Fl_Id id)
{
    size_t count=(size_t)cJSON_GetArraySize(entities);
    Removal *items=calloc(count,sizeof(*items)); size_t *path=malloc(count*sizeof(*path));
    if (count && (!items || !path)) {free(items); free(path); return fl_json_fail(c,"Cannot allocate override hierarchy");}
    size_t at=0; bool ok=true;
    for (cJSON *v=entities->child;v;v=v->next,++at) {
        items[at].node=v;
        if (!fl_json_uint(c,fl_json_member(c,v,"id",0),&items[at].id) || !fl_json_uint(c,fl_json_member(c,v,"parent",0),&items[at].parent)) {ok=false; break;}
        if (items[at].id==id) items[at].state=3;
    }
    if (ok) {
        qsort(items,count,sizeof(*items),compare_removal);
        for (size_t i=0;i<count;++i) if (!items[i].state) {
            size_t depth=0,current=i;
            while (current!=SIZE_MAX && !items[current].state) {items[current].state=1; path[depth++]=current; current=removal_find(items,count,items[current].parent);}
            unsigned char result=current!=SIZE_MAX && items[current].state==3 ? 3 : 2;
            while (depth) items[path[--depth]].state=result;
        }
        for (size_t i=0;i<count;++i) if (items[i].state==3) cJSON_Delete(cJSON_DetachItemViaPointer(entities,items[i].node));
    }
    free(items); free(path); return ok;
}

/* Mutate an unpublished JSON candidate; the complete scope is validated once after the ordered batch. */
static bool apply_override(Fl_Json *c, const Fl_Prototype *prototype, cJSON *root, const cJSON *op, size_t *unresolved)
{
    uint64_t entity=0; if (!fl_json_uint(c,fl_json_member(c,op,"entity",0),&entity)) return false;
    const cJSON *kind=fl_json_member(c,op,"op",cJSON_String), *value=cJSON_GetObjectItemCaseSensitive(op,"value");
    if (!kind) return false;
    cJSON *entities=cJSON_GetObjectItemCaseSensitive(root,"entities");
    if (!entities || c->error[0]) return false;
    cJSON *target=record(c,entities,entity); bool ok=true;
    if (!strcmp(kind->valuestring,"entity.add")) {
        uint64_t id=0;
        if (target || !(entity>>63) || !fl_json_uint(c,fl_json_member(c,value,"id",0),&id) || id!=entity) ok=fl_json_fail(c,"Added prototype entities require unique extension IDs (high bit set)");
        else {
            const cJSON *components=fl_json_member(c,value,"components",cJSON_Array);
            ok=components!=NULL;
            for (const cJSON *v=components ? components->child : NULL;ok && v;v=v->next) {
                uint64_t component_id; ok=fl_json_uint(c,fl_json_member(c,v,"id",0),&component_id) && (component_id>>63);
            }
            if (!ok) fl_json_fail(c,"Added entity components require extension IDs (high bit set)");
            else ok=fl_json_add(c,entities,NULL,cJSON_Duplicate(value,true));
        }
    } else if (!target) {++*unresolved; return true;}
    else if (!strcmp(kind->valuestring,"entity.remove")) {
        ok=entity!=prototype->root && remove_subtree(c,entities,entity);
        if (!ok) fl_json_fail(c,"Cannot remove a prototype root");
    } else if (!strcmp(kind->valuestring,"entity.patch")) {
        ok=fl_json_object(c,value,"name visible parent") && patch(c,target,value);
    } else {
        cJSON *components=cJSON_GetObjectItemCaseSensitive(target,"components"); uint64_t id=0;
        if (!strcmp(kind->valuestring,"component.set")) {
            ok=fl_json_uint(c,fl_json_member(c,value,"id",0),&id) && id;
        } else ok=fl_json_uint(c,fl_json_member(c,op,"component",0),&id);
        cJSON *component=ok ? record(c,components,id) : NULL;
        if (ok && !strcmp(kind->valuestring,"component.set")) {
            if (!component && !(id>>63)) ok=fl_json_fail(c,"Added components require extension IDs (high bit set)");
            else if (component && !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(component,"type"),cJSON_GetObjectItemCaseSensitive(value,"type"),true)) ok=fl_json_fail(c,"Cannot change an existing component schema");
            else {
                cJSON *copy=cJSON_Duplicate(value,true);
                if (component) {if (!copy || !cJSON_ReplaceItemViaPointer(components,component,copy)) {cJSON_Delete(copy); ok=fl_json_fail(c,"Cannot replace prototype component");}}
                else ok=fl_json_add(c,components,NULL,copy);
            }
        } else if (ok && !component) {++*unresolved; return true;}
        else if (ok && !strcmp(kind->valuestring,"component.remove")) cJSON_Delete(cJSON_DetachItemViaPointer(components,component));
        else if (ok) ok=patch(c,component,value);
    }
    return ok;
}

bool fl_prototype_apply_overrides(Fl_Json *c, Fl_Prototype *prototype, const cJSON *const *operations,
                                  size_t count, size_t *unresolved, const atomic_bool *cancel)
{
    if (!count) return true;
    cJSON *root=fl_prototype_write(c,prototype);
    bool ok=root && !c->error[0]; size_t missing=0;
    for (size_t i=0;ok && i<count;++i) {
        if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) ok=fl_json_fail(c,"Evaluation cancelled");
        else ok=apply_override(c,prototype,root,operations[i],&missing);
    }
    Fl_Prototype next={.root=prototype->root,.next_id=UINT64_MAX,.next_component_id=UINT64_MAX,.scope={.duration=prototype->scope.duration,.project=prototype->scope.project}};
    if (ok) ok=fl_document_content_read(c,root,&next.scope) && fl_prototype_local_valid(c,&next);
    cJSON_Delete(root);
    if (!ok) {fl_prototype_clear(&next); return false;}
    fl_prototype_clear(prototype); *prototype=next; *unresolved+=missing; return true;
}
