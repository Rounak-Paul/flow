#include "fl_render_host.h"
#include "render/gpu/fl_scene_renderer.h"
#include "core/fl_event.h"
#include "core/fl_job.h"
#include "project/fl_project.h"
#include "text/fl_fonts.h"
#include "components/fl_effect.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "media/fl_png.h"
#include <string.h>
#include <causality.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Read one finite command-line number without accepting trailing characters. */
static bool number(const char *text, double *value)
{
    char *end=NULL; errno=0; *value=strtod(text,&end);
    return !errno && end!=text && !*end && isfinite(*value);
}

/* Measure consecutive 60fps frames through the real GPU renderer without encoding or modifying the project. */
int main(int argc, char **argv)
{
    if (argc!=6 && argc!=8 && argc!=9) {fprintf(stderr,"Usage: %s PROJECT SCENE_INDEX SECONDS WIDTH COUNT [--frames DIRECTORY | EFFECT_NAME SOURCE [OUTPUT_PNG]]\n",argv[0]); return 2;}
    double index,time,width,count;
    if (!number(argv[2],&index) || !number(argv[3],&time) || !number(argv[4],&width) || !number(argv[5],&count) ||
        index<0 || index>100000 || index!=floor(index) || time<0 || width<2 || width>8192 || width!=floor(width) || count<1 || count>600 || count!=floor(count)) return 2;
    bool sequence = argc == 8 && !strcmp(argv[6], "--frames");
    if (sequence && !fl_fs_mkdir_p(argv[7])) return 2;
    Fl_Project *project=fl_project_open(argv[1],NULL);
    const Fl_Scene *scene=project ? fl_project_scene_at(project,(size_t)index) : NULL;
    if (!project) fprintf(stderr,"Cannot open project; close it in Flow or profile an independent copy.\n");
    if (!scene || time+(count-1)/60.>(double)fl_scene_length(scene)/FL_TICKS_PER_SECOND) {fl_project_destroy(project); return 2;}
    if (argc>=8 && !sequence) {
        size_t size=0; char *source=fl_fs_read_file(argv[7],65536,&size);
        bool replaced=false;
        for (size_t i=0;source && i<scene->entity_count;++i) if (!strcmp(scene->entities[i].name,argv[6])) {
            const Fl_Entity *entity=&scene->entities[i];
            const Fl_Component *existing=fl_entity_component(entity,FL_COMPONENT_EFFECT);
            Fl_Effect effect;
            unsigned char *bytes=malloc(FL_EFFECT_HEADER+size);
            size_t encoded=0;
            if (bytes && fl_effect_decode(existing,&effect)) {
                effect.kind=FL_EFFECT_CUSTOM; effect.source=source; effect.source_size=size;
                encoded=fl_effect_encode(&effect,bytes,FL_EFFECT_HEADER+size);
            }
            if (encoded) replaced=fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene->id,.entity=entity->id,
                .component={.id=existing->id,.type=FL_COMPONENT_EFFECT,.version=1,.size=encoded,.data=bytes}},NULL);
            free(bytes); break;
        }
        free(source);
        if (!replaced) {fprintf(stderr,"Could not override effect in memory\n"); fl_project_destroy(project); return 2;}
        scene=fl_project_scene_at(project,(size_t)index);
    }
    Fl_EventBus *bus=fl_event_bus_create(&(Fl_EventBusDesc){0});
    Fl_JobSystem *jobs=bus ? fl_jobs_create(&(Fl_JobSystemDesc){.bus=bus}) : NULL;
    Fl_Fonts *fonts=bus ? fl_fonts_create(&(Fl_FontsDesc){.bus=bus}) : NULL;
    Ca_Instance *instance=ca_instance_create(&(Ca_InstanceDesc){.app_name="Flow render benchmark"});
    Fl_SceneRenderer *renderer=instance && jobs && fonts ? fl_scene_renderer_create(&(Fl_SceneRendererDesc){.gpu=fl_render_host_context(instance),.fonts=fonts,.jobs=jobs}) : NULL;
    bool ok=renderer!=NULL;
    for (unsigned i=0;ok && i<(unsigned)count;++i) {
        Fl_Frame frame={0}; Fl_Tick tick=0;
        if (!fl_time_from_seconds(time+(double)i/60.,&tick)) {ok=false; break;}
        ok=fl_scene_renderer_render(renderer,project,scene,tick,(uint32_t)width,&frame);
        Fl_RenderStats s=fl_scene_renderer_stats(renderer);
        printf("{\"scene\":%.0f,\"frame\":%u,\"width\":%u,\"height\":%u,\"evaluation_ms\":%.4f,\"prepare_ms\":%.4f,\"record_ms\":%.4f,\"gpu_wait_ms\":%.4f,\"readback_ms\":%.4f,\"total_ms\":%.4f}\n",index,i,frame.width,frame.height,s.evaluation_ms,s.prepare_ms,s.record_ms,s.gpu_wait_ms,s.readback_ms,s.total_ms);
        fflush(stdout);
        if (ok && (sequence || (argc==9 && i==0))) {
            char path[FL_PATH_MAX], name[32];
            const char *output = argc==9 ? argv[8] : NULL;
            if (sequence) {
                snprintf(name, sizeof(name), "frame-%06u.png", i);
                if (!fl_fs_join(path, sizeof(path), argv[7], name)) {fl_frame_free(&frame); ok=false; break;}
                output = path;
            }
            size_t size=0; unsigned char *png=fl_png_encode_region(frame.rgba,frame.width,frame.height,0,0,frame.width,frame.height,0,&size,NULL,NULL);
            FILE *file=png ? fl_fs_fopen(output,"wb") : NULL;
            bool written=file && fwrite(png,1,size,file)==size;
            if (file && fclose(file)!=0) written=false;
            free(png); if (!written) ok=false;
        }
        fl_frame_free(&frame);
        if (!ok) fprintf(stderr,"%s\n",fl_scene_renderer_error(renderer));
    }
    fl_scene_renderer_destroy(renderer); if (instance) ca_instance_destroy(instance);
    fl_fonts_destroy(fonts); fl_jobs_destroy(jobs); fl_event_bus_destroy(bus); fl_project_destroy(project);
    return ok ? 0 : 1;
}
