#pragma once

#include "core/fl_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Rotating log-file sink. One per process; opening again replaces the
 * previous file. Records at INFO and above are flushed immediately, so a
 * crash loses at most buffered TRACE/DEBUG output.
 */

typedef struct Fl_LogFileDesc {
    const char *dir;            /* required; created if missing              */
    const char *name;           /* default "flow.log"                        */
    uint64_t    rotate_bytes;   /* rotate when exceeded; 0 = 8 MiB           */
    uint32_t    keep_files;     /* rotated files kept (name.1 ...); 0 = 3    */
    Fl_LogLevel min_level;      /* sink threshold                            */
} Fl_LogFileDesc;

/*
 * Open the log file and attach it as a log sink. Rotates an existing file
 * that already exceeds rotate_bytes.
 *
 * Returns  false when the directory or file cannot be created; no sink is
 *          attached then.
 */
bool fl_log_file_open(const Fl_LogFileDesc *desc);

/*
 * Detach the sink, flush and close the file. No-op when not open.
 */
void fl_log_file_close(void);

/*
 * Path of the active log file.
 *
 * Returns  false when no file is open or the path does not fit in cap.
 */
bool fl_log_file_path(char *out, size_t cap);
