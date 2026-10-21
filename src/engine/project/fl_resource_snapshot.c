#include "fl_resource_snapshot.h"
#include "fl_project_internal.h"
#include "schema/fl_widgets.h"
#include "time/fl_composition.h"
#include "core/fl_clock.h"
#include "core/fl_sha256.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct Fl_ResourceSnapshot {
    Fl_Project *view;
    Fl_JobSystem *jobs;
    Fl_JobHandle job;
    atomic_bool cancel;
    Fl_ResourceStatus result;
    bool *used, owns_root, registered;
    struct Fl_ResourceSnapshot *next;
    size_t *stack, stack_count;
    char source[FL_PATH_MAX], root[FL_PATH_MAX], error[256];
};
static atomic_uint_fast64_t snapshot_serial=1;

static atomic_flag lease_lock=ATOMIC_FLAG_INIT;
static Fl_ResourceSnapshot *leases;

/* Protect a short-lived registry of owned immutable metadata, never live project pointers. */
static void lock_leases(void) {while (atomic_flag_test_and_set_explicit(&lease_lock,memory_order_acquire)) {}}
static void unlock_leases(void) {atomic_flag_clear_explicit(&lease_lock,memory_order_release);}

bool fl_resource_snapshot_leased(const char *directory, const unsigned char hash[32])
{
    if (!directory || !hash) return false;
    bool found=false; lock_leases();
    for (const Fl_ResourceSnapshot *snapshot=leases;snapshot && !found;snapshot=snapshot->next) {
        if (strcmp(snapshot->source,directory)) continue;
        for (size_t i=0;i<fl_project_asset_count(snapshot->view);++i)
            if (snapshot->used[i] && !memcmp(fl_project_asset_at(snapshot->view,i)->hash,hash,32)) {found=true; break;}
    }
    unlock_leases(); return found;
}

/* Resolve the sorted catalog without depending on a live project's storage pointers. */
static size_t position(const Fl_Project *view, Fl_Id id)
{
    size_t lo=0,hi=fl_project_asset_count(view);
    while (lo<hi) {size_t mid=lo+(hi-lo)/2; if (fl_project_asset_at(view,mid)->id<id) lo=mid+1; else hi=mid;}
    const Fl_Asset *asset=fl_project_asset_at(view,lo); return asset && asset->id==id ? lo : SIZE_MAX;
}
/* Mark each resource once; cached source dependencies supply the transitive closure. */
static bool mark(Fl_Id id, Fl_AssetKind kind, void *data)
{
    Fl_ResourceSnapshot *snapshot=data; size_t index=position(snapshot->view,id);
    const Fl_Asset *asset=fl_project_asset_at(snapshot->view,index);
    if (!asset || (kind && asset->kind!=kind)) return false;
    if (!snapshot->used[index]) {snapshot->used[index]=true; snapshot->stack[snapshot->stack_count++]=index;}
    return true;
}

/* Copy and fingerprint bounded chunks; partially copied bytes are never published as ready resources. */
static bool copy_asset(Fl_ResourceSnapshot *snapshot, const Fl_Asset *asset, unsigned char *buffer, size_t capacity)
{
    char source[FL_PATH_MAX],destination[FL_PATH_MAX];
    if (!fl_asset_store_path(snapshot->source,asset->hash,asset->extension,source,sizeof(source)) ||
        !fl_asset_store_path(snapshot->root,asset->hash,asset->extension,destination,sizeof(destination))) return false;
    FILE *input=fl_fs_fopen(source,"rb"), *output=input ? fl_fs_fopen(destination,"wbx") : NULL;
    bool ok=input && output; Fl_Sha256 hash; fl_sha256_init(&hash); uint64_t bytes=0;
    while (ok) {
        if (atomic_load_explicit(&snapshot->cancel,memory_order_relaxed)) {ok=false; break;}
        size_t n=fread(buffer,1,capacity,input);
        if (!n) {ok=!ferror(input); break;}
        if (bytes>asset->size || n>asset->size-bytes) {ok=false; break;}
        bytes+=n; fl_sha256_update(&hash,buffer,n); ok=fwrite(buffer,1,n,output)==n;
    }
    unsigned char digest[32]; fl_sha256_final(&hash,digest);
    ok=ok && bytes==asset->size && !memcmp(digest,asset->hash,32) && fl_fs_flush(output);
    if (input && fclose(input)) ok=false;
    if (output && fclose(output)) ok=false;
    return ok;
}

