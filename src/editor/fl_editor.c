#include "fl_editor.h"

#include <causality.h>

#include <stdio.h>
#include <stdlib.h>

#define FLOW_WINDOW_WIDTH  1024
#define FLOW_WINDOW_HEIGHT 640

struct Fl_Editor {
    Fl_EventBus *bus;
    Ca_Instance *instance;
    Ca_Window   *window;
    Ca_Effect   *bus_pump;
};

/*
 * Bus wake hook: interrupts causality's blocking event wait so queued events
 * are delivered on the next tick. Thread-safe.
 *
 * user_data  Unused.
 */
static void wake_ui_loop(void *user_data)
{
    (void)user_data;
    ca_instance_wake();
}

/*
 * Per-tick frame effect delivering queued bus events before UI layout and
 * rendering, so posted changes appear in the same frame.
 *
 * user_data  Owning editor.
 */
static void pump_bus(void *user_data)
{
    Fl_Editor *editor = user_data;
    fl_event_bus_dispatch(editor->bus);
}

/*
 * Main window close notification: secondary windows must not outlive it.
 *
 * window     Main window being closed.
 * user_data  Owning editor.
 */
static void on_main_window_close(Ca_Window *window, void *user_data)
{
    Fl_Editor *editor = user_data;
    ca_instance_destroy_other_windows(editor->instance, window);
    editor->window = NULL;
}

/*
 * Build the initial UI tree for the main window.
 *
 * window  Main editor window that receives the root container.
 */
static void build_ui(Ca_Window *window)
{
    ca_ui_begin(window, &(Ca_DivDesc){
        .direction  = CA_VERTICAL,
        .background = ca_color(0.07f, 0.07f, 0.09f, 1.0f),
    });
    ca_ui_end();
}

Fl_Editor *fl_editor_create(void)
{
    Fl_Editor *editor = calloc(1, sizeof(*editor));
    if (!editor) return NULL;

    editor->bus = fl_event_bus_create(&(Fl_EventBusDesc){ .wake = wake_ui_loop });
    if (!editor->bus) {
        fprintf(stderr, "flow: failed to create event bus\n");
        fl_editor_destroy(editor);
        return NULL;
    }

    editor->instance = ca_instance_create(&(Ca_InstanceDesc){
        .app_name             = "Flow",
        .prefer_dedicated_gpu = true,
    });
    if (!editor->instance) {
        fprintf(stderr, "flow: failed to create causality instance\n");
        fl_editor_destroy(editor);
        return NULL;
    }

    editor->window = ca_window_create(editor->instance, &(Ca_WindowDesc){
        .title         = "Flow " FLOW_VERSION,
        .width         = FLOW_WINDOW_WIDTH,
        .height        = FLOW_WINDOW_HEIGHT,
        .on_close      = on_main_window_close,
        .on_close_data = editor,
    });
    if (!editor->window) {
        fprintf(stderr, "flow: failed to create main window\n");
        fl_editor_destroy(editor);
        return NULL;
    }
    ca_window_maximize(editor->window);
    build_ui(editor->window);

    editor->bus_pump = ca_frame_effect(editor->instance, pump_bus, editor);
    if (!editor->bus_pump) {
        fprintf(stderr, "flow: failed to register event bus pump\n");
        fl_editor_destroy(editor);
        return NULL;
    }
    return editor;
}

int fl_editor_run(Fl_Editor *editor)
{
    if (!editor || !editor->instance) return 1;
    while (ca_instance_tick(editor->instance)) {
    }
    return 0;
}

void fl_editor_destroy(Fl_Editor *editor)
{
    if (!editor) return;
    ca_effect_destroy(editor->bus_pump);
    editor->bus_pump = NULL;
    if (editor->instance) ca_instance_destroy(editor->instance);
    if (editor->bus && !fl_event_bus_destroy(editor->bus))
        fprintf(stderr, "flow: event bus destroyed from an invalid context\n");
    free(editor);
}

Fl_EventBus *fl_editor_bus(Fl_Editor *editor)
{
    return editor ? editor->bus : NULL;
}
