#pragma once

#include "core/fl_event.h"

#include <ca_dock.h>
#include <causality.h>

#include <stdbool.h>

/*
 * Console panel — shows the application log.
 *
 * Observes FL_EVENT_LOG_RECORD on the application bus, keeps the most
 * recent FL_CONSOLE_HISTORY records, and renders them as level-coloured
 * lines that stay scrolled to the newest entry. Records arriving before the
 * panel is first shown are kept, so the console opens with full context.
 */
typedef struct Fl_Console Fl_Console;

/* Records retained for display; older ones are discarded first. */
#define FL_CONSOLE_HISTORY 1000u

/* Dock panel id of the console. */
#define FL_CONSOLE_PANEL_ID "console"

/*
 * Create the console and start observing log records.
 *
 * bus       Application bus (owner thread); FL_EVENT_LOG_RECORD is
 *           registered if it does not exist yet.
 * instance  Causality instance used to schedule follow-up frames.
 * Returns   The console, or NULL on invalid arguments or failure.
 */
Fl_Console *fl_console_create(Fl_EventBus *bus, Ca_Instance *instance);

/*
 * Stop observing and free the console. The dock panel must already be
 * torn down. NULL is a no-op.
 */
void fl_console_destroy(Fl_Console *console);

/*
 * Dock panel build callback: emits the console into the current container.
 *
 * user_data  The Fl_Console.
 */
void fl_console_build_panel(Ca_Dock *dock, Ca_Window *window, void *user_data);

/*
 * Dock panel teardown callback: forgets the mounted panel.
 *
 * user_data  The Fl_Console.
 */
void fl_console_teardown_panel(void *user_data);
