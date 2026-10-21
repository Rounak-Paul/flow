#include "fl_bake.h"
#include "fl_prototype.h"
#include "fl_scene_storage.h"
#include "fl_timeline_internal.h"
#include "components/fl_local_composition.h"
#include "components/fl_sampled_animation.h"
#include "doc/fl_document.h"
#include "time/fl_composition.h"
#include "time/fl_scene_state.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAKE_MEMORY (64u*1024u*1024u)
#define BAKE_POSES 1000000u

struct Fl_Bake {
    Fl_JobSystem *jobs;
    bool own_jobs;
    Fl_JobHandle job;
    Fl_Project *view;
    Fl_Id scene;
    uint64_t generation, identity;
    Fl_BakeOptions options;
    atomic_bool cancel;
    atomic_int status;
    char error[256];
    char *document;
    size_t document_size;
};
typedef struct BakedScope {
    cJSON *document;
    size_t parent;
    Fl_Id owner, generator;
    Fl_TimeMapping mapping;
} BakedScope;

/* Record a worker-local diagnostic; status publication supplies the release fence. */
static bool bake_fail(Fl_Bake *b, const char *message)
{
    snprintf(b->error,sizeof(b->error),"%s",message); return false;
}

/* Test every captured interior point against the exact native linear/held interpolation. */
static bool segment_valid(const Fl_SampledKey *keys, size_t a, size_t z, double tolerance, const atomic_bool *cancel)
{
    for (size_t i=a+1;i<z;++i) {
        if (atomic_load_explicit(cancel,memory_order_relaxed)) return false;
        if (keys[i].text_size!=keys[a].text_size || (keys[i].text_size && memcmp(keys[i].text,keys[a].text,keys[i].text_size))) return false;
        double t=(double)(((long double)keys[i].tick-keys[a].tick)/((long double)keys[z].tick-keys[a].tick));
        for (int p=0;p<FL_PROP_COUNT;++p) for (unsigned c=0;c<4;++c)
            if (fabs(keys[a].values[p][c]+(keys[z].values[p][c]-keys[a].values[p][c])*t-keys[i].values[p][c])>tolerance) return false;
    }
    return true;
}

/* Copy a reduced pose sequence without invalidating source text ownership. */
static bool encode_reduced(Fl_Bake *b, const Fl_SampledKey *keys, size_t count, bool text, Fl_Component *out)
{
    Fl_SampledKey *reduced=malloc(count*sizeof(*reduced)); if (!reduced) return false;
    size_t n=0,anchor=0; reduced[n++]=keys[0];
    while (anchor+1<count && !atomic_load_explicit(&b->cancel,memory_order_relaxed)) {
        size_t end=anchor+1,distance=2;
        while (distance<count-anchor) {
            size_t candidate=anchor+distance;
            if (!segment_valid(keys,anchor,candidate,b->options.tolerance,&b->cancel)) break;
            end=candidate;
            if (candidate==count-1) break;
            distance=distance>(count-1-anchor)/2 ? count-1-anchor : distance*2;
        }
        reduced[n++]=keys[end]; anchor=end;
    }
    bool ok=!atomic_load_explicit(&b->cancel,memory_order_relaxed) && fl_sampled_animation_encode(reduced,n,text,out);
    free(reduced); return ok;
}

/* Find a typed native JSON record by its exact local identity. */
static cJSON *find_record(cJSON *array, Fl_Id id)
{
    Fl_Json c={0};
    for (cJSON *item=array ? array->child : NULL;item;item=item->next) {
        uint64_t found=0; if (fl_json_uint(&c,cJSON_GetObjectItemCaseSensitive(item,"id"),&found) && found==id) return item;
    }
    return NULL;
}