/* Worker owns result/error until its completion fence; all input paths and metadata are independently owned. */
static void capture(void *data)
{
    Fl_ResourceSnapshot *snapshot=data; size_t size=1024u*1024u; unsigned char *buffer=malloc(size);
    bool ok=buffer!=NULL;
    for (size_t i=0;ok && i<fl_project_asset_count(snapshot->view);++i) {
        if (!snapshot->used[i]) continue;
        const Fl_Asset *asset=fl_project_asset_at(snapshot->view,i);
        if (asset->kind!=FL_ASSET_IMAGE && asset->kind!=FL_ASSET_VIDEO && asset->kind!=FL_ASSET_AUDIO) continue;
        ok=copy_asset(snapshot,asset,buffer,size);
        if (!ok) snprintf(snapshot->error,sizeof(snapshot->error),"%s: cannot capture verified resource bytes",asset->name);
    }
    free(buffer);
    if (atomic_load_explicit(&snapshot->cancel,memory_order_relaxed)) snapshot->result=FL_RESOURCES_CANCELLED;
    else snapshot->result=ok ? FL_RESOURCES_READY : FL_RESOURCES_FAILED;
    if (!ok && !snapshot->error[0]) snprintf(snapshot->error,sizeof(snapshot->error),"Cannot allocate resource capture buffer");
}

