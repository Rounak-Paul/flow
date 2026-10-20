#pragma once
#include "ui/fl_button_style.h"
#include "render/fl_frame.h"

/*
 * style.h — single source of truth for every visual decision in the editor.
 *
 * Layout and logic code never hard-codes colours, sizes, fonts or spacing:
 * it references the class names below, and the stylesheet assembled here
 * styles them. To retheme Flow, change the theme tokens; to restyle a
 * component, change its rule. Nothing outside this file needs to move.
 *
 * Structure
 *   1. Theme tokens   palette, metrics, typography (values only)
 *   2. Presentation   window and instance defaults
 *   3. Class names    the vocabulary editor code may reference
 *   4. Stylesheet     causality palette + system chrome + Flow components,
 *                     built from the tokens by string-literal concatenation
 */

/* =====================================================================
   1. THEME TOKENS — "Flow Glass"
   ===================================================================== */

#define FL_THEME_NAME "Flow Glass"

/* Surfaces behind and beneath the glass. */
#define FL_COLOR_BG_VOID        "#070a14"
#define FL_COLOR_BG_BASE        "#0c1120"
#define FL_COLOR_BG_ELEVATED    "#121829"
#define FL_COLOR_BG_SURFACE     "#1a2135"
#define FL_COLOR_BG_OVERLAY     "#242c44"
#define FL_COLOR_SEPARATOR      "#1c2338"

/* OLED-black backdrop shared by the title bar and every window root. */
#define FL_COLOR_BACKDROP       "#000000"

/* Backdrop fill shared by every window root. */
#define FL_STYLE_BACKDROP "background: " FL_COLOR_BACKDROP ";"

/* Glass materials: translucent fills, hairline rims and soft depth. */
#define FL_COLOR_GLASS_TOP      "rgba(64, 80, 82, 0.20)"
#define FL_COLOR_GLASS_BOTTOM   "rgba(58, 68, 66, 0.30)"
#define FL_COLOR_GLASS_BAR      "rgba(255, 255, 255, 0.012)"
#define FL_COLOR_GLASS_BORDER   "rgba(255, 255, 255, 0.075)"
#define FL_COLOR_GLASS_RIM      "rgba(255, 255, 255, 0.12)"
#define FL_COLOR_GLASS_SHADOW   "rgba(0, 0, 0, 0.42)"
#define FL_COLOR_FIELD          "rgba(14, 30, 64, 0.20)"
#define FL_COLOR_FIELD_HOVER    "rgba(0, 0, 0, 0.44)"
#define FL_COLOR_CONTROL        "rgba(255, 255, 255, 0.035)"
#define FL_COLOR_CONTROL_HOVER  "rgba(255, 255, 255, 0.07)"
#define FL_COLOR_CONTROL_PRESS  "rgba(255, 255, 255, 0.02)"
/* Sunken tab track: dark floor, deep inner shadow, lit lower lip. */
#define FL_COLOR_TAB_GUTTER_TOP "rgba(0, 0, 0, 0.85)"
#define FL_COLOR_TAB_GUTTER_BOTTOM "rgba(0, 0, 0, 0.55)"
#define FL_COLOR_TAB_GUTTER_SHADE "rgba(0, 0, 0, 0.95)"
#define FL_COLOR_TAB_GUTTER_EDGE "rgba(0, 0, 0, 0.9)"
#define FL_COLOR_TAB_GUTTER_LIP "rgba(255, 255, 255, 0.10)"
/* Raised glass tab: bright sheen, specular top rim, dark underside, cast shadow. */
#define FL_COLOR_TREE_LINE      "rgba(150, 175, 220, 0.18)"
#define FL_COLOR_TREE_MIXED     "#7f95bd"
#define FL_COLOR_TAB_PILL_TOP   "rgba(255, 255, 255, 0.115)"
#define FL_COLOR_TAB_PILL_BOTTOM "rgba(255, 255, 255, 0.022)"
#define FL_COLOR_CARD_SELECTED_GLOW "rgba(106, 166, 255, 0.10)"
#define FL_COLOR_CARD_SELECTED_RIM "rgba(106, 166, 255, 0.50)"
#define FL_COLOR_CARD_SELECTED_SPECULAR "rgba(150, 195, 255, 0.60)"
#define FL_COLOR_CARD_GLASS_TOP "rgba(255, 255, 255, 0.016)"
#define FL_COLOR_CARD_GLASS_BOTTOM "rgba(255, 255, 255, 0.0)"
#define FL_COLOR_TAB_PILL_RIM   "rgba(255, 255, 255, 0.10)"
#define FL_COLOR_TAB_PILL_SPECULAR "rgba(255, 255, 255, 0.32)"
#define FL_COLOR_TAB_PILL_UNDER "rgba(0, 0, 0, 0.55)"
#define FL_COLOR_TAB_PILL_CAST  "rgba(0, 0, 0, 0.6)"

/* Text, from most to least prominent. */
#define FL_COLOR_TEXT_BRIGHT    "#eef2fa"
#define FL_COLOR_TEXT_MEDIUM    "#bcc5d8"
#define FL_COLOR_TEXT_MUTED     "#8e99b0"
#define FL_COLOR_TEXT_DIM       "#5f6b85"

/* Accent and interaction states. */
#define FL_COLOR_ACCENT         "#6aa6ff"
#define FL_COLOR_ACCENT_HOVER   "#8bbcff"
#define FL_COLOR_ON_ACCENT      "#06101f"
#define FL_COLOR_ACCENT_GLOW    "rgba(106, 166, 255, 0.30)"
#define FL_COLOR_HOVER          "rgba(255, 255, 255, 0.025)"
#define FL_COLOR_PRESSED        "rgba(255, 255, 255, 0.04)"
#define FL_COLOR_SELECTION      "rgba(106, 166, 255, 0.11)"

/* Status. */
#define FL_COLOR_SUCCESS        "#4cc38a"
#define FL_COLOR_WARNING        "#e8b04a"
#define FL_COLOR_DANGER         "#f0707a"
#define FL_COLOR_ON_DANGER      "#0b0b0d"
#define FL_COLOR_DANGER_HOVER   "rgba(240, 112, 122, 0.22)"

/* Overlays and floating surfaces. */
#define FL_COLOR_SCRIM          "rgba(4, 6, 14, 0.55)"
#define FL_COLOR_FLOATING       "rgba(30, 38, 62, 0.62)"
#define FL_COLOR_SCROLL_TRACK   "rgba(0, 0, 0, 0.12)"
#define FL_COLOR_SCROLL_THUMB   "rgba(190, 205, 235, 0.22)"
#define FL_COLOR_SCROLL_ACTIVE  "rgba(210, 222, 245, 0.50)"

/* Motion: the tab pill's slide and how gel-like it is (0 = rigid), and the
   hover/selection fade shared by controls. */
#define FL_MOTION_TAB_SLIDE     "0.42s"
#define FL_MOTION_TAB_EASE      "cubic-bezier(0.34, 1.56, 0.64, 1)"
#define FL_MOTION_TAB_STRETCH   "1"
#define FL_MOTION_FADE          "background-color 0.16s ease-out"
#define FL_MOTION_FADE_BORDER   "border-color 0.16s ease-out"

/* Spacing scale. */
#define FL_SPACE_XS             "2px"
#define FL_SPACE_SM             "4px"
#define FL_SPACE_MD             "8px"
#define FL_SPACE_LG             "12px"
#define FL_SPACE_XL             "16px"