/* Capture one native scope using its runtime identities, storing data at its original local addresses. */
static bool sample_scope(Fl_Bake *b, const Fl_Scene *scene, BakedScope *scope, bool root, size_t *pose_budget, size_t *byte_budget)
{
    Fl_Tick begin=root ? b->options.start : 0, end=root ? b->options.end : (Fl_Tick)fl_scene_length(scene);
    uint64_t span=(uint64_t)(end-begin), step=(uint64_t)b->options.step;
    uint64_t count64=span/step+1+(span%step!=0);
    if (count64>FL_SAMPLED_ANIMATION_KEYS || scene->entity_count>BAKE_POSES/count64 || scene->entity_count*count64>BAKE_POSES-*pose_budget) return bake_fail(b,"Bake exceeds the pose budget; increase maximum step or shorten the interval");
    size_t count=(size_t)count64,total=count*scene->entity_count;
    if (total>BAKE_MEMORY/sizeof(Fl_SampledKey)) return bake_fail(b,"Bake scope exceeds the capture memory budget");
    *pose_budget+=total;
    Fl_SampledKey *keys=calloc(total ? total : 1,sizeof(*keys)); Fl_SceneState state={0};
    if (!keys) return bake_fail(b,"Cannot allocate bake capture");
    size_t strings=0; bool ok=true;
    for (size_t k=0;k<count && ok;++k) {
        Fl_Tick tick=k==count-1 ? end : begin+(Fl_Tick)(k*step);
        ok=fl_scene_state_update_native(&state,scene,tick,&b->cancel);
        for (size_t i=0;i<scene->entity_count && ok;++i) {
            const char *error=fl_scene_state_entity_error(&state,i);
            if (error[0]) {ok=bake_fail(b,error); break;}
            Fl_SampledKey *key=&keys[i*count+k]; const Fl_EntityState *pose=&state.entities[i];
            key->tick=tick; fl_entity_state_values(pose,key->values);
            const Fl_Component *native_text=fl_entity_component(&scene->entities[i],FL_COMPONENT_TEXT);
            const char *text=pose->text_override ? pose->text : native_text ? (const char *)native_text->data : "";
            size_t size=pose->text_override ? pose->text_size : native_text ? native_text->size : 0;
            if (size>=BAKE_MEMORY-strings || total*sizeof(*keys)>BAKE_MEMORY-strings-size-1) {ok=bake_fail(b,"Bake text exceeds the capture memory budget"); break;}
            char *copy=malloc(size+1); if (!copy) {ok=false; break;}
            if (size) memcpy(copy,text,size); copy[size]=0; key->text=copy; key->text_size=size; strings+=size+1;
        }
    }
    cJSON *entities=cJSON_GetObjectItemCaseSensitive(scope->document,"entities"); size_t index=0;
    uint64_t allocated=0,retained=0; Fl_Json cursor_context={0};
    for (cJSON *e=entities ? entities->child : NULL;e;e=e->next) {
        cJSON *components=cJSON_GetObjectItemCaseSensitive(e,"components");
        for (cJSON *component=components ? components->child : NULL;component;component=component->next) {
            uint64_t id=0; if (!fl_json_uint(&cursor_context,cJSON_GetObjectItemCaseSensitive(component,"id"),&id)) ok=false;
            if (id>allocated) allocated=id;
        }
    }
    if (!fl_json_uint(&cursor_context,cJSON_GetObjectItemCaseSensitive(scope->document,"next_component_id"),&retained) || allocated>=UINT64_MAX-1) ok=false;
    ++allocated;
    if (retained!=UINT64_MAX && retained>allocated) allocated=retained;
    for (cJSON *entity=entities ? entities->child : NULL;entity && ok && index<scene->entity_count;entity=entity->next,++index) {
        cJSON *components=cJSON_GetObjectItemCaseSensitive(entity,"components"); Fl_Id sampled_id=0;
        for (cJSON *component=components ? components->child : NULL,*next;component;component=next) {
            next=component->next; const cJSON *type=cJSON_GetObjectItemCaseSensitive(component,"type");
            if (cJSON_IsString(type) && (!strcmp(type->valuestring,"animation") || !strcmp(type->valuestring,"sampled"))) {
                Fl_Json c={0}; if (!sampled_id) fl_json_uint(&c,cJSON_GetObjectItemCaseSensitive(component,"id"),&sampled_id);
                cJSON_Delete(cJSON_DetachItemViaPointer(components,component));
            }
        }
        if (!sampled_id) {
            if (allocated>=UINT64_MAX-1) {ok=false; break;}
            sampled_id=allocated++;
            ok=cJSON_ReplaceItemInObjectCaseSensitive(scope->document,"next_component_id",fl_json_u64(retained==UINT64_MAX ? retained : allocated));
        }
        Fl_Component clip={0}; bool text=fl_entity_component(&scene->entities[index],FL_COMPONENT_TEXT)!=NULL;
        if (ok) ok=encode_reduced(b,&keys[index*count],count,text,&clip);
        Fl_Json c={0}; clip.id=sampled_id;
        cJSON *encoded=ok ? fl_document_component_write(&c,&clip) : NULL;
        if (clip.size>FL_DOCUMENT_LIMIT-*byte_budget) ok=bake_fail(b,"Bake exceeds native document budget");
        else *byte_budget+=clip.size;
        if (ok) ok=encoded && fl_json_add(&c,components,NULL,encoded); else cJSON_Delete(encoded);
        fl_component_clear(&clip);
    }
    for (size_t i=0;i<total;++i) free((void *)keys[i].text);
    free(keys); fl_scene_state_clear(&state); return ok;
}

