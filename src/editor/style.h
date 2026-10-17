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
#define FL_TAB_BAR_HEIGHT       "30px"
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
    ".ca-overlay-selected { background: " FL_COLOR_SELECTION "; color: " FL_COLOR_TEXT_BRIGHT "; }" \
    ".ca-dock-host, .ca-dock-group { background: " FL_COLOR_BG_BASE "; }"          \
    ".ca-dock-group.ca-dock-empty { background: " FL_COLOR_BG_VOID "; }"         \
    ".ca-dock-tabs {"                                                            \
    "  height: " FL_TAB_BAR_HEIGHT "; gap: " FL_SPACE_XS ";"                     \
    "  padding: " FL_SPACE_SM " " FL_SPACE_SM " 0px " FL_SPACE_SM ";"            \
    "  background: " FL_COLOR_BG_ELEVATED "; font-size: " FL_FONT_SM ";"         \
    "}"                                                                          \
    ".ca-dock-tab {"                                                             \
    "  padding: 0px " FL_SPACE_LG "; color: " FL_COLOR_TEXT_DIM ";"              \
    "  border-top-left-radius: " FL_RADIUS_MD "; border-top-right-radius: " FL_RADIUS_MD ";" \
    "}"                                                                          \
    ".ca-dock-tab:hover { color: " FL_COLOR_TEXT_BRIGHT "; background: " FL_COLOR_HOVER "; }" \
    ".ca-dock-tab.active, .ca-dock-tab.active:hover {"                           \
    "  color: " FL_COLOR_TEXT_BRIGHT "; background: " FL_COLOR_BG_BASE ";"       \
    "}"                                                                          \
    ".ca-dock-target { background: " FL_COLOR_FLOATING "; border-radius: " FL_RADIUS_LG "; }" \
    ".ca-dock-target.hot { border: 1px solid " FL_COLOR_ACCENT "; }"             \
    ".ca-dock-target-glyph.hot, .ca-dock-preview { background: " FL_COLOR_ACCENT "; }" \
    ".ca-dock-preview { border-radius: " FL_RADIUS_LG "; }"