/* Corner radii. */
#define FL_RADIUS_SM            "6px"
#define FL_RADIUS_MD            "9px"
#define FL_RADIUS_CARD          "12px"
#define FL_RADIUS_CARD_GUTTER   "17px"
#define FL_RADIUS_LG            "14px"
#define FL_RADIUS_PILL          "999px"

/* Blur strengths (logical pixels) and the gutter between floating panels. */
#define FL_BLUR_PANEL           "28px"
#define FL_AMBIENT_LIGHT        "56px 140px 0.45"
#define FL_BLUR_FLOAT           "40px"
#define FL_PANEL_GAP            "8px"

/* Type scale (logical pixels; causality applies the UI scale). */
#define FL_FONT_XS              "10px"
#define FL_FONT_SM              "11px"
#define FL_FONT_MD              "12px"
#define FL_FONT_LG              "14px"
#define FL_FONT_XL              "16px"

/* Chrome metrics. */
#define FL_TITLEBAR_HEIGHT      "34px"
#define FL_TAB_BAR_HEIGHT       "28px"
#define FL_SCROLLBAR_WIDTH      "8px"

/* =====================================================================
   2. PRESENTATION DEFAULTS
   ===================================================================== */

#define FL_STYLE_APP_NAME          "Flow"

/* Default height of the console when docked below the scene. */
#define FL_STYLE_CONSOLE_HEIGHT    220.0f

/* Size the main window returns to when un-maximized; it opens maximized. */
#define FL_STYLE_WINDOW_RESTORE_W  1024
#define FL_STYLE_WINDOW_RESTORE_H  640

/* Instance-wide UI scale (1.0 = 100%). */
#define FL_STYLE_UI_SCALE          1.0f

/* =====================================================================
   3. CLASS NAMES
   ===================================================================== */

#define FL_CLASS_APP_ROOT       "fl-app-root"
#define FL_CLASS_SCENE          "fl-scene"
#define FL_CLASS_CONSOLE        "fl-console"
#define FL_CLASS_CONSOLE_LINE   "fl-console-line"

/* Console line modifiers, indexed by Fl_LogLevel (TRACE … FATAL). */
#define FL_CLASS_CONSOLE_LEVELS { "fl-log-trace", "fl-log-debug", "fl-log-info", \
                                  "fl-log-warn", "fl-log-error", "fl-log-fatal" }

/* Mark shown before visible panels in the View menu. */
#define FL_STYLE_MENU_CHECK     CA_ICON_NF_FA_CHECK

/* =====================================================================
   4. STYLESHEET
   ===================================================================== */

/* Causality palette: system widgets Flow does not restyle resolve these. */
#define FL_STYLE_CSS_PALETTE                                                     \
    ":root {"                                                                    \
    "  --ca-bg-void: " FL_COLOR_BG_VOID ";"                                      \
    "  --ca-bg-base: " FL_COLOR_BG_BASE ";"                                      \
    "  --ca-bg-elevated: " FL_COLOR_BG_ELEVATED ";"                              \
    "  --ca-bg-surface: " FL_COLOR_BG_SURFACE ";"                                \
    "  --ca-bg-overlay: " FL_COLOR_BG_OVERLAY ";"                                \
    "  --ca-separator: " FL_COLOR_SEPARATOR ";"                                  \
    "  --ca-text-bright: " FL_COLOR_TEXT_BRIGHT ";"                              \
    "  --ca-text-medium: " FL_COLOR_TEXT_MEDIUM ";"                              \
    "  --ca-text-muted: " FL_COLOR_TEXT_MUTED ";"                                \
    "  --ca-text-dim: " FL_COLOR_TEXT_DIM ";"                                    \
    "  --ca-accent: " FL_COLOR_ACCENT ";"                                        \
    "  --ca-on-accent: " FL_COLOR_ON_ACCENT ";"                                  \
    "  --ca-success: " FL_COLOR_SUCCESS ";"                                      \
    "  --ca-warning: " FL_COLOR_WARNING ";"                                      \
    "  --ca-danger: " FL_COLOR_DANGER ";"                                        \
    "  --ca-on-danger: " FL_COLOR_ON_DANGER ";"                                  \
    "}"                                                                          \
    "* {"                                                                        \
    "  scrollbar-width: " FL_SCROLLBAR_WIDTH ";"                                 \
    "  scrollbar-radius: " FL_RADIUS_SM ";"                                      \
    "  scrollbar-track-color: " FL_COLOR_SCROLL_TRACK ";"                        \
    "  scrollbar-thumb-color: " FL_COLOR_SCROLL_THUMB ";"                        \
    "  scrollbar-thumb-active-color: " FL_COLOR_SCROLL_ACTIVE ";"                \
    "}"