/* Convert the outer scene's independent roots into one identity grouping scope without changing media clocks. */
static bool root_document(Fl_Json *c, cJSON *document)
{
    Fl_Scene scene={0}; bool ok=fl_document_scene_read(c,document,&scene);
    Fl_Id max=0,component=0; size_t n=fl_timeline_id_count(&scene);
    Fl_Id *ids=n ? malloc(n*sizeof(*ids)) : NULL;
    if (n && !ids) ok=false;
    if (ok) {fl_timeline_ids(&scene,ids); for (size_t i=0;i<n;++i) if (ids[i]>max) max=ids[i];}
    free(ids);
    for (size_t i=0;i<scene.entity_count;++i) {
        if (scene.entities[i].id>max) max=scene.entities[i].id;
        for (size_t j=0;j<scene.entities[i].component_count;++j) if (scene.entities[i].components[j].id>component) component=scene.entities[i].components[j].id;
    }
    if (max>=UINT64_MAX-2 || component>=UINT64_MAX-2) ok=false;
    cJSON *entities=cJSON_GetObjectItemCaseSensitive(document,"entities");
    for (cJSON *entity=entities ? entities->child : NULL;entity && ok;entity=entity->next) {
        uint64_t parent; ok=fl_json_uint(c,cJSON_GetObjectItemCaseSensitive(entity,"parent"),&parent);
        if (ok && !parent) ok=cJSON_ReplaceItemInObjectCaseSensitive(entity,"parent",fl_json_u64(max+1));
    }
    unsigned char bytes[56]; fl_transform_encode(&(Fl_Transform){.scale_x=1,.scale_y=1},bytes);
    Fl_Component transform={.id=component+1,.type=FL_COMPONENT_TRANSFORM,.version=1,.data=bytes,.size=56};
    cJSON *group=cJSON_CreateObject(), *components=cJSON_CreateArray();
    if (ok) ok=group && components && fl_json_add(c,group,"id",fl_json_u64(max+1)) && fl_json_add(c,group,"parent",fl_json_u64(0)) && fl_json_add(c,group,"name",cJSON_CreateString("Baked content")) && fl_json_add(c,group,"visible",cJSON_CreateBool(true));
    if (ok) {ok=fl_json_add(c,group,"components",components); components=NULL;}
    if (ok) ok=fl_json_add(c,cJSON_GetObjectItemCaseSensitive(group,"components"),NULL,fl_document_component_write(c,&transform));
    if (ok) {ok=fl_json_add(c,entities,NULL,group); group=NULL;}
    cJSON_Delete(group); cJSON_Delete(components);
    static const char *fields[]={"id","name","revision","width","height","aspect_width","aspect_height","camera_x","camera_y","background"};
    for (size_t i=0;i<sizeof(fields)/sizeof(*fields);++i) cJSON_DeleteItemFromObjectCaseSensitive(document,fields[i]);
    if (ok) ok=fl_json_add(c,document,"root",fl_json_u64(max+1)) && fl_json_add(c,document,"next_id",fl_json_u64(max+2)) && fl_json_add(c,document,"next_component_id",fl_json_u64(component+2));
    fl_scene_storage_clear(&scene); return ok;
}