/* Flow components. */
#define FL_STYLE_CSS_COMPONENTS                                                  \
    "." FL_CLASS_APP_ROOT " {"                                                   \
    "  width: 100%; height: 100%; padding: 0px; gap: 0px;"                       \
    "  background: " FL_COLOR_BG_BASE "; color: " FL_COLOR_TEXT_MEDIUM ";"       \
    "  font-size: " FL_FONT_MD ";"                                               \
    "}"                                                                          \
    "." FL_CLASS_SCENE " { width: 100%; height: 100%; background: " FL_COLOR_BG_VOID "; }" \
    "." FL_CLASS_CONSOLE " {"                                                    \
    "  width: 100%; height: 100%; padding: " FL_SPACE_SM " " FL_SPACE_MD ";"     \
    "  gap: " FL_SPACE_XS "; overflow-y: auto; background: " FL_COLOR_BG_BASE ";" \
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
#define FL_STYLE_TREE_INDENT 14.0f
#define FL_STYLE_FORM_W 420
#define FL_STYLE_RENAME_H 150
#define FL_STYLE_LAUNCHER_W 760
#define FL_STYLE_LAUNCHER_H 580
#define FL_STYLE_SCENE_RECT_RGBA {91,156,255,255}
#define FL_STYLE_VIEW_CLEAR {0.002428f,0.002732f,0.003677f,1.0f}
#define FL_STYLE_ARTBOARD_CLEAR {0.006995f,0.008568f,0.011612f,1.0f}
#define FL_CLASS_TOOLBAR "fl-toolbar"
#define FL_CLASS_HEADING "fl-heading"
#define FL_CLASS_LABEL "fl-label"
#define FL_CLASS_MUTED "fl-muted"
#define FL_CLASS_ACTION "fl-action"
#define FL_CLASS_SCENE_STRIP "fl-scene-strip"
#define FL_CLASS_CARD "fl-scene-card"
#define FL_CLASS_CARD_SELECTED "fl-scene-card selected"
#define FL_CLASS_HIERARCHY "fl-hierarchy"
#define FL_CLASS_ENTITY "fl-entity"
#define FL_CLASS_ENTITY_SELECTED "fl-entity selected"
#define FL_CLASS_VIEW "fl-view"
#define FL_CLASS_VIEW_TITLE "fl-view-title"
#define FL_CLASS_VIEWPORT "fl-viewport"
#define FL_CLASS_FORM "fl-form"
#define FL_CLASS_INPUT "fl-input"
#define FL_CLASS_LAUNCHER "fl-launcher"
#define FL_CLASS_LAUNCHER_TITLE "fl-launcher-title"
#define FL_CLASS_RECENT "fl-recent"
#define FL_STYLE_CSS_SCENE \
    ".fl-scene { gap: 0px; overflow: hidden; }" \
    ".fl-toolbar { width: 100%; min-height: 32px; height: auto; padding: 4px 8px; gap: 8px; align-items: center; flex-shrink: 0; }" \
    ".fl-heading { color: " FL_COLOR_TEXT_BRIGHT "; font-size: " FL_FONT_MD "; }" \
    ".fl-label { color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-muted { color: " FL_COLOR_TEXT_DIM "; font-size: " FL_FONT_SM "; }" \
    ".fl-action { padding: 5px 10px; background: " FL_COLOR_BG_OVERLAY "; border-radius: " FL_RADIUS_SM "; color: " FL_COLOR_TEXT_BRIGHT "; flex-shrink: 0; }" \
    ".fl-action:hover, .fl-recent:hover { background: " FL_COLOR_SELECTION "; }" \
    ".fl-scene-strip { width: 100%; height: 104px; flex-shrink: 0; padding: 8px; gap: 8px; overflow-x: auto; background: " FL_COLOR_BG_ELEVATED "; }" \
    ".fl-scene-card { width: 164px; height: 82px; flex-shrink: 0; overflow: hidden; padding: 12px; gap: 12px; background: " FL_COLOR_BG_SURFACE "; border: 1px solid " FL_COLOR_SEPARATOR "; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-scene-card.selected { border-color: " FL_COLOR_ACCENT "; background: " FL_COLOR_SELECTION "; }" \
    ".fl-hierarchy { width: 100%; height: 100%; overflow-y: auto; gap: 4px; padding: 8px 0px; background: " FL_COLOR_BG_BASE "; }" \
    ".fl-entity { width: 100%; height: 28px; flex-shrink: 0; overflow: hidden; padding: 4px 8px; background: transparent; gap: 0px; }" \
    ".fl-entity:hover { background: " FL_COLOR_HOVER "; }" \
    ".fl-entity.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-view { width: 100%; height: 100%; gap: 0px; overflow: hidden; }" \
    ".fl-view-title { height: 30px; padding: 8px; flex-shrink: 0; color: " FL_COLOR_TEXT_MUTED "; }" \
    ".fl-viewport { width: 100%; flex-grow: 1; min-height: 0px; }" \
    ".fl-form { width: 100%; height: 100%; padding: 16px; gap: 12px; background: " FL_COLOR_BG_BASE "; }" \
    ".fl-input { width: 100%; height: 32px; padding: 6px 8px; flex-shrink: 0; background: " FL_COLOR_BG_SURFACE "; color: " FL_COLOR_TEXT_BRIGHT "; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-launcher { width: 100%; height: 100%; padding: 28px; gap: 12px; overflow-y: auto; background: " FL_COLOR_BG_BASE "; }" \
    ".fl-launcher-title { font-size: 28px; color: " FL_COLOR_TEXT_BRIGHT "; }" \
    ".fl-recent { width: 100%; height: 36px; padding: 8px; flex-shrink: 0; justify-content: flex-start; color: " FL_COLOR_TEXT_MEDIUM "; background: " FL_COLOR_BG_ELEVATED "; }"