/* Causality system chrome: title bar, popups, menus, tooltips, overlays, dock. */
#define FL_STYLE_CSS_SYSTEM                                                      \
    ".ca-titlebar {"                                                             \
    "  height: " FL_TITLEBAR_HEIGHT "; background: " FL_COLOR_BACKDROP "; border: 0px;" \
    "}"                                                                          \
    ".ca-titlebar-menu { height: 100%; align-items: center; background: transparent; }" \
    ".ca-titlebar-menu-item {"                                                   \
    "  height: 24px; padding: 0px " FL_SPACE_LG "; color: " FL_COLOR_TEXT_MEDIUM ";" \
    "  background: transparent; border-radius: " FL_RADIUS_PILL "; font-size: " FL_FONT_SM ";" \
    "}"                                                                          \
    ".ca-titlebar-menu-item:hover {"                                             \
    "  background: " FL_COLOR_CONTROL "; color: " FL_COLOR_TEXT_BRIGHT ";"       \
    "}"                                                                          \
    ".ca-titlebar-drag { height: 100%; background: transparent; }"               \
    ".ca-titlebar-title { color: " FL_COLOR_TEXT_MUTED "; font-size: " FL_FONT_SM "; }" \
    ".ca-titlebar-controls { height: 100%; gap: " FL_SPACE_SM "; align-items: center; }" \
    ".ca-titlebar-control {"                                                     \
    "  width: 30px; height: 22px; color: " FL_COLOR_TEXT_MEDIUM ";"              \
    "  background: transparent; border-radius: " FL_RADIUS_PILL "; font-size: " FL_FONT_XS ";" \
    "}"                                                                          \
    ".ca-titlebar-control:hover { background: " FL_COLOR_CONTROL "; }"           \
    ".ca-titlebar-close { color: " FL_COLOR_DANGER "; }"                         \
    ".ca-titlebar-close:hover { background: " FL_COLOR_DANGER_HOVER "; }"        \
    ".ca-popup-root {"                                                           \
    "  width: 100%; height: 100%; padding: " FL_SPACE_XL ";"                     \
    "  background: " FL_COLOR_SCRIM "; align-items: stretch; justify-content: center;" \
    "  backdrop-filter: blur(" FL_BLUR_PANEL ");"                                \
    "}"                                                                          \
    ".ca-popup-card {"                                                           \
    "  padding: " FL_SPACE_XL "; gap: " FL_SPACE_LG "; background: " FL_COLOR_FLOATING ";" \
    "  border: 1px solid " FL_COLOR_GLASS_RIM "; border-radius: " FL_RADIUS_LG ";" \
    "  backdrop-filter: blur(" FL_BLUR_FLOAT ");"                                \
    "  box-shadow: 0px 24px 60px " FL_COLOR_GLASS_SHADOW ";"                     \
    "}"                                                                          \
    ".ca-popup-title { color: " FL_COLOR_TEXT_BRIGHT "; font-size: " FL_FONT_LG "; }" \
    ".ca-popup-message { color: " FL_COLOR_TEXT_MEDIUM "; font-size: " FL_FONT_MD "; text-wrap: wrap; }" \
    ".ca-popup-actions { gap: " FL_SPACE_MD "; justify-content: flex-end; }"     \
    ".ca-popup-btn {"                                                            \
    "  padding: " FL_SPACE_SM " " FL_SPACE_XL "; color: " FL_COLOR_TEXT_BRIGHT ";" \
    "  background: " FL_COLOR_CONTROL "; border: 1px solid " FL_COLOR_GLASS_BORDER ";" \
    "  border-radius: " FL_RADIUS_PILL ";"                                       \
    "}"                                                                          \
    ".ca-popup-btn:hover { background: " FL_COLOR_CONTROL_HOVER "; }"            \
    ".ca-select-popup, .ca-tooltip, .ca-context-menu, .ca-menubar-popup {"       \
    "  background: " FL_COLOR_FLOATING "; color: " FL_COLOR_TEXT_BRIGHT ";"      \
    "  border: 1px solid " FL_COLOR_GLASS_RIM "; border-radius: " FL_RADIUS_MD ";" \
    "  backdrop-filter: blur(" FL_BLUR_FLOAT ");"                                \
    "  box-shadow: 0px 14px 40px " FL_COLOR_GLASS_SHADOW ";"                     \
    "}"                                                                          \
    ".ca-overlay-hover { background: " FL_COLOR_CONTROL "; border-radius: " FL_RADIUS_SM "; }" \
    ".ca-overlay-selected { background: " FL_COLOR_SELECTION "; color: " FL_COLOR_TEXT_BRIGHT "; border-radius: " FL_RADIUS_SM "; }" \
    ".ca-dock-host {"                                                            \
    "  padding: " FL_PANEL_GAP "; background: transparent;"                      \
    "}"                                                                          \
    "splitter { gap: " FL_PANEL_GAP "; }"                                        \
    ".ca-dock-group {"                                                           \
    "  border-radius: " FL_RADIUS_LG ";"                                         \
    "  background: linear-gradient(to bottom, " FL_COLOR_GLASS_TOP ", " FL_COLOR_GLASS_BOTTOM ");" \
    "  border: 1px solid " FL_COLOR_GLASS_BORDER ";"                             \
    "  backdrop-filter: blur(" FL_BLUR_PANEL ");"                                \
    "}"                                                                          \
    ".ca-dock-group.ca-dock-empty { background: " FL_COLOR_FIELD "; }" \
    ".ca-dock-content { background: transparent; }"                              \
    ".ca-dock-tabs {"                                                            \
    "  height: " FL_TAB_BAR_HEIGHT "; gap: " FL_SPACE_XS ";"                     \
    "  padding: " FL_SPACE_SM "; margin: " FL_SPACE_SM ";" \
    "  flex-shrink: 0;"                                                          \
    "  background: linear-gradient(to bottom, " FL_COLOR_TAB_GUTTER_TOP ", " FL_COLOR_TAB_GUTTER_BOTTOM ");" \
    "  box-shadow: inset 0px 3px 8px " FL_COLOR_TAB_GUTTER_SHADE ";"             \
    "  border: 1px solid " FL_COLOR_TAB_GUTTER_EDGE ";"                          \
    "  border-top-color: " FL_COLOR_TAB_GUTTER_EDGE "; border-bottom-color: " FL_COLOR_TAB_GUTTER_LIP ";" \
    "  font-size: " FL_FONT_SM ";"                                               \
    "  border-radius: " FL_RADIUS_PILL ";"                                       \
    "}"                                                                          \
    ".ca-dock-tab {"                                                             \
    "  padding: 0px " FL_SPACE_LG "; color: " FL_COLOR_TEXT_MUTED "; flex-shrink: 0;" \
    "  border-radius: " FL_RADIUS_PILL ";"                                       \
    "}"                                                                          \
    ".ca-dock-tab:hover { color: " FL_COLOR_TEXT_BRIGHT "; }"                    \
    ".ca-dock-tab.active, .ca-dock-tab.active:hover { color: " FL_COLOR_TEXT_BRIGHT "; background: transparent; }" \
    ".ca-dock-tab-indicator {"                                                   \
    "  border-radius: " FL_RADIUS_PILL ";"                                       \
    "  background: linear-gradient(to bottom, " FL_COLOR_TAB_PILL_TOP ", " FL_COLOR_TAB_PILL_BOTTOM ");" \
    "  border: 1px solid " FL_COLOR_TAB_PILL_RIM ";"                             \
    "  border-top-color: " FL_COLOR_TAB_PILL_SPECULAR "; border-bottom-color: " FL_COLOR_TAB_PILL_UNDER ";" \
    "  box-shadow: 0px 1px 3px " FL_COLOR_TAB_PILL_CAST ";"                      \
    "  transition: left " FL_MOTION_TAB_SLIDE " " FL_MOTION_TAB_EASE ";"         \
    "  transition-stretch: " FL_MOTION_TAB_STRETCH ";"                           \
    "}"                                                                          \
    ".ca-dock-target {"                                                          \
    "  background: " FL_COLOR_FLOATING "; border: 1px solid " FL_COLOR_GLASS_BORDER ";" \
    "  border-radius: " FL_RADIUS_LG "; backdrop-filter: blur(" FL_BLUR_FLOAT ");" \
    "}"                                                                          \
    ".ca-dock-target.hot { border: 1px solid " FL_COLOR_ACCENT "; }"             \
    ".ca-dock-target-glyph.hot { background: " FL_COLOR_ACCENT "; }"             \
    ".ca-dock-preview {"                                                         \
    "  background: " FL_COLOR_SELECTION "; border: 1px solid " FL_COLOR_ACCENT ";" \
    "  border-radius: " FL_RADIUS_LG ";"                                         \
    "}"

/* Flow components. */
#define FL_STYLE_CSS_COMPONENTS                                                  \
    "." FL_CLASS_APP_ROOT " {"                                                   \
    "  width: 100%; height: 100%; padding: 0px; gap: 0px;"                       \
    "  " FL_STYLE_BACKDROP \
    "  color: " FL_COLOR_TEXT_MEDIUM "; font-size: " FL_FONT_MD ";"                                            \
    "}"                                                                          \
    "." FL_CLASS_SCENE " { width: 100%; height: 100%; background: transparent; }" \
    "." FL_CLASS_CONSOLE " {"                                                    \
    "  width: 100%; height: 100%; padding: " FL_SPACE_SM " " FL_SPACE_MD ";"     \
    "  gap: " FL_SPACE_XS "; overflow-y: auto; background: transparent;" \
    "  font-size: " FL_FONT_SM ";"                                               \
    "}"                                                                          \
    "." FL_CLASS_CONSOLE_LINE " { width: 100%; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-log-trace, .fl-log-debug { color: " FL_COLOR_TEXT_DIM "; }"             \
    ".fl-log-info { color: " FL_COLOR_TEXT_MEDIUM "; }"                          \
    ".fl-log-warn { color: " FL_COLOR_WARNING "; }"                              \
    ".fl-log-error, .fl-log-fatal { color: " FL_COLOR_DANGER "; }"

/* Complete editor stylesheet. */