/* Worker owns only immutable input and unpublished output; no editor/project mutation occurs here. */
static void bake_run(void *data)
{
    Fl_Bake *b=data; const Fl_Scene *source=fl_project_scene(b->view,b->scene);
    Fl_Composition *composition=fl_composition_create(source,&b->cancel,b->error,sizeof(b->error));
    BakedScope *scopes=NULL; size_t count=0,poses=0,bytes=0; bool ok=composition!=NULL;
    if (ok && fl_composition_unresolved(composition)) ok=bake_fail(b,"Resolve orphaned scope overrides before baking");
    const Fl_Scene *local; size_t parent; const Fl_TimeMapping *mapping;
    while (ok && fl_composition_scope(composition,count,&local,&parent,&mapping)) {
        BakedScope *grown=realloc(scopes,(count+1)*sizeof(*grown)); if (!grown) {ok=false; break;} scopes=grown;
        BakedScope *scope=&scopes[count]; *scope=(BakedScope){.parent=parent,.mapping=mapping ? *mapping : fl_time_mapping_default()};
        scope->document=fl_composition_scope_document(composition,count,&scope->owner,&scope->generator); ++count;
        ok=scope->document && (parent!=SIZE_MAX || root_document(&(Fl_Json){0},scope->document));
        if (ok) ok=sample_scope(b,local,scope,parent==SIZE_MAX,&poses,&bytes);
    }
    for (size_t i=count;ok && i>0;--i) {
        BakedScope *scope=&scopes[i-1];
        cJSON *entities=cJSON_GetObjectItemCaseSensitive(scope->document,"entities");
        for (cJSON *entity=entities ? entities->child : NULL;entity;entity=entity->next) {
            cJSON *components=cJSON_GetObjectItemCaseSensitive(entity,"components");
            for (cJSON *component=components ? components->child : NULL,*next;component;component=next) {
                next=component->next; const cJSON *type=cJSON_GetObjectItemCaseSensitive(component,"type");
                if (cJSON_IsString(type) && !strcmp(type->valuestring,"script")) cJSON_Delete(cJSON_DetachItemViaPointer(components,component));
            }
        }
        if (scope->parent!=SIZE_MAX) {
            char *text=cJSON_PrintUnformatted(scope->document); Fl_Component owned={0};
            ok=text && fl_local_composition_encode(&(Fl_LocalComposition){.time=scope->mapping,.document=text,.document_size=text ? strlen(text) : 0},&owned); free(text);
            cJSON *entity=find_record(cJSON_GetObjectItemCaseSensitive(scopes[scope->parent].document,"entities"),scope->owner);
            cJSON *components=cJSON_GetObjectItemCaseSensitive(entity,"components");
            cJSON *old=find_record(components,scope->generator); owned.id=scope->generator; Fl_Json c={0};
            cJSON *replacement=ok ? fl_document_component_write(&c,&owned) : NULL;
            if (ok && replacement) {
                if (old) {ok=cJSON_ReplaceItemViaPointer(components,old,replacement); if (!ok) cJSON_Delete(replacement);}
                else ok=fl_json_add(&c,components,NULL,replacement);
            } else {cJSON_Delete(replacement); ok=false;}
            fl_component_clear(&owned);
        }
    }
    if (ok) {
        b->document=cJSON_PrintUnformatted(scopes[0].document); b->document_size=b->document ? strlen(b->document) : 0;
        Fl_Json c={0}; Fl_Prototype check={0};
        ok=b->document && fl_owned_document_read(&c,b->document,b->document_size,&check); fl_prototype_clear(&check);
        if (!ok) bake_fail(b,c.error);
    }
    for (size_t i=0;i<count;++i) cJSON_Delete(scopes[i].document);
    free(scopes); fl_composition_destroy(composition);
    if (!ok && !b->error[0]) bake_fail(b,"Bake failed: invalid native data or resource budget exhausted");
    atomic_store_explicit(&b->status,atomic_load_explicit(&b->cancel,memory_order_relaxed) ? FL_BAKE_CANCELLED : ok ? FL_BAKE_READY : FL_BAKE_FAILED,memory_order_release);
}

Fl_Bake *fl_bake_create(Fl_JobSystem *jobs)
{
    Fl_Bake *b=calloc(1,sizeof(*b)); if (!b) return NULL;
    b->jobs=jobs ? jobs : fl_jobs_create(&(Fl_JobSystemDesc){.worker_count=1}); b->own_jobs=!jobs;
    atomic_init(&b->cancel,false); atomic_init(&b->status,FL_BAKE_IDLE);
    if (!b->jobs) {free(b); return NULL;} return b;
}