Fl_ResourceSnapshot *fl_resource_snapshot_create(const Fl_Project *project, Fl_JobSystem *jobs,
                                                const Fl_Id *scenes, size_t count, char *error, size_t capacity)
{
    if (error && capacity) error[0]=0;
    if (!project || !jobs || !fl_jobs_done_event(jobs) || (count && !scenes)) return NULL;
    Fl_ResourceSnapshot *snapshot=calloc(1,sizeof(*snapshot)); if (!snapshot) return NULL;
    snapshot->jobs=jobs; atomic_init(&snapshot->cancel,false); snapshot->view=fl_project_read_view(project);
    size_t assets=fl_project_asset_count(project);
    snapshot->used=assets ? calloc(assets,sizeof(*snapshot->used)) : NULL;
    snapshot->stack=assets && assets<=SIZE_MAX/sizeof(*snapshot->stack) ? malloc(assets*sizeof(*snapshot->stack)) : NULL;
    bool ok=snapshot->view && (!assets || (snapshot->used && snapshot->stack)) && strlen(fl_project_directory(project))<sizeof(snapshot->source);
    if (ok) snprintf(snapshot->source,sizeof(snapshot->source),"%s",fl_project_directory(project));
    for (size_t i=0;ok && i<count;++i) {
        const Fl_Scene *scene=fl_project_scene(snapshot->view,scenes[i]); if (!scene) {ok=false; break;}
        Fl_Composition *composition=NULL;
        if (fl_composition_required(scene)) {
            composition=fl_composition_create(scene,NULL,snapshot->error,sizeof(snapshot->error));
            if (!composition) {ok=false; break;}
            scene=fl_composition_scene(composition);
        }
        for (size_t j=0;ok && j<scene->clip_count;++j) ok=mark(scene->clips[j].asset,FL_ASSET_AUDIO,snapshot);
        for (size_t j=0;ok && j<scene->entity_count;++j) {
            const Fl_Entity *entity=&scene->entities[j];
            for (size_t k=0;ok && k<entity->component_count;++k) ok=fl_component_visit_assets(&entity->components[k],mark,snapshot);
        }
        const Fl_Scene *scope_scene; const Fl_TimeMapping *mapping; size_t parent;
        for (size_t scope=1;ok && fl_composition_scope(composition,scope,&scope_scene,&parent,&mapping);++scope)
            for (size_t j=0;ok && j<scope_scene->clip_count;++j) ok=mark(scope_scene->clips[j].asset,FL_ASSET_AUDIO,snapshot);
        fl_composition_destroy(composition);
    }
    while (ok && snapshot->stack_count) {
        const Fl_Asset *asset=fl_project_asset_at(snapshot->view,snapshot->stack[--snapshot->stack_count]);
        for (size_t i=0;ok && i<asset->reference_count;++i) ok=mark(asset->references[i],0,snapshot);
    }
    char cache[FL_PATH_MAX],parent[FL_PATH_MAX],leaf[64],folder[FL_PATH_MAX];
    snprintf(leaf,sizeof(leaf),"%016llx-%016llx",(unsigned long long)fl_clock_monotonic_ns(),
        (unsigned long long)atomic_fetch_add_explicit(&snapshot_serial,1,memory_order_relaxed));
    if (ok) ok=fl_paths_user_dir(FL_PATH_CACHE,cache,sizeof(cache)) && fl_fs_join(parent,sizeof(parent),cache,"resources") && fl_fs_mkdir_p(parent) &&
        fl_fs_join(snapshot->root,sizeof(snapshot->root),parent,leaf) && fl_fs_mkdir_exclusive(snapshot->root);
    if (ok) snapshot->owns_root=true;
    if (ok) ok=fl_fs_join(folder,sizeof(folder),snapshot->root,"assets") && fl_fs_mkdir_p(folder) && fl_project_read_view_rebase(snapshot->view,snapshot->root);
    if (ok) {
        lock_leases(); snapshot->next=leases; leases=snapshot; snapshot->registered=true; unlock_leases();
        snapshot->job=fl_job_submit(jobs,&(Fl_JobDesc){.fn=capture,.data=snapshot,.priority=FL_JOB_PRIORITY_HIGH,.notify=true});
        ok=snapshot->job.generation!=0;
    }
    if (!ok) {
        if (error && capacity) snprintf(error,capacity,"Cannot prepare immutable resource snapshot");
        fl_resource_snapshot_destroy(snapshot); return NULL;
    }
    return snapshot;
}

void fl_resource_snapshot_destroy(Fl_ResourceSnapshot *snapshot)
{
    if (!snapshot) return;
    atomic_store_explicit(&snapshot->cancel,true,memory_order_relaxed); fl_job_wait(snapshot->jobs,snapshot->job);
    if (snapshot->registered) {
        lock_leases(); Fl_ResourceSnapshot **entry=&leases;
        while (*entry && *entry!=snapshot) entry=&(*entry)->next;
        if (*entry) *entry=snapshot->next;
        unlock_leases();
    }
    fl_project_destroy(snapshot->view);
    if (snapshot->owns_root) fl_fs_remove_tree(snapshot->root);
    free(snapshot->used); free(snapshot->stack); free(snapshot);
}
Fl_ResourceStatus fl_resource_snapshot_status(const Fl_ResourceSnapshot *snapshot)
{
    if (!snapshot) return FL_RESOURCES_FAILED;
    return fl_job_is_done(snapshot->jobs,snapshot->job) ? snapshot->result : FL_RESOURCES_PENDING;
}
const char *fl_resource_snapshot_error(const Fl_ResourceSnapshot *snapshot)
{
    return snapshot && fl_resource_snapshot_status(snapshot)!=FL_RESOURCES_PENDING ? snapshot->error : "";
}
Fl_Project *fl_resource_snapshot_project(const Fl_ResourceSnapshot *snapshot) {return snapshot ? snapshot->view : NULL;}
bool fl_resource_snapshot_matches(const Fl_ResourceSnapshot *snapshot, const Fl_JobDoneEvent *event)
{
    return snapshot && event && snapshot->job.index==event->job.index && snapshot->job.generation==event->job.generation;
}