/* Scene workspace and project manager metrics. */
#define FL_STYLE_HIERARCHY_W 260.0f
#define FL_STYLE_FORM_W 420
#define FL_STYLE_RENAME_H 150
#define FL_STYLE_LAUNCHER_W 760
#define FL_STYLE_LAUNCHER_H 580
#define FL_STYLE_SCENE_RECT_RGBA {91,156,255,255}
#define FL_STYLE_ARTBOARD_MARGIN 14.0
#define FL_STYLE_CANVAS_FRAME_RGBA {115, 145, 185, 190}
#define FL_STYLE_CANVAS_GRID_RGBA {105, 126, 156, 36}
#define FL_STYLE_CANVAS_GRID_MAJOR_RGBA {115, 142, 178, 64}
#define FL_STYLE_VIEW_CLEAR {0.0030f,0.0048f,0.0130f,1.0f}
#define FL_STYLE_ARTBOARD_CLEAR {0.0075f,0.0105f,0.0210f,1.0f}
#define FL_CLASS_TOOLBAR "fl-toolbar"
#define FL_CLASS_HEADING "fl-heading"
#define FL_CLASS_LABEL "fl-label"
#define FL_CLASS_MUTED "fl-muted"
#define FL_CLASS_SCENE_GUTTER "fl-scene-gutter fl-ambient"
#define FL_CLASS_SCENE_STRIP "fl-scene-strip"
#define FL_CLASS_SCENE_RIM "fl-scene-rim"

#define FL_SCENE_CARD_W "116px"
#define FL_SCENE_CARD_H "66px"
#define FL_SCENE_STRIP_H "74px"
#define FL_CLASS_CARD "fl-scene-card"
#define FL_CLASS_SCENE_DRAG "fl-scene-drag"
#define FL_CLASS_SCENE_THUMB "fl-scene-thumb"
#define FL_CLASS_SCENE_GLASS "fl-scene-glass"
#define FL_CLASS_SCENE_GLASS_SELECTED "fl-scene-glass selected"
#define FL_CLASS_SCENE_LABEL "fl-scene-label"
#define FL_CLASS_SCENE_NAME "fl-scene-name"
#define FL_CLASS_SCENE_TIME "fl-scene-time"
#define FL_CLASS_CARD_SELECTED "fl-scene-card selected"
#define FL_CLASS_HIERARCHY "fl-hierarchy"
#define FL_CLASS_HIERARCHY_PANE "fl-hierarchy-pane"
#define FL_CLASS_TREE_ROW "fl-tree-row"
#define FL_CLASS_TREE_ROW_SELECTED "fl-tree-row selected"
#define FL_CLASS_TREE_GUIDE "fl-tree-guide"
#define FL_CLASS_TREE_LINE "fl-tree-line"
#define FL_CLASS_TREE_ELBOW_TOP "fl-tree-elbow-top"
#define FL_CLASS_TREE_ELBOW_BOTTOM "fl-tree-elbow-bottom"
#define FL_CLASS_TREE_CHEVRON "fl-tree-chevron"
#define FL_CLASS_TREE_LEAF "fl-tree-leaf"
#define FL_CLASS_TREE_ICON "fl-tree-icon"
#define FL_CLASS_TREE_NAME "fl-tree-name"
#define FL_CLASS_TREE_NAME_MUTED "fl-tree-name dimmed"
#define FL_CLASS_TREE_EYE "fl-tree-eye"
#define FL_CLASS_TREE_EYE_MIXED "fl-tree-eye mixed"
#define FL_CLASS_TREE_EYE_HIDDEN "fl-tree-eye hidden"
#define FL_TREE_ROW_H "26px"
#define FL_TREE_HALF_H "13px"
#define FL_CLASS_VIEW "fl-view"
#define FL_CLASS_SCENE_BODY "fl-scene-body"
#define FL_CLASS_SCENE_TOOLBAR "fl-toolbar fl-scene-toolbar"
#define FL_CLASS_TOOLBAR_SPACER "fl-toolbar-spacer"
#define FL_CLASS_ASPECT_SELECT "fl-aspect-select"
#define FL_CLASS_VIEW_GUTTER "fl-view-gutter"
#define FL_CLASS_VIEW_RIM "fl-view-rim"
#define FL_CLASS_VIEWPORT "fl-viewport"
#define FL_CLASS_FORM "fl-form"
#define FL_CLASS_INPUT "fl-input"
#define FL_CLASS_LAUNCHER "fl-launcher"
#define FL_CLASS_LAUNCHER_TITLE "fl-launcher-title"
#define FL_CLASS_RECENT "fl-recent"
#define FL_STYLE_CSS_SCENE \
    ".fl-scene { gap: " FL_SPACE_SM "; overflow: hidden; }" \
    ".fl-toolbar { width: 100%; min-height: 32px; height: auto; padding: 4px 8px; gap: 8px; align-items: center; flex-shrink: 0; }" \
    ".fl-heading { color: " FL_COLOR_TEXT_BRIGHT "; font-size: " FL_FONT_MD "; }" \
    ".fl-label { color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-muted { color: " FL_COLOR_TEXT_DIM "; font-size: " FL_FONT_SM "; }" \
    ".fl-recent:hover { background: " FL_COLOR_CONTROL_HOVER "; }" \
    ".fl-ambient { ambient-light: " FL_AMBIENT_LIGHT "; }" \
    ".fl-scene-toolbar { height: 32px; }" \
    ".fl-scene-body { width: 100%; flex-grow: 1; min-height: 0px; }" \
    ".fl-scene-gutter { height: " FL_SCENE_STRIP_H "; margin: 0px " FL_SPACE_SM "; flex-shrink: 0; border-radius: " FL_RADIUS_CARD_GUTTER "; }" \
    ".fl-sunken { background: linear-gradient(to bottom, " FL_COLOR_TAB_GUTTER_TOP ", " FL_COLOR_TAB_GUTTER_BOTTOM "); }" \
    ".fl-scene-strip { width: 100%; height: 100%; padding: " FL_SPACE_SM "; gap: " FL_SPACE_SM "; overflow-x: auto; scrollbar-width: 0px; border-radius: " FL_RADIUS_CARD_GUTTER "; background: linear-gradient(to bottom, " FL_COLOR_TAB_GUTTER_TOP ", " FL_COLOR_TAB_GUTTER_BOTTOM "); }" \
    ".fl-scene-rim { left: 0px; top: 0px; width: 100%; height: 100%; z-index: 1; pointer-events: none; background: transparent; border-radius: " FL_RADIUS_CARD_GUTTER "; box-shadow: inset 0px 3px 8px " FL_COLOR_TAB_GUTTER_SHADE "; border: 1px solid " FL_COLOR_TAB_GUTTER_EDGE "; border-top-color: " FL_COLOR_TAB_GUTTER_EDGE "; border-bottom-color: " FL_COLOR_TAB_GUTTER_LIP "; }" \
    ".fl-scene-drag { width: " FL_SCENE_CARD_W "; height: " FL_SCENE_CARD_H "; flex-shrink: 0; cursor: grab; }" \
    ".fl-scene-card { width: 100%; height: 100%; flex-shrink: 0; overflow: hidden; gap: 0px; background: transparent; border-radius: " FL_RADIUS_CARD "; }" \
    ".fl-scene-card:hover { glow-radius: 8px; glow-color: " FL_COLOR_TAB_PILL_RIM "; }" \
    ".fl-scene-card.selected { glow-radius: 6px; glow-color: " FL_COLOR_CARD_SELECTED_GLOW "; }" \
    ".fl-scene-thumb { pointer-events: none; }" \
    ".fl-scene-glass { left: 0px; top: 0px; width: 100%; height: 100%; z-index: 1; pointer-events: none; border-radius: " FL_RADIUS_CARD "; background: linear-gradient(to right, " FL_COLOR_CARD_GLASS_TOP ", " FL_COLOR_CARD_GLASS_BOTTOM "); border: 1px solid " FL_COLOR_TAB_PILL_RIM "; border-top-color: " FL_COLOR_TAB_PILL_SPECULAR "; border-bottom-color: " FL_COLOR_TAB_PILL_UNDER "; }" \
    ".fl-scene-glass.selected { border-color: " FL_COLOR_CARD_SELECTED_RIM "; border-top-color: " FL_COLOR_CARD_SELECTED_SPECULAR "; }" \
    ".fl-scene-label { left: 0px; top: 32px; width: 100%; height: 34px; padding: 0px 10px; gap: 0px; justify-content: center; z-index: 1; background: transparent; pointer-events: none; }" \
    ".fl-scene-name { z-index: 1; color: " FL_COLOR_TEXT_BRIGHT "; font-size: " FL_FONT_SM "; }" \
    ".fl-scene-time { z-index: 1; color: " FL_COLOR_TEXT_MEDIUM "; font-size: " FL_FONT_XS "; }"

