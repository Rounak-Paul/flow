#include "fl_menubar.h"

#include "style.h"

#include "core/fl_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG   "menubar"
#define LABEL_MAX 96

/* Stable action context for one View menu entry. */
typedef struct PanelToggle {
    Fl_MenuBar *menubar;
    int         panel;
} PanelToggle;

struct Fl_MenuBar {
    Ca_Window       *window;
    Fl_EventBus     *bus;
    Fl_Workspace    *workspace;
    Fl_Project *project;
    void (*projects)(void *);
    void *projects_data;
    PanelToggle     *toggles;
    Ca_MenuItemDesc *items;
    char           (*labels)[LABEL_MAX];
    int              count;
    Ca_Effect       *refresher;
    bool             dirty;
};

/*
 * View menu action: toggle the chosen panel.
 *
 * user_data  PanelToggle of the entry.
 */
static void toggle_panel(void *user_data)
{
    const PanelToggle *toggle = user_data;
    const Fl_MenuBar *menubar = toggle->menubar;
    fl_workspace_toggle_panel(menubar->workspace, fl_workspace_panel_id(menubar->workspace, toggle->panel));
}

/* Save the current project through the shared persistence boundary. */
static void save_project(void *data)
{
    Fl_MenuBar *bar = data;
    if (!fl_project_save(bar->project)) FL_LOG_ERROR("project", "save failed");
}

/* Request a deferred transition back to the separate project manager. */
static void show_projects(void *data)
{
    Fl_MenuBar *bar = data; bar->projects(bar->projects_data);
}

/* Rebuild the menus from the workspace's current panel visibility. */
static void install_menus(Fl_MenuBar *menubar)
{
    const int check_cells = ca_codepoint_cell_width(0xF00Cu);
    for (int i = 0; i < menubar->count; ++i) {
        const char *id = fl_workspace_panel_id(menubar->workspace, i);
        const char *title = fl_workspace_panel_title(menubar->workspace, i);
        if (fl_workspace_panel_visible(menubar->workspace, id))
            snprintf(menubar->labels[i], LABEL_MAX, "%s  %s", FL_STYLE_MENU_CHECK, title);
        else
            snprintf(menubar->labels[i], LABEL_MAX, "%*s  %s", check_cells > 0 ? check_cells : 1, "", title);
        menubar->items[i] = (Ca_MenuItemDesc){
            .label = menubar->labels[i], .action = toggle_panel, .action_data = &menubar->toggles[i],
        };
    }
    const Ca_MenuItemDesc file[] = {
        {.label = "Save project", .action = save_project, .action_data = menubar},
        {.label = "Projects...", .action = show_projects, .action_data = menubar},
    };
    const Ca_MenuDesc menus[] = {
        {.label = "File", .items = file, .item_count = 2},
        { .label = "View", .items = menubar->items, .item_count = menubar->count },
    };
    ca_window_set_title_bar_menus(menubar->window, menus, (int)(sizeof(menus) / sizeof(menus[0])));
}

/*
 * Dock observer: a panel opened or closed somewhere; refresh the check marks
 * on the next tick, since the change may come from inside a menu action
 * whose menu must not be replaced while it is still dispatching.
 *
 * event      FL_EVENT_DOCK_PANEL_OPENED or _CLOSED.
 * user_data  Owning menu bar.
 * Returns    false; other observers also receive the change.
 */
static bool on_panel_visibility(const Fl_Event *event, void *user_data)
{
    (void)event;
    Fl_MenuBar *menubar = user_data;
    menubar->dirty = true;
    ca_instance_request_frame_after(ca_window_instance(menubar->window), 0.0);
    return false;
}

/*
 * Per-tick refresh of a menu marked dirty by a visibility change.
 *
 * user_data  Owning menu bar.
 */
static void refresh_menus(void *user_data)
{
    Fl_MenuBar *menubar = user_data;
    if (!menubar->dirty || !ca_window_is_open(menubar->window)) return;
    menubar->dirty = false;
    install_menus(menubar);
}

static bool observe(Fl_MenuBar *menubar, const char *name)
{
    const Fl_EventType type = fl_event_type_find(menubar->bus, name);
    return type != FL_EVENT_TYPE_INVALID &&
           fl_event_subscribe(menubar->bus, &(Fl_SubscribeDesc){
               .type = type, .fn = on_panel_visibility, .user_data = menubar, .owner = menubar,
           }).generation != 0;
}

Fl_MenuBar *fl_menubar_create(Ca_Window *window, Fl_EventBus *bus, Fl_Workspace *workspace,
                                 Fl_Project *project, void (*projects)(void *), void *data)
{
    if (!window || !bus || !workspace || !project || !projects) return NULL;
    Fl_MenuBar *menubar = calloc(1, sizeof(*menubar));
    if (!menubar) return NULL;
    menubar->window = window;
    menubar->bus = bus;
    menubar->workspace = workspace;
    menubar->project = project; menubar->projects = projects; menubar->projects_data = data;
    menubar->count = fl_workspace_panel_count(workspace);
    if (menubar->count > 0) {
        const size_t n = (size_t)menubar->count;
        menubar->toggles = calloc(n, sizeof(*menubar->toggles));
        menubar->items = calloc(n, sizeof(*menubar->items));
        menubar->labels = calloc(n, sizeof(*menubar->labels));
    }
    const bool allocated = menubar->count == 0 || (menubar->toggles && menubar->items && menubar->labels);
    if (allocated)
        menubar->refresher = ca_frame_effect(ca_window_instance(window), refresh_menus, menubar);
    if (!menubar->refresher || !observe(menubar, FL_EVENT_DOCK_PANEL_OPENED) ||
        !observe(menubar, FL_EVENT_DOCK_PANEL_CLOSED)) {
        FL_LOG_ERROR(LOG_TAG, "failed to create the menu bar");
        fl_menubar_destroy(menubar);
        return NULL;
    }
    for (int i = 0; i < menubar->count; ++i)
        menubar->toggles[i] = (PanelToggle){ .menubar = menubar, .panel = i };
    install_menus(menubar);
    return menubar;
}

void fl_menubar_destroy(Fl_MenuBar *menubar)
{
    if (!menubar) return;
    ca_effect_destroy(menubar->refresher);
    fl_event_unsubscribe_owner(menubar->bus, menubar);
    if (ca_window_is_open(menubar->window)) ca_window_set_title_bar_menus(menubar->window, NULL, 0);
    free(menubar->toggles);
    free(menubar->items);
    free(menubar->labels);
    free(menubar);
}
