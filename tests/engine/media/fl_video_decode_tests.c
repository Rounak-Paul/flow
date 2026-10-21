#include "flow_test.h"
#include "media/fl_video_decode.h"
#include "platform/fl_process.h"
#include "platform/fl_paths.h"
#include "platform/fl_fs.h"
#include <stdlib.h>
#include <string.h>

/* Seek both directions in a variable-rate source without extracting a persistent frame collection. */
int main(int argc, char **argv)
{
    CHECK(argc==2); if (argc!=2) return 1;
    const char *ffmpeg=fl_ffmpeg_path(); if (!ffmpeg) {printf("video decode skipped: ffmpeg unavailable\n"); return 0;}
    char path[FL_PATH_MAX],command[4*FL_PATH_MAX]="";
    CHECK(fl_fs_join(path,sizeof(path),argv[1],"variable.mp4"));
    const char *args[]={ffmpeg,"-v","error","-y","-f","lavfi","-i","color=c=red:s=64x64:r=5:d=1",
        "-f","lavfi","-i","color=c=blue:s=64x64:r=7:d=1","-filter_complex","[0:v][1:v]concat=n=2:v=1:a=0[v]",
        "-map","[v]","-fps_mode","vfr","-c:v","libx264","-pix_fmt","yuv420p",path};
    bool built=true;
    for (size_t i=0;built && i<sizeof(args)/sizeof(*args);++i) built=fl_command_append(command,sizeof(command),args[i]);
    CHECK(built && fl_command_run(command));
    uint32_t width=0,height=0;
    double times[]={0.4,1.4,0.2,1.7};
    for (size_t i=0;i<4;++i) {
        unsigned char *pixels=fl_video_decode(path,times[i],32,&width,&height,NULL);
        CHECK(pixels && width==32 && height==32);
        if (pixels) {CHECK(i%2 ? pixels[2]>230 && pixels[0]<45 : pixels[0]>230 && pixels[2]<45); free(pixels);}
    }
    atomic_bool cancel=true;
    CHECK(!fl_video_decode(path,0,32,&width,&height,&cancel));
    CHECK(!fl_video_decode(path,-1,32,&width,&height,NULL));
    CHECK(!fl_video_decode(path,0,8192,&width,&height,NULL));
    return FLOW_TEST_RESULT("fl_video_decode_tests");
}
