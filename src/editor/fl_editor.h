#pragma once

#include "core/fl_event.h"

/*
 * Flow editor — owns the causality instance, the main window and the
 * application event bus, and pumps queued bus events once per UI tick.
 */
typedef struct Fl_Editor Fl_Editor;

/*
 * Create the editor: event bus, causality instance and maximized main window.
 * Must be called on the thread that will run the editor.
 *
 * Returns  A new editor, or NULL when any part fails to initialise.
 */
Fl_Editor *fl_editor_create(void);

/*
 * Run the UI loop until every window is closed.
 *
 * editor   Editor from fl_editor_create.
 * Returns  0 on clean shutdown, non-zero on invalid arguments.
 */
int fl_editor_run(Fl_Editor *editor);

/*
 * Destroy the editor and everything it owns. NULL is a no-op.
 */
void fl_editor_destroy(Fl_Editor *editor);

/*
 * Application-wide event bus owned by the editor.
 *
 * Returns  The bus, valid until fl_editor_destroy; NULL for a NULL editor.
 */
Fl_EventBus *fl_editor_bus(Fl_Editor *editor);