#define FL_STYLE_CSS_SCENE_PANES \
    ".fl-toolbar-spacer { flex-grow: 1; min-width: 0px; }" \
    ".fl-aspect-select { width: 96px; min-width: 60px; height: 24px; flex-shrink: 1; border-radius: " FL_RADIUS_PILL "; background: " FL_COLOR_FIELD "; color: " FL_COLOR_TEXT_BRIGHT "; }" \
    ".fl-hierarchy-pane { width: 100%; height: 100%; padding: 0px " FL_SPACE_SM " " FL_SPACE_SM "; gap: 0px; }" \
    ".fl-hierarchy { width: 100%; flex-grow: 1; min-height: 0px; overflow-y: auto; gap: 0px; background: transparent; }" \
    ".fl-view { width: 100%; height: 100%; padding: 0px " FL_SPACE_SM " " FL_SPACE_SM "; gap: 0px; overflow: hidden; }" \
    ".fl-view-gutter { width: 100%; flex-grow: 1; min-height: 0px; gap: 0px; border-radius: " FL_RADIUS_CARD_GUTTER "; }" \
    ".fl-view-rim { left: 0px; top: 0px; width: 100%; height: 100%; z-index: 1; pointer-events: none; background: transparent; border-radius: " FL_RADIUS_CARD_GUTTER "; box-shadow: inset 0px 2px 5px " FL_COLOR_TAB_GUTTER_SHADE "; border: 1px solid " FL_COLOR_TAB_GUTTER_EDGE "; border-top-color: " FL_COLOR_TAB_GUTTER_EDGE "; border-bottom-color: " FL_COLOR_TAB_GUTTER_LIP "; }" \
    ".fl-viewport { width: 100%; flex-grow: 1; min-height: 0px; border-radius: " FL_RADIUS_CARD_GUTTER "; }" \
    ".fl-form { width: 100%; height: 100%; padding: 16px; gap: 12px; " FL_STYLE_BACKDROP " }" \
    ".fl-input { width: 100%; height: 32px; padding: 6px 10px; flex-shrink: 0; background: " FL_COLOR_FIELD "; color: " FL_COLOR_TEXT_BRIGHT "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-launcher { width: 100%; height: 100%; padding: 28px; gap: 12px; overflow-y: auto; " FL_STYLE_BACKDROP " }" \
    ".fl-launcher-title { font-size: 32px; color: " FL_COLOR_TEXT_BRIGHT "; }" \
    ".fl-recent { width: 100%; height: 36px; padding: 8px 14px; flex-shrink: 0; justify-content: flex-start; color: " FL_COLOR_TEXT_MEDIUM "; background: " FL_COLOR_CONTROL "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_MD "; }"

#define FL_STYLE_CSS_TREE \
    ".fl-tree-row { width: 100%; height: " FL_TREE_ROW_H "; flex-shrink: 0; overflow: hidden; padding: 0px 4px 0px 2px; gap: 0px; align-items: center; background: transparent; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-tree-row:hover { background: " FL_COLOR_HOVER "; }" \
    ".fl-tree-row.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-tree-guide { width: 16px; height: " FL_TREE_ROW_H "; gap: 0px; flex-shrink: 0; background: transparent; }" \
    ".fl-tree-line { width: 9px; height: " FL_TREE_ROW_H "; margin-left: 7px; flex-shrink: 0; background: transparent; border-left: 1px solid " FL_COLOR_TREE_LINE "; }" \
    ".fl-tree-elbow-top { width: 9px; height: " FL_TREE_HALF_H "; margin-left: 7px; flex-shrink: 0; background: transparent; border-left: 1px solid " FL_COLOR_TREE_LINE "; border-bottom: 1px solid " FL_COLOR_TREE_LINE "; }" \
    ".fl-tree-elbow-bottom { width: 9px; height: " FL_TREE_HALF_H "; margin-left: 7px; flex-shrink: 0; background: transparent; border-left: 1px solid " FL_COLOR_TREE_LINE "; }" \
    ".fl-tree-chevron { width: 18px; height: 22px; padding: 0px; flex-shrink: 0; justify-content: center; align-items: center; background: transparent; color: " FL_COLOR_TEXT_DIM "; font-size: " FL_FONT_XS "; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-tree-chevron:hover { color: " FL_COLOR_TEXT_BRIGHT "; background: " FL_COLOR_CONTROL "; }" \
    ".fl-tree-leaf { width: 18px; height: 22px; flex-shrink: 0; background: transparent; }" \
    ".fl-tree-icon { width: 18px; flex-shrink: 0; color: " FL_COLOR_TEXT_DIM "; font-size: " FL_FONT_SM "; }" \
    ".fl-tree-name { flex-grow: 1; min-width: 0px; overflow: hidden; white-space: nowrap; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-tree-name.dimmed { color: " FL_COLOR_TEXT_DIM "; }" \
    ".fl-tree-eye { width: 24px; height: 22px; padding: 0px; flex-shrink: 0; justify-content: center; align-items: center; background: transparent; color: " FL_COLOR_TEXT_MEDIUM "; font-size: " FL_FONT_SM "; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-tree-eye:hover { color: " FL_COLOR_TEXT_BRIGHT "; background: " FL_COLOR_CONTROL "; }" \
    ".fl-tree-eye.mixed { color: " FL_COLOR_TREE_MIXED "; }" \
    ".fl-tree-eye.hidden { color: " FL_COLOR_TEXT_DIM "; }"

