#include "fl_encoder.h"
#include "core/fl_thread.h"
#include "platform/fl_process_writer.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct Fl_Encoder {
    Fl_Thread *thread;
    Fl_Mutex mutex;
    Fl_CondVar wake;
    atomic_bool cancel;
    Fl_ProcessWriter *process;
    Fl_EventBus *bus;
    Fl_EventType event;
    Fl_EncoderEvent notification;
    char *command;
    void *pixels;
    size_t size;
    bool started,end,finished,failed;
};

/* Completion events contain identities only; destruction never leaves a queued owner pointer. */
static void notify(Fl_Encoder *encoder)
{
    while (!atomic_load_explicit(&encoder->cancel,memory_order_relaxed) &&
        !fl_event_post(encoder->bus,encoder->event,&encoder->notification,sizeof(encoder->notification))) {
        fl_mutex_lock(&encoder->mutex);
        if (!atomic_load_explicit(&encoder->cancel,memory_order_relaxed)) fl_cond_wait_ms(&encoder->wake,&encoder->mutex,20);
        fl_mutex_unlock(&encoder->mutex);
    }
}

/* Own process writes and final wait; only the frame mailbox crosses the owner/worker boundary. */
static void encode(void *data)
{
    Fl_Encoder *encoder=data; fl_thread_set_name("Flow encoder");
    Fl_ProcessWriter *process=fl_process_writer_open(encoder->command);
    fl_mutex_lock(&encoder->mutex);
    encoder->process=process; encoder->started=true; encoder->failed=process==NULL;
    fl_mutex_unlock(&encoder->mutex); notify(encoder);
    bool ok=process!=NULL;
    while (ok) {
        fl_mutex_lock(&encoder->mutex);
        while (!encoder->pixels && !encoder->end && !atomic_load_explicit(&encoder->cancel,memory_order_relaxed))
            fl_cond_wait(&encoder->wake,&encoder->mutex);
        void *pixels=encoder->pixels; size_t size=encoder->size;
        encoder->pixels=NULL; encoder->size=0;
        bool done=encoder->end && !pixels,cancelled=atomic_load_explicit(&encoder->cancel,memory_order_relaxed);
        fl_mutex_unlock(&encoder->mutex);
        if (cancelled || done) {free(pixels); break;}
        notify(encoder);
        ok=fl_process_writer_write(process,pixels,size,&encoder->cancel); free(pixels);
    }
    if (process) {
        if (!ok || atomic_load_explicit(&encoder->cancel,memory_order_relaxed)) fl_process_writer_cancel(process);
        ok=fl_process_writer_wait(process,&encoder->cancel) && ok;
    }
    fl_mutex_lock(&encoder->mutex); encoder->finished=true; encoder->failed=!ok; fl_mutex_unlock(&encoder->mutex);
    notify(encoder);
}

Fl_Encoder *fl_encoder_create(const char *command, Fl_EventBus *bus, Fl_EventType event, uint64_t token)
{
    if (!command || !command[0] || !bus || !event || !token) return NULL;
    Fl_Encoder *encoder=calloc(1,sizeof(*encoder)); if (!encoder) return NULL;
    if (!fl_mutex_init(&encoder->mutex)) {free(encoder); return NULL;}
    if (!fl_cond_init(&encoder->wake)) {fl_mutex_destroy(&encoder->mutex); free(encoder); return NULL;}
    encoder->command=malloc(strlen(command)+1);
    encoder->bus=bus; encoder->event=event; encoder->notification.token=token; atomic_init(&encoder->cancel,false);
    if (encoder->command) {strcpy(encoder->command,command); encoder->thread=fl_thread_create(encode,encoder);}
    if (!encoder->thread) {fl_cond_destroy(&encoder->wake); fl_mutex_destroy(&encoder->mutex); free(encoder->command); free(encoder); return NULL;}
    return encoder;
}
Fl_EncoderStatus fl_encoder_status(Fl_Encoder *encoder)
{
    if (!encoder) return FL_ENCODER_FAILED;
    fl_mutex_lock(&encoder->mutex);
    Fl_EncoderStatus status=encoder->failed ? FL_ENCODER_FAILED : encoder->finished ? FL_ENCODER_DONE :
        encoder->started && !encoder->pixels && !encoder->end ? FL_ENCODER_READY : FL_ENCODER_PENDING;
    fl_mutex_unlock(&encoder->mutex); return status;
}
bool fl_encoder_submit(Fl_Encoder *encoder, void *pixels, size_t size)
{
    if (!encoder || !pixels || !size) return false;
    fl_mutex_lock(&encoder->mutex);
    bool ok=encoder->started && !encoder->failed && !encoder->finished && !encoder->end && !encoder->pixels && !atomic_load_explicit(&encoder->cancel,memory_order_relaxed);
    if (ok) {encoder->pixels=pixels; encoder->size=size; fl_cond_signal(&encoder->wake);}
    fl_mutex_unlock(&encoder->mutex); return ok;
}
void fl_encoder_end(Fl_Encoder *encoder)
{
    if (!encoder) return;
    fl_mutex_lock(&encoder->mutex); encoder->end=true; fl_cond_signal(&encoder->wake); fl_mutex_unlock(&encoder->mutex);
}
void fl_encoder_destroy(Fl_Encoder *encoder)
{
    if (!encoder) return;
    atomic_store_explicit(&encoder->cancel,true,memory_order_relaxed);
    fl_mutex_lock(&encoder->mutex);
    fl_process_writer_cancel(encoder->process); fl_cond_broadcast(&encoder->wake);
    fl_mutex_unlock(&encoder->mutex); fl_thread_join(encoder->thread);
    fl_process_writer_destroy(encoder->process); free(encoder->pixels); free(encoder->command);
    fl_cond_destroy(&encoder->wake); fl_mutex_destroy(&encoder->mutex); free(encoder);
}