/* Scene authoring panels and viewport handles. */
#define FL_STYLE_WIDGETS_W 210.0f
#define FL_STYLE_INSPECTOR_W 280.0f
#define FL_STYLE_WIDGET_DRAG_THRESHOLD 4.0f
#define FL_STYLE_ROTATE_OFFSET 28.0
#define FL_STYLE_HANDLE_HIT 9.0
#define FL_STYLE_HANDLE_SIZE 7.0
#define FL_STYLE_OUTLINE_WIDTH 2.0
#define FL_STYLE_GIZMO_RGBA {91,156,255,255}
#define FL_CLASS_ENTITY_ROW "fl-entity-row"
#define FL_CLASS_STAGE "fl-stage"
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
    ".fl-entity-row { width: 100%; height: 28px; flex-shrink: 0; }" \
    ".fl-stage { width: 100%; flex-grow: 1; min-height: 0px; overflow: hidden; }" \
    ".fl-library, .fl-inspector { width: 100%; height: 100%; padding: 10px; gap: 10px; overflow-y: auto; background: " FL_COLOR_BG_BASE "; }" \
    ".fl-widget-wrap { width: 90px; height: 72px; flex-shrink: 0; }" \
    ".fl-widget-card { width: 100%; height: 100%; padding: 8px 4px; gap: 6px; align-items: center; justify-content: center; background: " FL_COLOR_BG_SURFACE "; border: 1px solid " FL_COLOR_SEPARATOR "; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-widget-icon { font-size: 22px; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-widget-card:hover { border-color: " FL_COLOR_ACCENT "; background: " FL_COLOR_SELECTION "; }" \
    ".fl-property-row { width: 100%; height: 30px; flex-shrink: 0; gap: 8px; align-items: center; }" \
    ".fl-property-label { width: 76px; flex-shrink: 0; font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MUTED "; }" \
    ".fl-property-label.keyed { color: " FL_COLOR_ACCENT "; }" \
    ".fl-property-input { flex-grow: 1; min-width: 0px; height: 28px; padding: 4px 6px; background: " FL_COLOR_BG_SURFACE "; color: " FL_COLOR_TEXT_BRIGHT "; border-radius: " FL_RADIUS_SM "; }"

/* Assets panel, image placeholders and the Inspector font picker. */
#define FL_STYLE_ASSETS_W 240.0f
#define FL_STYLE_IMAGE_PLACEHOLDER_RGBA {38,44,54,255}
#define FL_STYLE_IMAGE_PLACEHOLDER_STROKE_RGBA {88,98,114,255}
#define FL_STYLE_IMAGE_PLACEHOLDER_STROKE 2.0
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
#define FL_CLASS_CHIP "fl-chip"
#define FL_CLASS_CHIP_SELECTED "fl-chip selected"
#define FL_CLASS_PLAY "fl-play"
#define FL_CLASS_PLAY_ACTIVE "fl-play active"
#define FL_CLASS_EMPTY "fl-empty"
#define FL_CLASS_EMPTY_ICON "fl-empty-icon"
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
    ".fl-assets { width: 100%; height: 100%; padding: 8px; gap: 8px; overflow-y: auto; background: " FL_COLOR_BG_BASE "; }" \
    ".fl-asset-grid { width: 100%; flex-wrap: wrap; gap: 6px; flex-shrink: 0; }" \
    ".fl-asset-tile { width: 90px; height: 86px; flex-shrink: 0; }" \
    ".fl-play { position: absolute; left: 8px; top: 38px; width: 22px; height: 22px; z-index: 1; font-size: " FL_FONT_XS "; border-radius: 11px; background: " FL_COLOR_BG_OVERLAY "; color: " FL_COLOR_TEXT_BRIGHT "; }" \
    ".fl-play:hover, .fl-play.active { background: " FL_COLOR_ACCENT "; color: " FL_COLOR_ON_ACCENT "; }" \
    ".fl-asset-card { width: 100%; height: 100%; padding: 4px; gap: 4px; align-items: center; background: transparent; border-radius: " FL_RADIUS_MD "; }" \
    ".fl-asset-card:hover, .fl-font-row:hover, .fl-chip:hover { background: " FL_COLOR_HOVER "; }" \
    ".fl-asset-card.selected, .fl-font-row.selected, .fl-chip.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-asset-preview { width: 82px; height: 56px; flex-shrink: 0; align-items: center; justify-content: center; overflow: hidden; background: " FL_COLOR_BG_SURFACE "; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-asset-icon { font-size: 26px; color: " FL_COLOR_TEXT_DIM "; }" \
    ".fl-asset-name { width: 100%; height: 16px; overflow: hidden; white-space: nowrap; text-align: center; font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; }" \
    ".fl-chips { width: 100%; height: 28px; gap: 2px; flex-shrink: 0; }" \
    ".fl-chip { width: 32px; height: 26px; flex-shrink: 0; border-radius: " FL_RADIUS_SM "; background: transparent; color: " FL_COLOR_TEXT_MUTED "; }" \
    ".fl-empty { width: 100%; padding: 28px 8px; gap: 8px; align-items: center; flex-shrink: 0; }" \
    ".fl-empty-icon { font-size: 30px; color: " FL_COLOR_TEXT_DIM "; }" \
    ".fl-font-list { width: 100%; height: 180px; flex-shrink: 0; overflow-y: auto; gap: 0px; background: " FL_COLOR_BG_SURFACE "; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-font-row { width: 100%; height: 24px; flex-shrink: 0; padding: 4px 8px; gap: 8px; justify-content: space-between; background: transparent; }" \
    ".fl-warning { color: " FL_COLOR_WARNING "; font-size: " FL_FONT_SM "; text-wrap: wrap; }" \
    ".fl-property-select { flex-grow: 1; min-width: 0px; height: 28px; }"