void fl_bake_cancel(Fl_Bake *b)
{
    if (!b) return;
    atomic_store_explicit(&b->cancel,true,memory_order_relaxed);
    int ready=FL_BAKE_READY;
    atomic_compare_exchange_strong_explicit(&b->status,&ready,FL_BAKE_CANCELLED,memory_order_acq_rel,memory_order_acquire);
}
Fl_BakeStatus fl_bake_status(const Fl_Bake *b) {return b ? (Fl_BakeStatus)atomic_load_explicit(&b->status,memory_order_acquire) : FL_BAKE_IDLE;}
const char *fl_bake_error(const Fl_Bake *b) {return b && fl_bake_status(b)!=FL_BAKE_RUNNING ? b->error : "";}

void fl_bake_destroy(Fl_Bake *b)
{
    if (!b) return;
    fl_bake_cancel(b); fl_job_wait(b->jobs,b->job); fl_project_destroy(b->view); free(b->document);
    if (b->own_jobs) fl_jobs_destroy(b->jobs); free(b);
}

bool fl_bake_start(Fl_Bake *b, Fl_Project *project, Fl_Id scene, const Fl_BakeOptions *options)
{
    const Fl_Scene *source=fl_project_scene(project,scene);
    if (!b || !source || !options || fl_project_edit_active(project) || fl_bake_status(b)==FL_BAKE_RUNNING || options->start<0 || options->end<options->start || options->end>(Fl_Tick)fl_scene_length(source) || options->step<=0 || !isfinite(options->tolerance) || options->tolerance<0) return false;
    fl_job_wait(b->jobs,b->job); fl_project_destroy(b->view); b->view=NULL; free(b->document); b->document=NULL; b->document_size=0; b->error[0]=0;
    b->view=fl_project_read_view(project); if (!b->view) return false;
    b->scene=scene; b->generation=fl_project_generation(project); b->identity=fl_project_identity(project); b->options=*options;
    atomic_store_explicit(&b->cancel,false,memory_order_relaxed); atomic_store_explicit(&b->status,FL_BAKE_RUNNING,memory_order_release);
    b->job=fl_job_submit(b->jobs,&(Fl_JobDesc){.fn=bake_run,.data=b,.priority=FL_JOB_PRIORITY_LOW,.notify=fl_jobs_done_event(b->jobs)!=FL_EVENT_TYPE_INVALID});
    if (!b->job.generation) {bake_fail(b,"Cannot schedule bake"); atomic_store_explicit(&b->status,FL_BAKE_FAILED,memory_order_release); return false;} return true;
}

bool fl_bake_apply(Fl_Bake *b, Fl_Project *project, Fl_Id *owner)
{
    if (!b || fl_bake_status(b)!=FL_BAKE_READY || atomic_load_explicit(&b->cancel,memory_order_relaxed) || fl_project_edit_active(project) || fl_project_identity(project)!=b->identity || fl_project_generation(project)!=b->generation) return false;
    const Fl_Scene *source=fl_project_scene(b->view,b->scene); if (!source) return false;
    Fl_Component component={0};
    if (!fl_local_composition_encode(&(Fl_LocalComposition){.time=fl_time_mapping_default(),.document=b->document,.document_size=b->document_size},&component)) return false;
    bool ok=fl_project_edit_begin(project,"Bake scene",false); if (!ok) {fl_component_clear(&component); return false;}
    Fl_Id id=0;
    ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=b->scene,.name="Baked content",.components=&component,.component_count=1},&id);
    for (size_t i=0;ok && i<source->entity_count;++i) if (!source->entities[i].parent)
        ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=b->scene,.entity=source->entities[i].id},NULL);
    for (size_t i=0;ok && i<source->clip_count;++i) ok=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_CLIP_REMOVE,.scene=b->scene,.clip=source->clips[i].id},NULL);
    if (ok) ok=fl_project_edit_commit(project); else {bake_fail(b,fl_project_diagnostic(project)); fl_project_edit_cancel(project);}
    fl_component_clear(&component);
    if (ok) {if (owner) *owner=id; atomic_store_explicit(&b->status,FL_BAKE_APPLIED,memory_order_release);} return ok;
}

bool fl_bake_matches(const Fl_Bake *b, const Fl_JobDoneEvent *event)
{
    return b && event && b->job.generation && b->job.index==event->job.index && b->job.generation==event->job.generation;
}
