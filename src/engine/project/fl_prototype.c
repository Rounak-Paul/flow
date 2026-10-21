#include "fl_prototype.h"
#include "fl_scene_storage.h"
#include "fl_timeline_internal.h"
#include "doc/fl_document.h"
#include "schema/fl_widgets.h"
#include "scripting/fl_script_runtime.h"
#include <stdlib.h>

/* Order exact local identifiers without subtraction overflow. */
static int compare_ids(const void *a, const void *b)
{
    Fl_Id x=*(const Fl_Id *)a,y=*(const Fl_Id *)b;
    return (x>y)-(x<y);
}

/* Check nonzero unique local identities against the document's monotonic cursor. */
static bool unique_ids(Fl_Id *ids, size_t count, Fl_Id next)
{
    if (count) qsort(ids,count,sizeof(*ids),compare_ids);
    for (size_t i=0;i<count;++i) if (!ids[i] || ids[i]>=next || (i && ids[i]==ids[i-1])) return false;
    return next!=0;
}

/* Validate local namespaces before building lookups; reject disconnected roots, missing parents and cycles. */
bool fl_prototype_local_valid(Fl_Json *c, Fl_Prototype *p)
{
    Fl_Scene *s=&p->scope;
    size_t count=fl_timeline_id_count(s), components=0;
    if (!s->entity_count || count>SIZE_MAX-s->entity_count) return fl_json_fail(c,"Prototype requires a rooted hierarchy");
    count+=s->entity_count;
    for (size_t i=0;i<s->entity_count;++i) {
        if (s->entities[i].component_count>SIZE_MAX-components) return fl_json_fail(c,"Prototype component limit exceeded");
        components+=s->entities[i].component_count;
    }
    if (count>SIZE_MAX/sizeof(Fl_Id) || components>SIZE_MAX/sizeof(Fl_Id)) return fl_json_fail(c,"Prototype identity limit exceeded");
    Fl_Id *ids=malloc(count*sizeof(*ids)), *instance_ids=components ? malloc(components*sizeof(*instance_ids)) : NULL;
    if (!ids || (components && !instance_ids)) {free(ids); free(instance_ids); return fl_json_fail(c,"Cannot allocate prototype identities");}
    fl_timeline_ids(s,ids); size_t at=count-s->entity_count, component=0;
    bool valid=true;
    for (size_t i=0;i<s->entity_count;++i) {
        const Fl_Entity *e=&s->entities[i]; ids[at++]=e->id;
        for (size_t j=0;j<e->component_count;++j) {
            instance_ids[component++]=e->components[j].id;
            if (!fl_widget_component_valid(&e->components[j])) valid=false;
        }
    }
    valid=valid && unique_ids(ids,count,p->next_id) && unique_ids(instance_ids,components,p->next_component_id);
    free(ids); free(instance_ids);
    if (!valid || !fl_scene_storage_reserve_index(s,s->entity_count)) return fl_json_fail(c,"Invalid prototype identities or components");
    fl_scene_storage_rebuild_index(s);
    const Fl_Entity *root=fl_scene_entity(s,p->root);
    if (!root || root->parent) return fl_json_fail(c,"Invalid prototype root");
    unsigned char *state=calloc(s->entity_count,1);
    if (!state) return fl_json_fail(c,"Cannot allocate prototype hierarchy validation");
    for (size_t i=0;i<s->entity_count && valid;++i) {
        const Fl_Entity *e=&s->entities[i];
        while (e && !state[e-s->entities]) {
            state[e-s->entities]=1;
            if (e==root) {e=NULL; break;}
            e=fl_scene_entity(s,e->parent);
            if (!e) valid=false;
        }
        if (e && state[e-s->entities]==1) valid=false;
        e=&s->entities[i];
        while (e && state[e-s->entities]==1) {state[e-s->entities]=2; e=fl_scene_entity(s,e->parent);}
    }
    free(state);
    if (!valid) return fl_json_fail(c,"Prototype hierarchy is disconnected or cyclic");
    /* Audio resource validity is checked after the entire asset catalog has been assembled. */
    Fl_Scene native=*s; native.clips=NULL; native.clip_count=0;
    return fl_timeline_valid(NULL,&native,p->next_id) || fl_json_fail(c,"Invalid prototype timeline");
}

bool fl_prototype_read(Fl_Json *c, const char *source, size_t size, Fl_Prototype *out)
{
    if (!out || !source || !size || size>FL_PROTOTYPE_SOURCE_LIMIT) return fl_json_fail(c,"Invalid prototype source size");
    Fl_Prototype value={0};
    cJSON *root=fl_json_parse(c,source,size); uint64_t version=0;
    bool ok=root && fl_json_object(c,root,"version root next_id next_component_id duration entities beats clips") &&
        fl_json_uint(c,fl_json_member(c,root,"version",0),&version) && version==1 &&
        fl_json_uint(c,fl_json_member(c,root,"root",0),&value.root) &&
        fl_json_uint(c,fl_json_member(c,root,"next_id",0),&value.next_id) &&
        fl_json_uint(c,fl_json_member(c,root,"next_component_id",0),&value.next_component_id) &&
        fl_json_uint(c,fl_json_member(c,root,"duration",0),&value.scope.duration) &&
        value.next_id<=INT64_MAX && value.next_component_id<=INT64_MAX && value.scope.duration && value.scope.duration<=INT64_MAX && fl_document_content_read(c,root,&value.scope) && fl_prototype_local_valid(c,&value);
    cJSON_Delete(root);
    if (!ok) {fl_prototype_clear(&value); return fl_json_fail(c,"Invalid Flow prototype document");}
    *out=value; return true;
}

cJSON *fl_prototype_write(Fl_Json *c, const Fl_Prototype *p)
{
    cJSON *root=cJSON_CreateObject();
    fl_json_add(c,root,"version",fl_json_u64(1)); fl_json_add(c,root,"root",fl_json_u64(p->root));
    fl_json_add(c,root,"next_id",fl_json_u64(p->next_id)); fl_json_add(c,root,"next_component_id",fl_json_u64(p->next_component_id));
    fl_json_add(c,root,"duration",fl_json_u64(p->scope.duration)); fl_document_content_write(c,root,&p->scope);
    return root;
}

/* Resolve a direct or override-provided resource against the final project catalog. */
static bool reference_valid(Fl_Id id, Fl_AssetKind kind, void *data)
{
    const Fl_Asset *asset=fl_project_asset(data,id); return asset && (!kind || asset->kind==kind);
}

bool fl_prototype_validate(Fl_Json *c, Fl_Prototype *p, const Fl_Project *project, const atomic_bool *cancel)
{
    p->scope.project=project;
    for (size_t i=0;i<p->scope.entity_count;++i) {
        const Fl_Entity *e=&p->scope.entities[i];
        for (size_t j=0;j<e->component_count;++j) {
            if (!fl_component_visit_assets(&e->components[j],reference_valid,(void *)project)) return fl_json_fail(c,"Missing or incompatible prototype resource");
        }
    }
    if (!fl_timeline_valid(project,&p->scope,p->next_id)) return fl_json_fail(c,"Invalid prototype media timeline");
    char error[256]={0};
    return fl_script_validate_scene(&p->scope,cancel,error,sizeof(error)) || fl_json_fail(c,error);
}

void fl_prototype_clear(Fl_Prototype *p)
{
    if (p) {fl_scene_storage_clear(&p->scope); *p=(Fl_Prototype){0};}
}
