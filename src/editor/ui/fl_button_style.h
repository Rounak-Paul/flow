#pragma once

/* Shared editor button classes and theme rules. Tokens come from style.h. */
#define FL_CLASS_BUTTON "fl-button"
#define FL_CLASS_BUTTON_ACTIVE "fl-button active"
#define FL_CLASS_BUTTON_ICON "fl-button-icon"
#define FL_CLASS_BUTTON_ICON_ACTIVE "fl-button-icon active"
#define FL_CLASS_MODE "fl-mode"
#define FL_CLASS_MODE_ACTIVE "fl-mode active"

#define FL_STYLE_CSS_BUTTONS \
    ".fl-button { padding: 5px 14px; background: " FL_COLOR_CONTROL "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_PILL "; color: " FL_COLOR_TEXT_BRIGHT "; flex-shrink: 0; }" \
    ".fl-button.active, .fl-button:hover { background: " FL_COLOR_CONTROL_HOVER "; }" \
    ".fl-button-icon { width: 24px; height: 24px; flex-shrink: 0; padding: 0px; justify-content: center; align-items: center; color: " FL_COLOR_TEXT_MEDIUM "; background: " FL_COLOR_CONTROL "; border-radius: " FL_RADIUS_PILL "; }" \
    ".fl-button-icon.active { color: " FL_COLOR_TEXT_BRIGHT "; background: " FL_COLOR_SELECTION "; }" \
    ".fl-button.danger, .fl-button-icon.danger { color: " FL_COLOR_DANGER "; }" \
    ".fl-button.danger:hover, .fl-button-icon.danger:hover { background: " FL_COLOR_DANGER_HOVER "; }" \
    ".fl-mode { height: 24px; padding: 0px 10px; flex-shrink: 0; justify-content: center; align-items: center; color: " FL_COLOR_TEXT_MEDIUM "; background: " FL_COLOR_CONTROL "; border-radius: " FL_RADIUS_PILL "; }" \
    ".fl-mode.active { color: " FL_COLOR_TEXT_BRIGHT "; background: " FL_COLOR_SELECTION "; }"
