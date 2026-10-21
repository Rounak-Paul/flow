#include "fl_video_decode.h"
#include "fl_media.h"
#include "platform/fl_video_frame.h"
#include "platform/fl_process.h"
#include "platform/fl_process_writer.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "core/fl_clock.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static atomic_uint_fast64_t serial=1;

unsigned char *fl_video_decode(const char *path, double seconds, uint32_t max_side,
                               uint32_t *width, uint32_t *height, const atomic_bool *cancel)
{
    if (!path || !width || !height || !max_side || max_side>4096 || !isfinite(seconds) || seconds<0 ||
        (cancel && atomic_load_explicit(cancel,memory_order_relaxed))) return NULL;
    unsigned char *pixels=fl_video_frame_at(path,seconds,max_side,width,height,cancel);
    if (pixels || (cancel && atomic_load_explicit(cancel,memory_order_relaxed))) return pixels;
    const char *ffmpeg=fl_ffmpeg_path(); if (!ffmpeg) return NULL;
    char cache[FL_PATH_MAX],parent[FL_PATH_MAX],leaf[80],root[FL_PATH_MAX],output[FL_PATH_MAX];
    snprintf(leaf,sizeof(leaf),"%016llx-%016llx",(unsigned long long)fl_clock_monotonic_ns(),
        (unsigned long long)atomic_fetch_add_explicit(&serial,1,memory_order_relaxed));
    if (!fl_paths_user_dir(FL_PATH_CACHE,cache,sizeof(cache)) || !fl_fs_join(parent,sizeof(parent),cache,"decode") || !fl_fs_mkdir_p(parent) ||
        !fl_fs_join(root,sizeof(root),parent,leaf) || !fl_fs_mkdir_exclusive(root)) return NULL;
    char command[4*FL_PATH_MAX]="",timestamp[64],filter[128];
    snprintf(timestamp,sizeof(timestamp),"%.9f",seconds);
    snprintf(filter,sizeof(filter),"scale=%u:%u:force_original_aspect_ratio=decrease",max_side,max_side);
    bool ok=fl_fs_join(output,sizeof(output),root,"frame.png");
    const char *args[]={ffmpeg,"-nostdin","-v","error","-y","-ss",timestamp,"-i",path,"-an","-frames:v","1","-vf",filter,output};
    for (size_t i=0;ok && i<sizeof(args)/sizeof(*args);++i) ok=fl_command_append(command,sizeof(command),args[i]);
    Fl_ProcessWriter *process=ok ? fl_process_writer_open(command) : NULL;
    if (process) {
        ok=fl_process_writer_wait_timeout(process,cancel,30000); fl_process_writer_destroy(process);
        if (ok && !(cancel && atomic_load_explicit(cancel,memory_order_relaxed))) pixels=fl_image_decode(output,max_side,width,height);
    }
    fl_fs_remove_tree(root); return pixels;
}
