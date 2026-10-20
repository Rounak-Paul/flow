#include "fl_button.h"
#include "style.h"
#include <stdio.h>

Ca_Button *fl_button(const Fl_ButtonDesc *desc)
{
    if (!desc || !desc->text || !desc->text[0] ||
        (desc->kind != FL_BUTTON_TEXT && desc->kind != FL_BUTTON_ICON)) return NULL;
    const char *base = desc->kind == FL_BUTTON_ICON
        ? (desc->active ? FL_CLASS_BUTTON_ICON_ACTIVE : FL_CLASS_BUTTON_ICON)
        : (desc->active ? FL_CLASS_BUTTON_ACTIVE : FL_CLASS_BUTTON);
    char style[64];
    snprintf(style, sizeof(style), "%s%s", base, desc->destructive ? " danger" : "");
    Ca_Button *button = ca_btn_begin(&(Ca_BtnDesc){.text = desc->text, .id = desc->id, .style = style,
        .on_click = desc->on_click, .click_data = desc->click_data, .disabled = desc->disabled});
    ca_btn_end();
    if (desc->tooltip && desc->tooltip[0]) ca_tooltip(&(Ca_TooltipDesc){.text = desc->tooltip});
    return button;
}
