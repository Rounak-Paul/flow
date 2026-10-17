#include "fl_launcher.h"
#include "style.h"
#include "doc/fl_binary.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Recent { struct Fl_Launcher *launcher; char *path; } Recent;
struct Fl_Launcher {
    Ca_Window *window;
    Ca_Div *root;
    Ca_TextInput *name, *path;
    Fl_EventBus *bus;
    Fl_Project *selected;
    Recent *recent;
    size_t count;
    char recent_path[FL_PATH_MAX];
    char default_path[FL_PATH_MAX];
    char error[256];
};

/* Persist only launcher metadata; document serialization remains owned by the engine. */
static void save_recent(Fl_Launcher *l)
{
    if (!l->recent_path[0]) return;
    Fl_Binary b = {0}; fl_binary_u64(&b, l->count);
    for (size_t i = 0; i < l->count; ++i) fl_binary_string(&b, l->recent[i].path);
    fl_binary_save(&b, l->recent_path, "FLRECENT"); free(b.data);
}

/* Remember a selected path without duplicates, preserving prior entries on allocation failure. */
static void remember(Fl_Launcher *l, const char *path)
{
    for (size_t i = 0; i < l->count; ++i) if (!strcmp(l->recent[i].path, path)) return;
    if (l->count >= SIZE_MAX / sizeof(Recent) - 1) return;
    char *copy = malloc(strlen(path) + 1);
    if (!copy) return;
    strcpy(copy, path);
    Recent *recent = realloc(l->recent, (l->count + 1) * sizeof(*recent));
    if (!recent) { free(copy); return; }
    l->recent = recent; recent[l->count++] = (Recent){l, copy}; save_recent(l);
}

/* Open a project through its engine validator; show failures without closing the manager. */
static void open_directory(Fl_Launcher *l, const char *path, const char *name)
{
    if (l->selected) return;
    l->selected = name ? fl_project_create(path, name, l->bus) : fl_project_open(path, l->bus);
    if (l->selected) remember(l, fl_project_directory(l->selected));
    else {
        snprintf(l->error, sizeof(l->error), "%s", name ?
            "Could not create project. Check the name, directory, permissions, and whether a project already exists there." :
            "Could not open project. It may be in use, missing, corrupt, or from an unsupported version.");
        ca_popup_show(ca_window_instance(l->window), &(Ca_PopupDesc){.title = "Project unavailable", .message = l->error});
    }
}

/* Open or create using current form fields; text is read before any UI change. */
static void open_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_Launcher *l = data; open_directory(l, ca_input_text(l->path), NULL);
}
static void create_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_Launcher *l = data; open_directory(l, ca_input_text(l->path), ca_input_text(l->name));
}
static void recent_clicked(Ca_Button *button, void *data)
{
    (void)button; Recent *r = data; open_directory(r->launcher, r->path, NULL);
}

/* Mark launcher closure; the editor owns the transition and process lifetime. */
static void closed(Ca_Window *window, void *data)
{
    (void)window; Fl_Launcher *l = data; l->window = NULL; l->root = NULL;
}

/* Render the project manager form and existing recent project entries. */
static void build(Ca_Div *root, void *data)
{
    (void)root; Fl_Launcher *l = data;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_LAUNCHER});
    ca_text(&(Ca_TextDesc){.text = "Flow", .style = FL_CLASS_LAUNCHER_TITLE});
    ca_text(&(Ca_TextDesc){.text = "Projects", .style = FL_CLASS_HEADING});
    ca_text(&(Ca_TextDesc){.text = "Create a project or open a project directory.", .style = FL_CLASS_MUTED});
    ca_text(&(Ca_TextDesc){.text = "Project name (for a new project)", .style = FL_CLASS_LABEL});
    l->name = ca_input(&(Ca_InputDesc){.text = "Untitled", .id = "project-name", .style = FL_CLASS_INPUT});
    ca_text(&(Ca_TextDesc){.text = "Project directory", .style = FL_CLASS_LABEL});
    l->path = ca_input(&(Ca_InputDesc){.text = l->default_path, .id = "project-path", .style = FL_CLASS_INPUT});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TOOLBAR});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Create project", .on_click = create_clicked, .click_data = l, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){.text = "Open project", .on_click = open_clicked, .click_data = l, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_div_end();
    ca_text(&(Ca_TextDesc){.text = "Recent projects", .style = FL_CLASS_HEADING});
    for (size_t i = 0; i < l->count; ++i) {
        ca_btn_begin(&(Ca_BtnDesc){.text = l->recent[i].path, .on_click = recent_clicked,
            .click_data = &l->recent[i], .style = FL_CLASS_RECENT}); ca_btn_end();
    }
    if (!l->count) ca_text(&(Ca_TextDesc){.text = "Your projects will appear here.", .style = FL_CLASS_MUTED});
    ca_div_end();
}

Fl_Launcher *fl_launcher_create(Ca_Instance *instance, Fl_EventBus *bus)
{
    if (!instance || !bus) return NULL;
    Fl_Launcher *l = calloc(1, sizeof(*l)); if (!l) return NULL;
    l->bus = bus;
    char config[FL_PATH_MAX];
    if (fl_paths_user_dir(FL_PATH_CONFIG, config, sizeof(config)))
        fl_fs_join(l->recent_path, sizeof(l->recent_path), config, "recent.flbin");
    Fl_Binary b;
    if (l->recent_path[0] && fl_binary_load(&b, l->recent_path, "FLRECENT")) {
        uint64_t count = fl_binary_get_u64(&b);
        if (count <= (b.size - b.cursor) / 8 && count <= SIZE_MAX / sizeof(Recent)) {
            l->recent = count ? calloc((size_t)count, sizeof(Recent)) : NULL;
            if (l->recent) for (size_t i = 0; !b.failed && i < count; ++i) {
                char *path = fl_binary_get_string(&b);
                if (path) l->recent[l->count++] = (Recent){l, path};
            }
        }
        if (b.failed || b.cursor != b.size) {
            for (size_t i = 0; i < l->count; ++i) free(l->recent[i].path);
            free(l->recent); l->recent = NULL; l->count = 0;
        }
        free(b.data);
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home || !fl_fs_join(l->default_path, sizeof(l->default_path), home, "Documents/FlowProjects/Untitled"))
        snprintf(l->default_path, sizeof(l->default_path), "./Untitled");
    l->window = ca_window_create(instance, &(Ca_WindowDesc){.title = "Flow Projects",
        .width = FL_STYLE_LAUNCHER_W, .height = FL_STYLE_LAUNCHER_H, .on_close = closed, .on_close_data = l});
    l->root = l->window ? ca_window_content_div(l->window) : NULL;
    if (!l->root || !ca_div_set_builder(l->root, build, l)) { fl_launcher_destroy(l); return NULL; }
    return l;
}

Fl_Project *fl_launcher_take_project(Fl_Launcher *l)
{
    if (!l) return NULL;
    Fl_Project *p = l->selected; l->selected = NULL; return p;
}
bool fl_launcher_is_open(const Fl_Launcher *l) { return l && l->window; }
void fl_launcher_destroy(Fl_Launcher *l)
{
    if (!l) return;
    if (l->window) ca_window_destroy(l->window);
    fl_project_destroy(l->selected);
    for (size_t i = 0; i < l->count; ++i) free(l->recent[i].path);
    free(l->recent); free(l);
}