/* In-app file browser window. */
#define FL_STYLE_FILE_BROWSER_W 720
#define FL_STYLE_FILE_BROWSER_H 520
#define FL_STYLE_FILE_BROWSER_LIMIT 1000
#define FL_CLASS_ICON_ACTION "fl-icon-action"
#define FL_CLASS_FILE_BROWSER "fl-file-browser"
#define FL_CLASS_FILE_LIST "fl-file-list"
#define FL_CLASS_FILE_ROW "fl-file-row"
#define FL_CLASS_FILE_ROW_SELECTED "fl-file-row selected"
#define FL_CLASS_FILE_FOOTER "fl-file-footer"
#define FL_CLASS_FILE_SUMMARY "fl-file-summary"
#define FL_STYLE_CSS_FILE_BROWSER \
    ".fl-file-browser { width: 100%; height: 100%; padding: 8px; gap: 6px; background: " FL_COLOR_BG_BASE "; }" \
    ".fl-icon-action { width: 28px; height: 28px; flex-shrink: 0; background: " FL_COLOR_BG_OVERLAY "; border-radius: " FL_RADIUS_SM "; color: " FL_COLOR_TEXT_BRIGHT "; }" \
    ".fl-icon-action:hover { background: " FL_COLOR_SELECTION "; }" \
    ".fl-file-list { width: 100%; flex-grow: 1; min-height: 0px; overflow-y: auto; gap: 0px; background: " FL_COLOR_BG_SURFACE "; border-radius: " FL_RADIUS_SM "; }" \
    ".fl-file-row { width: 100%; height: 26px; flex-shrink: 0; padding: 4px 10px; gap: 8px; justify-content: space-between; background: transparent; }" \
    ".fl-file-row:hover { background: " FL_COLOR_HOVER "; }" \
    ".fl-file-row.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-file-footer { width: 100%; height: 40px; flex-shrink: 0; gap: 8px; align-items: center; }" \
    ".fl-file-summary { flex-grow: 1; min-width: 0px; color: " FL_COLOR_TEXT_MUTED "; font-size: " FL_FONT_SM "; }"