/* Scene authoring panels and viewport handles. */
#define FL_STYLE_WIDGETS_W 280.0f
#define FL_STYLE_INSPECTOR_W 280.0f
#define FL_STYLE_WIDGET_DRAG_THRESHOLD 4.0f
#define FL_STYLE_ROTATE_OFFSET 28.0
#define FL_STYLE_HANDLE_HIT 9.0
#define FL_STYLE_HANDLE_SIZE 7.0
#define FL_STYLE_OUTLINE_WIDTH 2.0
#define FL_STYLE_GIZMO_RGBA {91,156,255,255}
#define FL_CLASS_ENTITY_ROW "fl-entity-row"
#define FL_CLASS_STAGE "fl-stage fl-ambient"
#define FL_CLASS_LIBRARY "fl-library"
#define FL_CLASS_WIDGET_WRAP "fl-widget-wrap"
#define FL_CLASS_WIDGET_CARD "fl-widget-card"
#define FL_CLASS_WIDGET_ICON "fl-widget-icon"
#define FL_CLASS_INSPECTOR "fl-inspector"
#define FL_CLASS_PROPERTY_ROW "fl-property-row"
#define FL_CLASS_PROPERTY_LABEL "fl-property-label"
#define FL_CLASS_PROPERTY_LABEL_KEYED "fl-property-label keyed"
#define FL_CLASS_PROPERTY_INPUT "fl-property-input"
#define FL_STYLE_CSS_AUTHORING \
    ".fl-entity-row { width: 100%; flex-shrink: 0; }" \
    ".fl-stage { width: 100%; flex-grow: 1; min-height: 0px; overflow: hidden; border-radius: " FL_RADIUS_CARD_GUTTER "; }" \
    ".fl-library, .fl-inspector { width: 100%; height: 100%; padding: 10px; gap: 10px; overflow-y: auto; background: transparent; }" \
    ".fl-widget-wrap { min-width: 90px; flex-grow: 1; height: 72px; }" \
    ".fl-widget-card { width: 100%; height: 100%; padding: 8px 4px; gap: 6px; align-items: center; justify-content: center; background: " FL_COLOR_FIELD "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_LG "; }" \
    ".fl-widget-icon { font-size: 22px; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-widget-card:hover { border-color: " FL_COLOR_ACCENT "; background: " FL_COLOR_SELECTION "; glow-radius: 10px; glow-color: " FL_COLOR_ACCENT_GLOW "; }" \
    ".fl-property-row { width: 100%; height: 30px; flex-shrink: 0; gap: 8px; align-items: center; }" \
    ".fl-property-label { width: 76px; flex-shrink: 0; font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MUTED "; }" \
    ".fl-property-label.keyed { color: " FL_COLOR_ACCENT "; }" \
    ".fl-property-input { flex-grow: 1; min-width: 0px; height: 28px; padding: 4px 8px; background: " FL_COLOR_FIELD "; color: " FL_COLOR_TEXT_BRIGHT "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_SM "; }"

/* Assets panel, image placeholders and the Inspector font picker. */
#define FL_STYLE_ASSETS_W 240.0f
#define FL_STYLE_IMAGE_PLACEHOLDER_RGBA FL_RENDER_PLACEHOLDER_FILL_RGBA
#define FL_STYLE_IMAGE_PLACEHOLDER_STROKE_RGBA FL_RENDER_PLACEHOLDER_STROKE_RGBA
#define FL_STYLE_IMAGE_PLACEHOLDER_STROKE FL_RENDER_PLACEHOLDER_STROKE
#define FL_STYLE_FONT_LIST_LIMIT 300
#define FL_CLASS_ASSETS "fl-assets"
#define FL_CLASS_ASSET_GRID "fl-asset-grid"
#define FL_CLASS_ASSET_TILE "fl-asset-tile"
#define FL_CLASS_ASSET_CARD "fl-asset-card"
#define FL_CLASS_ASSET_CARD_SELECTED "fl-asset-card selected"
#define FL_CLASS_ASSET_PREVIEW "fl-asset-preview"
#define FL_CLASS_ASSET_ICON "fl-asset-icon"
#define FL_CLASS_ASSET_NAME "fl-asset-name"
#define FL_CLASS_CHIPS "fl-chips"
#define FL_CLASS_ASSET_PLAY "fl-asset-play"
#define FL_CLASS_EMPTY "fl-empty"
#define FL_CLASS_EMPTY_ICON "fl-empty-icon"
#define FL_STYLE_SCENE_THUMB_DISPLAY_W 116.0f
#define FL_STYLE_SCENE_THUMB_DISPLAY_H 66.0f
#define FL_STYLE_SCENE_THUMB_RADIUS 12.0f
#define FL_STYLE_SCENE_THUMB_W 232u
#define FL_STYLE_SCENE_THUMB_H 132u
#define FL_STYLE_SCENE_THUMB_CANVAS_RGBA {0,0,0,255}
#define FL_STYLE_SCENE_THUMB_ARTBOARD_RGBA {21,26,40,255}
#define FL_STYLE_SCENE_THUMB_SCRIM_START 0.45
#define FL_STYLE_SCENE_THUMB_SCRIM_STRENGTH 0.78
#define FL_STYLE_THUMBNAIL_PIXELS 192u
#define FL_STYLE_THUMBNAIL_W 82.0f
#define FL_STYLE_THUMBNAIL_H 56.0f
#define FL_STYLE_SPECIMEN_SCALE 0.4f
#define FL_CLASS_FONT_LIST "fl-font-list"
#define FL_CLASS_FONT_ROW "fl-font-row"
#define FL_CLASS_FONT_ROW_SELECTED "fl-font-row selected"
#define FL_CLASS_WARNING "fl-warning"
#define FL_CLASS_PROPERTY_SELECT "fl-property-select"
#define FL_STYLE_CSS_ASSETS \
    ".fl-assets { width: 100%; height: 100%; padding: 8px; gap: 8px; overflow-y: auto; background: transparent; }" \
    ".fl-asset-grid { width: 100%; flex-wrap: wrap; gap: 6px; flex-shrink: 0; }" \
    ".fl-asset-tile { width: 90px; height: 86px; flex-shrink: 0; }" \
    ".fl-asset-play { left: 8px; top: 38px; width: 24px; height: 24px; z-index: 1; }" \
    ".fl-asset-card { width: 100%; height: 100%; padding: 4px; gap: 4px; align-items: center; background: transparent; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-asset-card:hover, .fl-font-row:hover { background: " FL_COLOR_HOVER "; }" \
    ".fl-asset-card.selected, .fl-font-row.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-asset-preview { width: 82px; height: 56px; flex-shrink: 0; align-items: center; justify-content: center; overflow: hidden; background: " FL_COLOR_FIELD "; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-asset-icon { font-size: 26px; color: " FL_COLOR_TEXT_DIM "; }" \
    ".fl-asset-name { width: 100%; height: 16px; overflow: hidden; white-space: nowrap; text-align: center; font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-chips { width: 100%; height: 28px; gap: 2px; flex-shrink: 0; }" \
    ".fl-empty { width: 100%; padding: 28px 8px; gap: 8px; align-items: center; flex-shrink: 0; }" \
    ".fl-empty-icon { font-size: 30px; color: " FL_COLOR_TEXT_DIM "; }" \
    ".fl-font-list { width: 100%; height: 180px; flex-shrink: 0; overflow-y: auto; gap: 0px; background: " FL_COLOR_FIELD "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-font-row { width: 100%; height: 24px; flex-shrink: 0; padding: 4px 8px; gap: 8px; justify-content: space-between; background: transparent; }" \
    ".fl-warning { color: " FL_COLOR_WARNING "; font-size: " FL_FONT_SM "; text-wrap: wrap; }" \
    ".fl-property-select { flex-grow: 1; min-width: 0px; height: 28px; }"

/* In-app file browser window. */
#define FL_STYLE_FILE_BROWSER_W 720
#define FL_STYLE_FILE_BROWSER_H 520
#define FL_STYLE_FILE_BROWSER_LIMIT 1000
#define FL_CLASS_FILE_BROWSER "fl-file-browser"
#define FL_CLASS_FILE_LIST "fl-file-list"
#define FL_CLASS_FILE_ROW "fl-file-row"
#define FL_CLASS_FILE_ROW_SELECTED "fl-file-row selected"
#define FL_CLASS_FILE_FOOTER "fl-file-footer"
#define FL_CLASS_FILE_SUMMARY "fl-file-summary"
#define FL_CLASS_SCRIPTS "fl-scripts"
#define FL_CLASS_SCRIPT_LIST "fl-script-list"
#define FL_CLASS_SCRIPT_ROW "fl-script-row"
#define FL_CLASS_SCRIPT_ROW_SELECTED "fl-script-row selected"
#define FL_CLASS_SCRIPT_ICON "fl-script-icon"
#define FL_CLASS_SCRIPT_NAME "fl-script-name"
#define FL_CLASS_SCRIPT_ACTIONS "fl-script-actions"
#define FL_CLASS_SCRIPT_CODE "fl-script-code"
#define FL_CLASS_SCRIPT_SOURCE "fl-script-source"
#define FL_STYLE_CSS_SCRIPTS \
    ".fl-scripts { width: 100%; height: 100%; padding: 8px; gap: 8px; overflow-y: auto; background: transparent; }" \
    ".fl-script-list { width: 100%; gap: 2px; flex-shrink: 0; }" \
    ".fl-script-row { width: 100%; height: 28px; flex-shrink: 0; padding: 0px 8px; gap: 8px; align-items: center; background: transparent; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-script-row:hover { background: " FL_COLOR_HOVER "; }" \
    ".fl-script-row.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-script-icon { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MUTED "; flex-shrink: 0; }" \
    ".fl-script-name { flex-grow: 1; min-width: 0px; overflow: hidden; white-space: nowrap; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-script-actions { width: 100%; flex-wrap: wrap; gap: 6px; flex-shrink: 0; }" \
    ".fl-script-code { width: 100%; padding: 8px; flex-shrink: 0; background: " FL_COLOR_FIELD "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-script-source { width: 100%; color: " FL_COLOR_TEXT_MEDIUM "; font-size: " FL_FONT_SM "; }"
#define FL_STYLE_CSS_FILE_BROWSER \
    ".fl-file-browser { width: 100%; height: 100%; padding: 12px; gap: 8px; " FL_STYLE_BACKDROP " }" \
    ".fl-file-list { width: 100%; flex-grow: 1; min-height: 0px; overflow-y: auto; gap: 0px; background: " FL_COLOR_FIELD "; border: 1px solid " FL_COLOR_GLASS_BORDER "; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-file-row { width: 100%; height: 26px; flex-shrink: 0; padding: 4px 10px; gap: 8px; justify-content: space-between; background: transparent; }" \
    ".fl-file-row:hover { background: " FL_COLOR_HOVER "; }" \
    ".fl-file-row.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-file-footer { width: 100%; height: 40px; flex-shrink: 0; gap: 8px; align-items: center; }" \
    ".fl-file-summary { flex-grow: 1; min-width: 0px; color: " FL_COLOR_TEXT_MUTED "; font-size: " FL_FONT_SM "; }"

/* Timeline panel. */
#define FL_STYLE_TL_LABEL_W 240.0f
#define FL_STYLE_TL_PAD 12.0f
#define FL_STYLE_TL_ROW_H 28.0f
#define FL_STYLE_TL_LANE_H 28.0f
#define FL_STYLE_TL_RULER_H 20.0f
#define FL_STYLE_TL_HEADER_H (FL_STYLE_TL_RULER_H + FL_STYLE_TL_LANE_H)
#define FL_STYLE_TL_PLAYHEAD_W 2.0f
#define FL_STYLE_TL_KEY 14.0f
#define FL_STYLE_TL_EDGE_W 14.0f
#define FL_STYLE_TL_SNAP 6.0
#define FL_STYLE_TL_ZOOM 120.0
#define FL_STYLE_TL_ZOOM_MIN 8.0
#define FL_STYLE_TL_ZOOM_MAX 2400.0
#define FL_STYLE_TL_BEAT_DEFAULT ((uint64_t)FL_TICKS_PER_SECOND * 3)
#define FL_STYLE_TL_BEAT_MIN ((Fl_Tick)FL_TICKS_PER_SECOND / 30)
#define FL_STYLE_TL_FADE ((uint64_t)FL_TICKS_PER_SECOND / 2)
#define FL_STYLE_TIMELINE_HEIGHT 240.0f
#define FL_CLASS_TIMELINE "fl-timeline"
#define FL_CLASS_TL_SELECT "fl-tl-select"
#define FL_CLASS_TL_TRACK_NAME "fl-tl-track-name"
#define FL_CLASS_TL_CLOCK "fl-tl-clock"
#define FL_CLASS_TL_SPACER "fl-tl-spacer"
#define FL_CLASS_TL_SCROLL "fl-tl-scroll"
#define FL_CLASS_TL_BODY "fl-tl-body"
#define FL_CLASS_TL_LABEL_VIEWPORT "fl-tl-label-viewport"
#define FL_CLASS_TL_LABEL_CONTENT "fl-tl-label-content"
#define FL_CLASS_TL_KEY_CONTENT "fl-tl-key-content"
#define FL_CLASS_TL_LABELS "fl-tl-labels"
#define FL_CLASS_TL_LABEL "fl-tl-label"
#define FL_CLASS_TL_LABEL_SELECTED "fl-tl-label selected"
#define FL_CLASS_TL_LABEL_TEXT "fl-tl-label-text"
#define FL_CLASS_TL_LANE_LABEL "fl-tl-lane-label"
#define FL_CLASS_TL_LANE_LABEL_TEXT "fl-tl-lane-label-text"
#define FL_CLASS_TL_LANES "fl-tl-lanes"
#define FL_CLASS_TL_BEATLABE_SEGMENTS "fl-tl-beat-segments"
#define FL_CLASS_TL_CONTENT "fl-tl-content"
#define FL_CLASS_TL_ROW "fl-tl-row"
#define FL_CLASS_TL_BEAT "fl-tl-beat"
#define FL_CLASS_TL_BEAT_SELECTED "fl-tl-beat selected"
#define FL_CLASS_TL_BEAT_LABEL "fl-tl-beat-label"
#define FL_CLASS_TL_BEAT_TEXT "fl-tl-beat-text"
#define FL_CLASS_TL_BEAT_DURATION "fl-tl-beat-duration"
#define FL_CLASS_TL_BEAT_EMPTY "fl-tl-beat-empty"
#define FL_CLASS_TL_EDGE "fl-tl-edge"
#define FL_CLASS_TL_BEAT_DROP "fl-tl-beat-drop"
#define FL_CLASS_TL_EDGE_MARK "fl-tl-edge-mark"
#define FL_CLASS_TL_SPAN "fl-tl-span"
#define FL_CLASS_TL_KEY "fl-tl-key"
#define FL_CLASS_TL_KEY_SELECTED "fl-tl-key selected"
#define FL_CLASS_TL_KEY_TEXT "fl-tl-key-text"
#define FL_CLASS_TL_KEY_TEXT_SELECTED "fl-tl-key-text selected"
#define FL_CLASS_TL_KEY_DETAIL "fl-tl-key-detail"
#define FL_CLASS_TL_CLIP "fl-tl-clip"
#define FL_CLASS_TL_CLIP_SELECTED "fl-tl-clip selected"
#define FL_CLASS_TL_PLAYHEAD "fl-tl-playhead"
#define FL_CLASS_TL_NEEDLE "fl-tl-needle"
#define FL_STYLE_CSS_TIMELINE \
    ".fl-timeline { width: 100%; height: 100%; gap: 0px; overflow: hidden; background: transparent; padding: 0px " FL_SPACE_SM " " FL_SPACE_SM "; }" \
    ".fl-tl-transport, .fl-tl-authoring { width: 100%; height: 34px; flex-shrink: 0; gap: 6px; padding: 0px 4px; align-items: center; overflow: hidden; }" \
    ".fl-tl-selection { width: 100%; min-width: 0px; flex-shrink: 1; overflow: hidden; white-space: nowrap; font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-tl-header, .fl-tl-label-header { height: 48px; flex-shrink: 0; overflow: hidden; gap: 0px; }" \
    ".fl-tl-label-header { width: 100%; padding: 0px; }" \
    ".fl-tl-object-header { width: 100%; flex-shrink: 0; padding: 0px 10px; align-items: center; overflow: hidden; }" \
    ".fl-tl-tick { background: " FL_COLOR_GLASS_BORDER "; }" \
    ".fl-tl-tick.major { background: " FL_COLOR_TEXT_DIM "; }" \
    ".fl-tl-tick-text { font-size: " FL_FONT_XS "; color: " FL_COLOR_TEXT_DIM "; white-space: nowrap; }" \
    ".fl-tl-select { width: 112px; flex-shrink: 0; }" \
    ".fl-tl-track-name { flex-grow: 1; min-width: 0px; overflow: hidden; white-space: nowrap; font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-tl-clock { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; padding: 0px 8px; flex-shrink: 0; }" \
    ".fl-tl-spacer { flex-grow: 1; height: 1px; }" \
    ".fl-tl-scroll { width: 100%; flex-grow: 1; min-height: 0px; overflow: auto; gap: 0px; }" \
    ".fl-tl-labels { width: 240px; flex-shrink: 0; gap: 0px; }" \
    ".fl-tl-label-viewport { width: 100%; flex-grow: 1; min-height: 0px; overflow: hidden; }" \
    ".fl-tl-label-content, .fl-tl-key-content { flex-shrink: 0; gap: 0px; }" \
    ".fl-tl-lane-label { width: 100%; height: 28px; flex-shrink: 0; padding: 0px 10px; align-items: center; }" \
    ".fl-tl-lane-label-text { font-size: " FL_FONT_XS "; color: " FL_COLOR_TEXT_DIM "; white-space: nowrap; }" \
    ".fl-tl-label { width: 100%; height: 28px; flex-shrink: 0; padding: 0px 10px; gap: 4px; align-items: center; overflow: hidden; }" \
    ".fl-tl-row.selected, .fl-tl-label.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-tl-label-text { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; white-space: nowrap; }" \
    ".fl-tl-lanes { flex-grow: 1; min-width: 0px; min-height: 0px; overflow: hidden; gap: 0px; }" \
    ".fl-tl-beat-segments { width: 100%; height: 28px; flex-shrink: 0; overflow: hidden; gap: 0px; }" \
    ".fl-tl-content { flex-shrink: 0; gap: 0px; }" \
    ".fl-tl-row { height: 28px; flex-shrink: 0; }" \
    ".fl-tl-beat-empty { height: 28px; padding: 0px 10px; align-items: center; }" \
    ".fl-tl-beat { cursor: grab; }" \
    ".fl-tl-beat:active { cursor: grabbing; }" \
    ".fl-tl-beat-label { left: 0px; top: 0px; width: 100%; height: 100%; z-index: 1; padding: 0px 8px; gap: 6px; align-items: center; justify-content: space-between; background: transparent; pointer-events: none; }" \
    ".fl-tl-beat:hover { background: " FL_COLOR_CONTROL_HOVER "; }" \
    ".fl-tl-beat.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-tl-beat-text { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; white-space: nowrap; overflow: hidden; }" \
    ".fl-tl-beat-duration { font-size: " FL_FONT_XS "; color: " FL_COLOR_TEXT_DIM "; white-space: nowrap; flex-shrink: 0; }"
#define FL_STYLE_CSS_TIMELINE_BEATS \
    ".fl-tl-body { position: relative; width: 100%; flex-grow: 1; min-height: 0px; overflow: hidden; gap: 0px; padding: 0px; border-radius: " FL_RADIUS_CARD_GUTTER "; box-shadow: inset 0px 3px 8px " FL_COLOR_TAB_GUTTER_SHADE "; background: linear-gradient(to bottom, " FL_COLOR_TAB_GUTTER_TOP ", " FL_COLOR_TAB_GUTTER_BOTTOM "); }" \
    ".fl-tl-time-ruler { height: 20px; flex-shrink: 0; overflow: hidden; background: transparent; }" \
    ".fl-tl-needle { z-index: 0; background: " FL_COLOR_DANGER "; cursor: ew-resize; }" \
    ".fl-tl-beat { z-index: 1; backdrop-filter: blur(2px); padding: 0px; align-items: center; overflow: hidden; border-radius: " FL_RADIUS_SM "; background: linear-gradient(to bottom, " FL_COLOR_CARD_GLASS_TOP ", " FL_COLOR_CARD_GLASS_BOTTOM "); border: 1px solid " FL_COLOR_TAB_PILL_RIM "; border-top-color: " FL_COLOR_TAB_PILL_SPECULAR "; border-bottom-color: " FL_COLOR_TAB_PILL_UNDER "; }" \
    ".fl-tl-clip { padding: 0px 8px; align-items: center; overflow: hidden; justify-content: flex-start; gap: 6px; border-radius: " FL_RADIUS_SM "; background: " FL_COLOR_CONTROL "; border: 1px solid " FL_COLOR_GLASS_BORDER "; }" \
    ".fl-tl-clip.selected { border-color: " FL_COLOR_ACCENT "; background: " FL_COLOR_SELECTION "; glow-radius: 8px; glow-color: " FL_COLOR_ACCENT_GLOW "; }" \
    ".fl-tl-beat-text { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; white-space: nowrap; overflow: hidden; }" \
    ".fl-tl-beat-duration { font-size: " FL_FONT_XS "; color: " FL_COLOR_TEXT_DIM "; white-space: nowrap; flex-shrink: 0; }" \
    ".fl-tl-beat-drop { background: " FL_COLOR_ACCENT "; z-index: 5; }" \
    ".fl-tl-edge { z-index: 3; cursor: ew-resize; align-items: center; justify-content: center; background: transparent; }" \
    ".fl-tl-edge-mark { width: 2px; height: 20px; background: " FL_COLOR_TEXT_DIM "; }" \
    ".fl-tl-span { background: " FL_COLOR_CONTROL "; }" \
    ".fl-tl-key { align-items: center; justify-content: center; }" \
    ".fl-tl-key-text { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MUTED "; }" \
    ".fl-tl-key-text.selected { color: " FL_COLOR_ACCENT "; }" \
    ".fl-tl-key-detail { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; white-space: nowrap; flex-shrink: 0; }" \
    ".fl-tl-playhead-viewport { overflow: hidden; z-index: 5; pointer-events: none; }" \
    ".fl-tl-playhead { z-index: 5; background: " FL_COLOR_DANGER "; cursor: ew-resize; }"


/* Motion: hover and selection fades shared by interactive controls. */
#define FL_STYLE_CSS_MOTION \
    ".ca-titlebar-menu-item, .ca-titlebar-control, .ca-popup-btn, .ca-dock-tab, .fl-button, .fl-button-icon, " \
    ".fl-recent, .fl-tree-row, .fl-widget-card, .fl-asset-card, .fl-font-row, .fl-file-row, " \
    ".fl-scene-card, .fl-script-row { transition: " FL_MOTION_FADE ", " FL_MOTION_FADE_BORDER "; }"
