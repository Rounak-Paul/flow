#include "flow_test.h"
#include "media/fl_encoder.h"
#include "core/fl_clock.h"
#include "core/fl_thread.h"
#include "platform/fl_process.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <stdlib.h>
#include <string.h>

/* Drain worker events until ready/terminal, with a deadline so lifecycle failures cannot hang the suite. */
static Fl_EncoderStatus await_status(Fl_Encoder *encoder, Fl_EventBus *bus)
{
    uint64_t deadline=fl_clock_monotonic_ns()+UINT64_C(10000000000);
    Fl_EncoderStatus status;
    while ((status=fl_encoder_status(encoder))==FL_ENCODER_PENDING && fl_clock_monotonic_ns()<deadline) fl_event_bus_dispatch(bus);
    return fl_encoder_status(encoder);
}

/* Run a real child writer, a failing child, and a child that never drains its pipe. */
int main(int argc, char **argv)
{
    if (argc==3 && !strcmp(argv[1],"sink")) {
        FILE *output=fl_fs_fopen(argv[2],"wb"); if (!output) return 2;
        unsigned char bytes[4096]; size_t n; bool ok=true;
        while ((n=fread(bytes,1,sizeof(bytes),stdin))!=0) if (fwrite(bytes,1,n,output)!=n) {ok=false; break;}
        ok=ok && !ferror(stdin); if (fclose(output)) ok=false; return ok ? 0 : 2;
    }
    if (argc==3 && !strcmp(argv[1],"stall")) {
        Fl_Mutex mutex; Fl_CondVar wake;
        if (!fl_mutex_init(&mutex) || !fl_cond_init(&wake)) return 2;
        fl_mutex_lock(&mutex); fl_cond_wait_ms(&wake,&mutex,30000); fl_mutex_unlock(&mutex);
        fl_cond_destroy(&wake); fl_mutex_destroy(&mutex); return 2;
    }
    if (argc==3 && !strcmp(argv[1],"fail")) return 3;
    CHECK(argc==2); if (argc!=2) return 1;
    Fl_EventBus *bus=fl_event_bus_create(&(Fl_EventBusDesc){0}); CHECK(bus); if (!bus) return 1;
    Fl_EventType event=fl_event_type_register(bus,"encoder.test",sizeof(Fl_EncoderEvent));
    char output[FL_PATH_MAX],command[4*FL_PATH_MAX]="";
    CHECK(fl_fs_join(output,sizeof(output),argv[1],"encoded.bin"));
    CHECK(fl_command_append(command,sizeof(command),argv[0]) && fl_command_append(command,sizeof(command),"sink") && fl_command_append(command,sizeof(command),output));
    Fl_Encoder *encoder=fl_encoder_create(command,bus,event,1); CHECK(encoder);
    for (size_t i=0;i<4;++i) {
        CHECK(await_status(encoder,bus)==FL_ENCODER_READY);
        unsigned char *pixels=malloc(1024*1024); CHECK(pixels); if (!pixels) break;
        memset(pixels,(int)i,1024*1024); bool accepted=fl_encoder_submit(encoder,pixels,1024*1024); CHECK(accepted); if (!accepted) free(pixels);
    }
    fl_encoder_end(encoder); CHECK(await_status(encoder,bus)==FL_ENCODER_DONE); fl_encoder_destroy(encoder);
    CHECK(fl_fs_file_size(output)==4*1024*1024);
    size_t size=0; unsigned char *bytes=(unsigned char *)fl_fs_read_file(output,5*1024*1024,&size); CHECK(bytes && size==4*1024*1024);
    if (bytes && size==4*1024*1024) for (size_t i=0;i<size;++i) if (bytes[i]!=i/(1024*1024)) {CHECK(false); break;}
    free(bytes);
    command[0]=0;
    CHECK(fl_command_append(command,sizeof(command),argv[0]) && fl_command_append(command,sizeof(command),"fail") && fl_command_append(command,sizeof(command),output));
    encoder=fl_encoder_create(command,bus,event,2); CHECK(encoder); fl_encoder_end(encoder);
    CHECK(await_status(encoder,bus)==FL_ENCODER_FAILED); fl_encoder_destroy(encoder);
    command[0]=0;
    CHECK(fl_command_append(command,sizeof(command),argv[0]) && fl_command_append(command,sizeof(command),"stall") && fl_command_append(command,sizeof(command),output));
    encoder=fl_encoder_create(command,bus,event,3); CHECK(encoder); CHECK(await_status(encoder,bus)==FL_ENCODER_READY);
    void *pixels=calloc(1,4*1024*1024); CHECK(pixels);
    bool accepted=fl_encoder_submit(encoder,pixels,4*1024*1024); CHECK(accepted); if (!accepted) free(pixels);
    uint64_t started=fl_clock_monotonic_ns(); fl_encoder_destroy(encoder);
    CHECK(fl_clock_monotonic_ns()-started<UINT64_C(2000000000));
    encoder=fl_encoder_create(command,bus,event,4); CHECK(encoder); fl_encoder_destroy(encoder);
    fl_event_bus_dispatch(bus); CHECK(fl_event_bus_destroy(bus));
    return FLOW_TEST_RESULT("fl_encoder_tests");
}
