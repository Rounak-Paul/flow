#pragma once

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
   1. THEME TOKENS — "Flow Dark"
   ===================================================================== */

#define FL_THEME_NAME "Flow Dark"

/* Surfaces, from deepest to most raised. */
#define FL_COLOR_BG_VOID        "#08090c"
#define FL_COLOR_BG_BASE        "#0e1014"
#define FL_COLOR_BG_ELEVATED    "#14171c"
#define FL_COLOR_BG_SURFACE     "#1b1f26"
#define FL_COLOR_BG_OVERLAY     "#252a33"
#define FL_COLOR_SEPARATOR      "#1f232b"

/* Text, from most to least prominent. */
#define FL_COLOR_TEXT_BRIGHT    "#e6e9ee"
#define FL_COLOR_TEXT_MEDIUM    "#b4bcc8"
#define FL_COLOR_TEXT_MUTED     "#8a94a3"
#define FL_COLOR_TEXT_DIM       "#5a6372"

/* Accent and interaction states. */
#define FL_COLOR_ACCENT         "#5b9cff"
#define FL_COLOR_ACCENT_HOVER   "#7ab0ff"
#define FL_COLOR_ON_ACCENT      "#06080d"
#define FL_COLOR_HOVER          "rgba(255, 255, 255, 0.05)"
#define FL_COLOR_PRESSED        "rgba(255, 255, 255, 0.09)"
#define FL_COLOR_SELECTION      "rgba(91, 156, 255, 0.26)"

/* Status. */
#define FL_COLOR_SUCCESS        "#4cc38a"
#define FL_COLOR_WARNING        "#e8b04a"
#define FL_COLOR_DANGER         "#f0707a"
#define FL_COLOR_ON_DANGER      "#0b0b0d"
#define FL_COLOR_DANGER_HOVER   "rgba(240, 112, 122, 0.20)"

/* Overlays and floating surfaces. */
#define FL_COLOR_SCRIM          "rgba(4, 5, 8, 0.80)"
#define FL_COLOR_FLOATING       "#161a20"
#define FL_COLOR_SCROLL_TRACK   "rgba(8, 9, 12, 0.30)"
#define FL_COLOR_SCROLL_THUMB   "rgba(138, 148, 163, 0.35)"
#define FL_COLOR_SCROLL_ACTIVE  "rgba(160, 172, 190, 0.55)"

/* Spacing scale. */
#define FL_SPACE_XS             "2px"
#define FL_SPACE_SM             "4px"
#define FL_SPACE_MD             "8px"
#define FL_SPACE_LG             "12px"
#define FL_SPACE_XL             "16px"

/* Corner radii. */
#define FL_RADIUS_SM            "3px"
#define FL_RADIUS_MD            "5px"
#define FL_RADIUS_LG            "8px"

/* Type scale (logical pixels; causality applies the UI scale). */
#define FL_FONT_XS              "10px"
#define FL_FONT_SM              "11px"
#define FL_FONT_MD              "12px"
#define FL_FONT_LG              "14px"
#define FL_FONT_XL              "16px"

/* Chrome metrics. */
#define FL_TITLEBAR_HEIGHT      "30px"
#define FL_SCROLLBAR_WIDTH      "8px"

/* =====================================================================
   2. PRESENTATION DEFAULTS
   ===================================================================== */

#define FL_STYLE_APP_NAME          "Flow"

/* Size the main window returns to when un-maximized; it opens maximized. */
#define FL_STYLE_WINDOW_RESTORE_W  1024
#define FL_STYLE_WINDOW_RESTORE_H  640

/* Instance-wide UI scale (1.0 = 100%). */
#define FL_STYLE_UI_SCALE          1.0f

/* =====================================================================
   3. CLASS NAMES
   ===================================================================== */

#define FL_CLASS_APP_ROOT  "fl-app-root"

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