/* Timeline panel. */
#define FL_STYLE_TL_PAD 12.0f
#define FL_STYLE_TL_ROW_H 26.0f
#define FL_STYLE_TL_RULER_H 22.0f
#define FL_STYLE_TL_KEY 14.0f
#define FL_STYLE_TL_EDGE_W 8.0f
#define FL_STYLE_TL_SNAP 6.0
#define FL_STYLE_TL_ZOOM 120.0
#define FL_STYLE_TL_ZOOM_MIN 8.0
#define FL_STYLE_TL_ZOOM_MAX 2400.0
#define FL_STYLE_TL_LABEL_SPACING 64.0
#define FL_STYLE_TL_BEAT_DEFAULT ((uint64_t)FL_TICKS_PER_SECOND * 3)
#define FL_STYLE_TL_BEAT_MIN ((Fl_Tick)FL_TICKS_PER_SECOND / 30)
#define FL_STYLE_TL_FADE ((uint64_t)FL_TICKS_PER_SECOND / 2)
#define FL_STYLE_TIMELINE_HEIGHT 240.0f
#define FL_CLASS_TIMELINE "fl-timeline"
#define FL_CLASS_TL_CLOCK "fl-tl-clock"
#define FL_CLASS_TL_SPACER "fl-tl-spacer"
#define FL_CLASS_TL_BODY "fl-tl-body"
#define FL_CLASS_TL_LABELS "fl-tl-labels"
#define FL_CLASS_TL_LABEL "fl-tl-label"
#define FL_CLASS_TL_LABEL_SELECTED "fl-tl-label selected"
#define FL_CLASS_TL_LABEL_TEXT "fl-tl-label-text"
#define FL_CLASS_TL_RULER_LABEL "fl-tl-ruler-label"
#define FL_CLASS_TL_LANES "fl-tl-lanes"
#define FL_CLASS_TL_CONTENT "fl-tl-content"
#define FL_CLASS_TL_RULER "fl-tl-ruler"
#define FL_CLASS_TL_TICK "fl-tl-tick"
#define FL_CLASS_TL_TICK_TEXT "fl-tl-tick-text"
#define FL_CLASS_TL_ROW "fl-tl-row"
#define FL_CLASS_TL_BEAT "fl-tl-beat"
#define FL_CLASS_TL_BEAT_SELECTED "fl-tl-beat selected"
#define FL_CLASS_TL_BEAT_TEXT "fl-tl-beat-text"
#define FL_CLASS_TL_EDGE "fl-tl-edge"
#define FL_CLASS_TL_SPAN "fl-tl-span"
#define FL_CLASS_TL_KEY "fl-tl-key"
#define FL_CLASS_TL_KEY_SELECTED "fl-tl-key selected"
#define FL_CLASS_TL_KEY_TEXT "fl-tl-key-text"
#define FL_CLASS_TL_KEY_TEXT_SELECTED "fl-tl-key-text selected"
#define FL_CLASS_TL_CLIP "fl-tl-clip"
#define FL_CLASS_TL_CLIP_SELECTED "fl-tl-clip selected"
#define FL_CLASS_TL_PLAYHEAD "fl-tl-playhead"
#define FL_STYLE_CSS_TIMELINE \
    ".fl-timeline { width: 100%; height: 100%; gap: 0px; overflow: hidden; background: " FL_COLOR_BG_BASE "; }" \
    ".fl-tl-clock { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; padding: 0px 8px; flex-shrink: 0; }" \
    ".fl-tl-spacer { flex-grow: 1; height: 1px; }" \
    ".fl-tl-body { width: 100%; flex-grow: 1; min-height: 0px; overflow-y: auto; gap: 0px; }" \
    ".fl-tl-labels { width: 170px; flex-shrink: 0; gap: 0px; background: " FL_COLOR_BG_ELEVATED "; }" \
    ".fl-tl-ruler-label { width: 100%; height: 22px; flex-shrink: 0; }" \
    ".fl-tl-label { width: 100%; height: 26px; flex-shrink: 0; padding: 0px 10px; align-items: center; overflow: hidden; }" \
    ".fl-tl-label.selected { background: " FL_COLOR_SELECTION "; }" \
    ".fl-tl-label-text { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; white-space: nowrap; }" \
    ".fl-tl-lanes { flex-grow: 1; min-width: 0px; overflow-x: auto; gap: 0px; }" \
    ".fl-tl-content { flex-shrink: 0; gap: 0px; }" \
    ".fl-tl-ruler { height: 22px; flex-shrink: 0; background: " FL_COLOR_BG_ELEVATED "; }" \
    ".fl-tl-tick { height: 22px; padding: 4px 0px 0px 3px; border-left: 1px solid " FL_COLOR_SEPARATOR "; }" \
    ".fl-tl-tick-text { font-size: " FL_FONT_XS "; color: " FL_COLOR_TEXT_DIM "; white-space: nowrap; }" \
    ".fl-tl-row { height: 26px; flex-shrink: 0; border-bottom: 1px solid " FL_COLOR_SEPARATOR "; }" \
    ".fl-tl-beat, .fl-tl-clip { padding: 0px 8px; align-items: center; overflow: hidden; border-radius: " FL_RADIUS_SM "; background: " FL_COLOR_BG_SURFACE "; border: 1px solid " FL_COLOR_SEPARATOR "; }" \
    ".fl-tl-clip { background: " FL_COLOR_BG_OVERLAY "; }" \
    ".fl-tl-beat.selected, .fl-tl-clip.selected { border-color: " FL_COLOR_ACCENT "; background: " FL_COLOR_SELECTION "; }" \
    ".fl-tl-beat-text { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MEDIUM "; white-space: nowrap; }" \
    ".fl-tl-edge { background: transparent; }" \
    ".fl-tl-edge:hover { background: " FL_COLOR_ACCENT "; }" \
    ".fl-tl-span { background: " FL_COLOR_SEPARATOR "; }" \
    ".fl-tl-key { align-items: center; justify-content: center; }" \
    ".fl-tl-key-text { font-size: " FL_FONT_SM "; color: " FL_COLOR_TEXT_MUTED "; }" \
    ".fl-tl-key-text.selected { color: " FL_COLOR_ACCENT "; }" \
    ".fl-tl-playhead { background: " FL_COLOR_ACCENT "; z-index: 2; }"
