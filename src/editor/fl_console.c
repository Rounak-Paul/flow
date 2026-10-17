#include "fl_console.h"

#include "fl_editor.h"
#include "style.h"

#include "core/fl_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCROLL_ID     "fl-console-scroll"
#define LINE_MAX_TEXT (FL_LOG_EVENT_TAG_MAX + FL_LOG_EVENT_MESSAGE_MAX + 8u)

struct Fl_Console {
    Fl_EventBus *bus;
    Ca_Instance *instance;
    Ca_Effect   *follower;       /* keeps the view pinned to the newest line */
    Ca_Dock     *dock;           /* set while the panel is mounted           */
    Ca_Window   *window;
    Fl_LogEvent *history;        /* ring buffer of FL_CONSOLE_HISTORY records */
    size_t       head;           /* index of the oldest record               */
    size_t       count;
    bool         scroll_pending;
};

static const char *const k_level_classes[] = FL_CLASS_CONSOLE_LEVELS;

static const Fl_LogEvent *record_at(const Fl_Console *console, size_t index)
{
    return &console->history[(console->head + index) % FL_CONSOLE_HISTORY];
}

/*
 * Log observer: append the record and refresh the panel if it is shown.
 *
 * event      FL_EVENT_LOG_RECORD carrying an Fl_LogEvent.
 * user_data  Owning console.
 * Returns    false; other observers also receive the record.
 */
static bool on_log_record(const Fl_Event *event, void *user_data)
{
    Fl_Console *console = user_data;
    const Fl_LogEvent *record = event->payload;
    if (console->count < FL_CONSOLE_HISTORY) {
        console->history[(console->head + console->count) % FL_CONSOLE_HISTORY] = *record;
        console->count++;
    } else {
        console->history[console->head] = *record;
        console->head = (console->head + 1u) % FL_CONSOLE_HISTORY;
    }
    if (console->dock) {
        ca_dock_invalidate_panel(console->dock, FL_CONSOLE_PANEL_ID);
        console->scroll_pending = true;
        ca_instance_request_frame_after(console->instance, 0.0);
    }
    return false;
}

/*
 * Per-tick follower: once a rebuild has been laid out, scroll to the newest line.
 *
 * user_data  Owning console.
 */
static void follow_newest(void *user_data)
{
    Fl_Console *console = user_data;
    if (!console->scroll_pending || !console->window) return;
    console->scroll_pending = false;
    ca_scroll_to_bottom(console->window, SCROLL_ID);
}

Fl_Console *fl_console_create(Fl_EventBus *bus, Ca_Instance *instance)
{
    if (!bus || !instance) return NULL;
    Fl_Console *console = calloc(1, sizeof(*console));
    if (!console) return NULL;
    console->bus = bus;
    console->instance = instance;
    console->history = calloc(FL_CONSOLE_HISTORY, sizeof(Fl_LogEvent));

    const Fl_EventType type = fl_event_type_register(bus, FL_EVENT_LOG_RECORD, sizeof(Fl_LogEvent));
    const bool subscribed = console->history && type != FL_EVENT_TYPE_INVALID &&
        fl_event_subscribe(bus, &(Fl_SubscribeDesc){
            .type = type, .fn = on_log_record, .user_data = console, .owner = console,
        }).generation != 0;
    console->follower = subscribed ? ca_frame_effect(instance, follow_newest, console) : NULL;
    if (!console->follower) {
        fl_console_destroy(console);
        return NULL;
    }
    return console;
}

void fl_console_destroy(Fl_Console *console)
{
    if (!console) return;
    ca_effect_destroy(console->follower);
    fl_event_unsubscribe_owner(console->bus, console);
    free(console->history);
    free(console);
}

void fl_console_build_panel(Ca_Dock *dock, Ca_Window *window, void *user_data)
{
    Fl_Console *console = user_data;
    console->dock = dock;
    console->window = window;

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = FL_CLASS_CONSOLE, .id = SCROLL_ID });
    char text[LINE_MAX_TEXT];
    char style[64];
    for (size_t i = 0; i < console->count; ++i) {
        const Fl_LogEvent *record = record_at(console, i);
        const int level = record->level >= 0 && record->level <= FL_LOG_FATAL ? record->level : FL_LOG_INFO;
        snprintf(text, sizeof(text), "[%s] %s", record->tag, record->message);
        snprintf(style, sizeof(style), "%s %s", FL_CLASS_CONSOLE_LINE, k_level_classes[level]);
        ca_text(&(Ca_TextDesc){ .text = text, .style = style, .wrap = true });
    }
    ca_div_end();
    console->scroll_pending = true;
    ca_instance_request_frame_after(console->instance, 0.0);
}

void fl_console_teardown_panel(void *user_data)
{
    Fl_Console *console = user_data;
    console->dock = NULL;
    console->window = NULL;
    console->scroll_pending = false;
}