/* Causality system chrome: title bar, popups, menus, tooltips, overlays. */
#define FL_STYLE_CSS_SYSTEM                                                      \
    ".ca-titlebar {"                                                             \
    "  height: " FL_TITLEBAR_HEIGHT "; background: " FL_COLOR_BG_VOID ";"        \
    "}"                                                                          \
    ".ca-titlebar-menu { height: 100%; align-items: center; background: transparent; }" \
    ".ca-titlebar-menu-item {"                                                   \
    "  height: 100%; padding: 0px " FL_SPACE_MD "; color: " FL_COLOR_TEXT_MEDIUM ";" \
    "  background: transparent; border-radius: 0px; font-size: " FL_FONT_SM ";"  \
    "}"                                                                          \
    ".ca-titlebar-menu-item:hover {"                                             \
    "  background: " FL_COLOR_HOVER "; color: " FL_COLOR_TEXT_BRIGHT ";"         \
    "}"                                                                          \
    ".ca-titlebar-drag { height: 100%; background: transparent; }"               \
    ".ca-titlebar-title { color: " FL_COLOR_TEXT_MUTED "; font-size: " FL_FONT_SM "; }" \
    ".ca-titlebar-controls { height: 100%; gap: " FL_SPACE_SM "; align-items: center; }" \
    ".ca-titlebar-control {"                                                     \
    "  width: 28px; height: 22px; color: " FL_COLOR_TEXT_MEDIUM ";"              \
    "  background: transparent; border-radius: " FL_RADIUS_SM "; font-size: " FL_FONT_XS ";" \
    "}"                                                                          \
    ".ca-titlebar-control:hover { background: " FL_COLOR_HOVER "; }"             \
    ".ca-titlebar-close { color: " FL_COLOR_DANGER "; }"                         \
    ".ca-titlebar-close:hover { background: " FL_COLOR_DANGER_HOVER "; }"        \
    ".ca-popup-root {"                                                           \
    "  width: 100%; height: 100%; padding: " FL_SPACE_XL ";"                     \
    "  background: " FL_COLOR_SCRIM "; align-items: stretch; justify-content: center;" \
    "}"                                                                          \
    ".ca-popup-card {"                                                           \
    "  padding: " FL_SPACE_XL "; gap: " FL_SPACE_LG "; background: " FL_COLOR_FLOATING ";" \
    "  border-radius: " FL_RADIUS_LG ";"                                         \
    "}"                                                                          \
    ".ca-popup-title { color: " FL_COLOR_TEXT_BRIGHT "; font-size: " FL_FONT_LG "; }" \
    ".ca-popup-message { color: " FL_COLOR_TEXT_MEDIUM "; font-size: " FL_FONT_MD "; text-wrap: wrap; }" \
    ".ca-popup-actions { gap: " FL_SPACE_MD "; justify-content: flex-end; }"     \
    ".ca-popup-btn {"                                                            \
    "  padding: " FL_SPACE_SM " " FL_SPACE_LG "; color: " FL_COLOR_TEXT_BRIGHT ";" \
    "  background: " FL_COLOR_BG_OVERLAY "; border-radius: " FL_RADIUS_MD ";"    \
    "}"                                                                          \
    ".ca-popup-btn:hover { background: " FL_COLOR_SELECTION "; }"                \
    ".ca-select-popup, .ca-tooltip, .ca-context-menu, .ca-menubar-popup {"       \
    "  background: " FL_COLOR_FLOATING "; color: " FL_COLOR_TEXT_BRIGHT ";"      \
    "  border-radius: " FL_RADIUS_MD ";"                                         \
    "}"                                                                          \
    ".ca-overlay-hover { background: " FL_COLOR_HOVER "; border-radius: " FL_RADIUS_SM "; }" \
    ".ca-overlay-selected { background: " FL_COLOR_SELECTION "; color: " FL_COLOR_TEXT_BRIGHT "; }"

/* Flow components. */
#define FL_STYLE_CSS_COMPONENTS                                                  \
    "." FL_CLASS_APP_ROOT " {"                                                   \
    "  width: 100%; height: 100%; padding: 0px; gap: 0px;"                       \
    "  background: " FL_COLOR_BG_BASE "; color: " FL_COLOR_TEXT_MEDIUM ";"       \
    "  font-size: " FL_FONT_MD ";"                                               \
    "}"

/* Complete editor stylesheet. */
#define FL_STYLE_CSS FL_STYLE_CSS_PALETTE FL_STYLE_CSS_SYSTEM FL_STYLE_CSS_COMPONENTS
