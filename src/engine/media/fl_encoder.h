#pragma once
#include "core/fl_event.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Fl_Encoder Fl_Encoder;
typedef struct Fl_EncoderEvent {uint64_t token;} Fl_EncoderEvent;
typedef enum Fl_EncoderStatus {FL_ENCODER_PENDING,FL_ENCODER_READY,FL_ENCODER_DONE,FL_ENCODER_FAILED} Fl_EncoderStatus;
/* Spawn on a dedicated worker. Notifications carry token; bus must outlive destruction. */
Fl_Encoder *fl_encoder_create(const char *command, Fl_EventBus *bus, Fl_EventType event, uint64_t token);
/* Poll the bounded mailbox; READY permits one frame submission without waiting. */
Fl_EncoderStatus fl_encoder_status(Fl_Encoder *encoder);
/* Transfer malloc-owned pixels into the mailbox. Failure retains caller ownership. */
bool fl_encoder_submit(Fl_Encoder *encoder, void *pixels, size_t size);
/* Declare end of input after the last submission. Completion arrives through the same event. */
void fl_encoder_end(Fl_Encoder *encoder);
/* Interrupt the process tree, wake and join the worker, then release owned frames. */
void fl_encoder_destroy(Fl_Encoder *encoder);
