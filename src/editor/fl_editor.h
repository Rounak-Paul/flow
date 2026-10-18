#pragma once

#include "core/fl_event.h"
#include "core/fl_job.h"

/*
 * Flow editor — the application host. Owns process services (logging, crash
 * capture, termination handling), the application event bus, the job system,
 * the causality instance and the main window, and pumps queued bus events once
 * per UI tick.
 */
typedef struct Fl_Editor Fl_Editor;

/* Posted when the OS or user asks the app to quit; payload is the int signal number. */
#define FL_EVENT_APP_QUIT_REQUESTED "app.quit_requested"

/* Posted for every log record at INFO and above; payload is Fl_LogEvent. */
#define FL_EVENT_LOG_RECORD "log.record"

#define FL_LOG_EVENT_TAG_MAX     32u
#define FL_LOG_EVENT_MESSAGE_MAX 480u

typedef struct Fl_LogEvent {
    uint64_t sequence;
    int32_t  level;                               /* Fl_LogLevel                 */
    char     tag[FL_LOG_EVENT_TAG_MAX];           /* truncated, NUL-terminated   */
    char     message[FL_LOG_EVENT_MESSAGE_MAX];   /* truncated, NUL-terminated   */
} Fl_LogEvent;

/*
 * Create the editor and every process service. Must be called on the main
 * thread before any other thread is started.
 *
 * Returns  A new editor, or NULL when any part fails to initialise.
 */
Fl_Editor *fl_editor_create(void);

/*
 * Open the project in directory, replacing the project launcher. Call before fl_editor_run.
 *
 * Returns  false when the project cannot be opened (for example it is open in another process).
 */
bool fl_editor_open_project(Fl_Editor *editor, const char *directory);

/*
 * Run the UI loop until every window is closed.
 *
 * editor   Editor from fl_editor_create.
 * Returns  0 on clean shutdown, non-zero on invalid arguments.
 */
int fl_editor_run(Fl_Editor *editor);

/*
 * Destroy the editor and shut every service down in reverse order. NULL is a
 * no-op.
 */
void fl_editor_destroy(Fl_Editor *editor);

/*
 * Application-wide event bus owned by the editor.
 *
 * Returns  The bus, valid until fl_editor_destroy; NULL for a NULL editor.
 */
Fl_EventBus *fl_editor_bus(Fl_Editor *editor);

/*
 * Application-wide job system owned by the editor.
 *
 * Returns  The job system, valid until fl_editor_destroy; NULL for a NULL editor.
 */
Fl_JobSystem *fl_editor_jobs(Fl_Editor *editor);
