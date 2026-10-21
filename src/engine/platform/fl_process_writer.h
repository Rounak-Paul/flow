#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct Fl_ProcessWriter Fl_ProcessWriter;
/* Spawn a command in its own process group/job with piped stdin. Caller owns the returned process. */
Fl_ProcessWriter *fl_process_writer_open(const char *command);
/* Write the complete buffer on one worker; cancellation interrupts backpressure. */
bool fl_process_writer_write(Fl_ProcessWriter *process, const void *bytes, size_t size, const atomic_bool *cancel);
/* Terminate this process tree. May run concurrently with write/wait; handles remain valid until destroy. */
void fl_process_writer_cancel(Fl_ProcessWriter *process);
/* Close stdin and reap the encoder on its worker. True only for exit status zero. */
bool fl_process_writer_wait(Fl_ProcessWriter *process, const atomic_bool *cancel);
/* As wait, but terminate the process tree when timeout_ms elapses; zero disables the deadline. */
bool fl_process_writer_wait_timeout(Fl_ProcessWriter *process, const atomic_bool *cancel, uint32_t timeout_ms);
/* Release a reaped process after all concurrent calls have ended. NULL is safe. */
void fl_process_writer_destroy(Fl_ProcessWriter *process);
