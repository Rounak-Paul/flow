#pragma once
#include <causality.h>

/* Standard editor action presentation; icon actions share one 24px circular shape. */
typedef enum Fl_ButtonKind { FL_BUTTON_TEXT, FL_BUTTON_ICON } Fl_ButtonKind;

/* Button content and borrowed callback data, consumed during the current UI build. */
typedef struct Fl_ButtonDesc {
    const char *text;
    const char *id;
    const char *tooltip;
    Ca_ClickFn on_click;
    void *click_data;
    Fl_ButtonKind kind;
    bool active;
    bool disabled;
} Fl_ButtonDesc;

/* Emit an action using desc content, callback and state, followed by its tooltip.
 * Returns the borrowed UI handle, or NULL for a NULL desc, empty text or invalid kind. */
Ca_Button *fl_button(const Fl_ButtonDesc *desc);
