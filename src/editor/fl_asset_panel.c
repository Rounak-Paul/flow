#include "fl_asset_panel.h"
#include "fl_file_browser.h"
#include "style.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_ID "assets"

typedef struct Fl_AssetRow { struct Fl_AssetPanel *panel; Fl_Id asset; } Fl_AssetRow;
typedef struct Fl_Placement { uint64_t request; Fl_SceneDropTarget target; } Fl_Placement;
struct Fl_AssetPanel {
    Fl_SceneEdit *edit;
    Fl_ScenePanel *scene;
    Fl_AssetImporter *importer;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Ca_Label *status;
    Fl_AssetRow *rows;
    size_t row_count;
    Fl_Placement *placements;
    size_t placement_count, placement_capacity;
    char *query;
    char folder[FL_PATH_MAX];
    Fl_FileBrowser *browser;
    Fl_Id selected;
    Ca_Window *rename_window;
    Ca_TextInput *rename_input;
    Fl_Id rename_asset;
    char message[256];
};

/* Rebuild the panel on the next frame. */
static void invalidate(Fl_AssetPanel *p)
{
    if (p->dock) ca_dock_invalidate_panel(p->dock, PANEL_ID);
}

/* Show a status message without rebuilding focused inputs. */
static void status(Fl_AssetPanel *p, const char *message)
{
    snprintf(p->message, sizeof(p->message), "%s", message);
    if (p->status) ca_set_text(p->status, p->message);
}

/* Replace an owned draft string, keeping the previous one on allocation failure. */
static void set_text(char **field, const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (!copy) return;
    memcpy(copy, text, size); free(*field); *field = copy;
}

/* Match names case-insensitively without allocating. */
static bool matches(const char *name, const char *query)
{
    if (!query || !query[0]) return true;
    for (; *name; ++name) {
        const char *a = name, *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

/* Format a byte count with binary units. */
static void format_size(uint64_t size, char *out, size_t capacity)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)size; size_t unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    snprintf(out, capacity, unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
}

/* Queue one import, optionally remembering where its image should be placed. */
static void import_path(Fl_AssetPanel *p, const char *path, const Fl_SceneDropTarget *target)
{
    uint64_t request = fl_asset_import(p->importer, path);
    if (!request) { FL_LOG_ERROR("assets", "could not queue import of %s", path); return; }
    if (!target) return;
    if (p->placement_count == p->placement_capacity) {
        size_t capacity = p->placement_capacity ? p->placement_capacity * 2 : 8;
        Fl_Placement *grown = realloc(p->placements, capacity * sizeof(*grown));
        if (!grown) return;
        p->placements = grown; p->placement_capacity = capacity;
    }
    p->placements[p->placement_count++] = (Fl_Placement){request, *target};
}

typedef struct Fl_FolderImport { Fl_AssetPanel *panel; const char *folder; const Fl_SceneDropTarget *target; } Fl_FolderImport;

/* Import one visible regular file of a dropped folder. */
static bool import_entry(const char *name, bool directory, void *data)
{
    Fl_FolderImport *folder = data;
    char path[FL_PATH_MAX];
    if (!directory && name[0] != '.' && fl_fs_join(path, sizeof(path), folder->folder, name))
        import_path(folder->panel, path, folder->target);
    return true;
}

/* Import a file, or a folder's immediate files, and refresh the status line. */
static void import_source(Fl_AssetPanel *p, const char *path, const Fl_SceneDropTarget *target)
{
    if (fl_fs_file_size(path) < 0 && fl_fs_exists(path)) {
        Fl_FolderImport folder = {p, path, target};
        if (!fl_fs_list_dir(path, import_entry, &folder)) FL_LOG_ERROR("assets", "could not read folder %s", path);
    } else import_path(p, path, target);
}

/* Announce outstanding work after queueing imports. */
static void report_pending(Fl_AssetPanel *p)
{
    size_t pending = fl_asset_importer_pending(p->importer);
    char text[96];
    if (pending) snprintf(text, sizeof(text), "Importing %zu file%s...", pending, pending == 1 ? "" : "s");
    else snprintf(text, sizeof(text), "No supported files to import.");
    status(p, text);
}

void fl_asset_panel_drop_files(Fl_AssetPanel *p, const char *const *paths, int count, double screen_x, double screen_y)
{
    if (!p || !paths || count <= 0) return;
    Fl_SceneDropTarget target;
    bool placed = fl_scene_panel_drop_target(p->scene, screen_x, screen_y, &target);
    for (int i = 0; i < count; ++i) if (paths[i] && paths[i][0]) import_source(p, paths[i], placed ? &target : NULL);
    report_pending(p);
}

/* Import the files chosen in the browser and remember their folder for the next import. */
static void files_chosen(const char *const *paths, size_t count, void *data)
{
    Fl_AssetPanel *p = data;
    snprintf(p->folder, sizeof(p->folder), "%s", paths[0]);
    char *cut = p->folder + strlen(p->folder);
    while (cut > p->folder && cut[-1] != '/' && cut[-1] != '\\') --cut;
    if (cut > p->folder + 1) cut[-1] = 0; else *cut = 0;
    for (size_t i = 0; i < count; ++i) import_source(p, paths[i], NULL);
    report_pending(p);
}

/* Open the in-app file browser filtered to importable media. */
static void browse_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_AssetPanel *p = data;
    static const char *images[] = {"png", "jpg", "jpeg", "gif", "bmp"}, *fonts[] = {"ttf", "otf", "ttc"},
        *video[] = {"mp4", "m4v", "mov", "mkv", "webm", "avi"}, *audio[] = {"wav", "mp3", "ogg", "flac", "m4a"},
        *media[] = {"png", "jpg", "jpeg", "gif", "bmp", "ttf", "otf", "ttc", "mp4", "m4v", "mov", "mkv", "webm", "avi",
                    "wav", "mp3", "ogg", "flac", "m4a"};
    const Fl_FileFilter filters[] = {
        {"Supported media", media, sizeof(media) / sizeof(media[0])}, {"Images", images, sizeof(images) / sizeof(images[0])},
        {"Fonts", fonts, sizeof(fonts) / sizeof(fonts[0])}, {"Video", video, sizeof(video) / sizeof(video[0])},
        {"Audio", audio, sizeof(audio) / sizeof(audio[0])}, {"All files", NULL, 0}};
    if (!p->browser && p->window) p->browser = fl_file_browser_create(ca_window_instance(p->window));
    if (!fl_file_browser_show(p->browser, &(Fl_FileBrowserDesc){.mode = FL_FILE_BROWSER_OPEN_FILES, .title = "Import assets",
            .confirm_label = "Import", .initial_path = p->folder[0] ? p->folder : NULL, .filters = filters,
            .filter_count = sizeof(filters) / sizeof(filters[0]), .multiple = true, .on_confirm = files_chosen, .user_data = p}))
        status(p, "The file browser could not be opened.");
}

/* Persist the search draft across rebuilds. */
static void query_changed(Ca_TextInput *input, void *data)
{
    Fl_AssetPanel *p = data; set_text(&p->query, ca_input_text(input)); invalidate(p);
}

/* Place an image at the active scene's centre. */
static void add_to_scene(Fl_AssetPanel *p, Fl_Id asset)
{
    const Fl_Scene *scene = fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
    if (!scene) { status(p, "Create or select a scene first."); return; }
    if (!fl_scene_edit_insert_image(p->edit, asset, 0, scene->width * .5, scene->height * .5, true))
        status(p, "The image could not be added to the scene.");
}

/* Release borrowed rename handles when the utility window closes. */
static void rename_closed(Ca_Window *window, void *data)
{
    (void)window; Fl_AssetPanel *p = data; p->rename_window = NULL; p->rename_input = NULL;
}

/* Commit a rename to the asset captured when the window opened. */
static void rename_apply(Ca_Button *button, void *data)
{
    (void)button; Fl_AssetPanel *p = data;
    if (fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_ASSET_RENAME, .asset = p->rename_asset,
                         .name = ca_input_text(p->rename_input)}, NULL)) ca_window_close(p->rename_window);
    else status(p, "Names cannot be empty.");
}

/* Open the rename form for one asset. */
static void rename_asset(Fl_AssetPanel *p, Fl_Id asset)
{
    const Fl_Asset *record = fl_project_asset(fl_scene_edit_project(p->edit), asset);
    if (!record || !p->window) return;
    if (p->rename_window) ca_window_destroy(p->rename_window);
    p->rename_asset = asset;
    p->rename_window = ca_window_create(ca_window_instance(p->window), &(Ca_WindowDesc){
        .title = "Rename asset", .width = FL_STYLE_FORM_W, .height = FL_STYLE_RENAME_H,
        .on_close = rename_closed, .on_close_data = p});
    if (!p->rename_window) return;
    ca_ui_begin(p->rename_window, &(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_FORM});
    p->rename_input = ca_input(&(Ca_InputDesc){.text = record->name, .style = FL_CLASS_INPUT});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Rename", .on_click = rename_apply, .click_data = p, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_ui_end(); ca_input_focus(p->rename_input);
}

/* Remove an unused asset; referenced assets are kept and the user is told why. */
static void delete_asset(Fl_AssetPanel *p, Fl_Id asset)
{
    Fl_Project *project = fl_scene_edit_project(p->edit);
    const Fl_Asset *record = fl_project_asset(project, asset);
    if (!record) return;
    size_t uses = fl_project_asset_uses(project, asset);
    char text[256];
    if (uses) {
        snprintf(text, sizeof(text), "\"%s\" is used by %zu entit%s. Remove those first.", record->name, uses, uses == 1 ? "y" : "ies");
        status(p, text); return;
    }
    snprintf(text, sizeof(text), "Removed \"%s\".", record->name);
    if (fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = asset}, NULL)) {
        if (p->selected == asset) p->selected = 0;
        status(p, text);
    } else status(p, "The asset could not be removed.");
}

/* Context actions operate on the clicked row's asset. */
static void row_menu(int index, void *data)
{
    Fl_AssetRow row = *(Fl_AssetRow *)data;
    const Fl_Asset *record = fl_project_asset(fl_scene_edit_project(row.panel->edit), row.asset);
    if (!record) return;
    int action = record->kind == FL_ASSET_IMAGE ? index : index + 1;
    if (action == 0) add_to_scene(row.panel, row.asset);
    else if (action == 1) rename_asset(row.panel, row.asset);
    else if (action == 2) delete_asset(row.panel, row.asset);
}

/* Select a row; keyboard activation of an image row also places it. */
static void row_clicked(Ca_Button *button, void *data)
{
    Fl_AssetRow *row = data;
    float x, y;
    row->panel->selected = row->asset;
    invalidate(row->panel);
    const Fl_Asset *record = fl_project_asset(fl_scene_edit_project(row->panel->edit), row->asset);
    if (!ca_button_get_click_pos(button, &x, &y) && record && record->kind == FL_ASSET_IMAGE) add_to_scene(row->panel, row->asset);
}

/* Finish dragging an image row onto the scene hierarchy or artboard. */
static void row_drag_end(const Ca_DragEvent *event, void *data)
{
    Fl_AssetRow *row = data;
    Fl_AssetPanel *p = row->panel;
    if (hypot(event->dx, event->dy) < FL_STYLE_WIDGET_DRAG_THRESHOLD) return;
    const Fl_Asset *record = fl_project_asset(fl_scene_edit_project(p->edit), row->asset);
    float x, y;
    Fl_SceneDropTarget target;
    if (!record || record->kind != FL_ASSET_IMAGE) return;
    if ((ca_window_mouse_release_position(event->window, 0, &x, &y) || ca_window_cursor_screen_position(event->window, &x, &y)) &&
        fl_scene_panel_drop_target(p->scene, x, y, &target))
        fl_scene_edit_insert_image(p->edit, row->asset, target.parent, target.x, target.y, target.at_point);
}

/* Place images dropped over the scene once their import finishes; refresh status for every result. */
static bool imported(const Fl_Event *event, void *data)
{
    Fl_AssetPanel *p = data;
    const Fl_AssetImported *result = event->payload;
    if (result->importer != p->importer) return false;
    for (size_t i = 0; i < p->placement_count; ++i) {
        if (p->placements[i].request != result->request) continue;
        Fl_SceneDropTarget target = p->placements[i].target;
        p->placements[i] = p->placements[--p->placement_count];
        const Fl_Asset *record = fl_project_asset(fl_scene_edit_project(p->edit), result->asset);
        if (record && record->kind == FL_ASSET_IMAGE && target.scene == fl_scene_edit_scene(p->edit) &&
            !fl_scene_edit_insert_image(p->edit, result->asset, target.parent, target.x, target.y, target.at_point))
            FL_LOG_WARN("assets", "imported %s but could not place it in the scene", result->name);
        break;
    }
    if (result->asset) p->selected = result->asset;
    char text[256];
    size_t pending = fl_asset_importer_pending(p->importer);
    const char *outcome = result->status == FL_IMPORT_ADDED ? "Imported" : result->status == FL_IMPORT_EXISTING ? "Already imported" :
                          result->status == FL_IMPORT_UNSUPPORTED ? "Unsupported file" : "Could not import";
    if (pending) snprintf(text, sizeof(text), "%s %s. %zu remaining...", outcome, result->name, pending);
    else snprintf(text, sizeof(text), "%s %s.", outcome, result->name);
    status(p, text);
    invalidate(p);
    return false;
}

/* Rebuild after asset records change. */
static bool project_changed(const Fl_Event *event, void *data)
{
    Fl_AssetPanel *p = data;
    const Fl_ProjectChange *change = event->payload;
    if (change->project == fl_scene_edit_project(p->edit) &&
        (change->kind == FL_OP_ASSET_ADD || change->kind == FL_OP_ASSET_REMOVE || change->kind == FL_OP_ASSET_RENAME)) invalidate(p);
    return false;
}

Fl_AssetPanel *fl_asset_panel_create(Fl_SceneEdit *edit, Fl_ScenePanel *scene, Fl_AssetImporter *importer, Fl_EventBus *bus)
{
    if (!edit || !scene || !importer || !bus) return NULL;
    Fl_AssetPanel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = edit; p->scene = scene; p->importer = importer; p->bus = bus;
    if (!fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_ASSET_IMPORTED),
            .fn = imported, .user_data = p, .owner = p}).generation ||
        !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_PROJECT_CHANGED),
            .fn = project_changed, .user_data = p, .owner = p}).generation) {
        fl_asset_panel_destroy(p); return NULL;
    }
    return p;
}

void fl_asset_panel_teardown(void *data)
{
    Fl_AssetPanel *p = data; p->dock = NULL; p->window = NULL; p->status = NULL;
}

void fl_asset_panel_destroy(Fl_AssetPanel *p)
{
    if (!p) return;
    if (p->rename_window) ca_window_destroy(p->rename_window);
    fl_file_browser_destroy(p->browser);
    fl_event_unsubscribe_owner(p->bus, p);
    free(p->rows); free(p->placements); free(p->query); free(p);
}

/* Emit one asset row with drag-to-scene for images and per-asset context actions. */
static void emit_row(Fl_AssetPanel *p, const Fl_Asset *a)
{
    Fl_AssetRow *row = &p->rows[p->row_count++];
    *row = (Fl_AssetRow){p, a->id};
    char id[48], meta[128], size[32], label[16];
    snprintf(id, sizeof(id), "asset-%llu", (unsigned long long)a->id);
    format_size(a->size, size, sizeof(size));
    size_t n = 0;
    for (; a->extension[n] && n < sizeof(label) - 1; ++n) label[n] = (char)toupper((unsigned char)a->extension[n]);
    label[n] = 0;
    if (a->width && a->height) snprintf(meta, sizeof(meta), "%s  %llux%llu  %s", label, (unsigned long long)a->width, (unsigned long long)a->height, size);
    else snprintf(meta, sizeof(meta), "%s  %s", label, size);
    bool image = a->kind == FL_ASSET_IMAGE;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_ASSET_WRAP, .id = id,
        .on_drag_end = image ? row_drag_end : NULL, .drag_data = row});
    ca_btn_begin(&(Ca_BtnDesc){.direction = CA_VERTICAL, .on_click = row_clicked, .click_data = row,
        .style = p->selected == a->id ? FL_CLASS_ASSET_ROW_SELECTED : FL_CLASS_ASSET_ROW});
    ca_text(&(Ca_TextDesc){.text = a->name, .style = FL_CLASS_LABEL});
    ca_text(&(Ca_TextDesc){.text = meta, .style = FL_CLASS_MUTED});
    ca_btn_end();
    ca_div_end();
    static const char *image_items[] = {"Add to scene", "Rename", "Delete"};
    ca_context_menu(&(Ca_CtxMenuDesc){.items = image ? image_items : image_items + 1, .item_count = image ? 3 : 2,
        .on_select = row_menu, .select_data = row});
}

void fl_asset_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_AssetPanel *p = data; p->dock = dock; p->window = window;
    Fl_Project *project = fl_scene_edit_project(p->edit);
    size_t count = fl_project_asset_count(project);
    free(p->rows); p->rows = count ? calloc(count, sizeof(*p->rows)) : NULL; p->row_count = 0;
    if (count && !p->rows) return;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_ASSETS});
    ca_input(&(Ca_InputDesc){.text = p->query, .placeholder = "Search assets", .id = "asset-search", .style = FL_CLASS_INPUT,
        .on_change = query_changed, .change_data = p});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TOOLBAR});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Import files...", .id = "asset-import", .on_click = browse_clicked, .click_data = p, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_div_end();
    p->status = ca_text(&(Ca_TextDesc){.text = p->message, .style = FL_CLASS_MUTED, .wrap = true});
    if (!count) ca_text(&(Ca_TextDesc){.text = "Drop images, videos, audio or fonts anywhere in this window, or use Import files. "
        "Images dropped on the viewport are placed where you drop them.", .style = FL_CLASS_DROP_HINT, .wrap = true});
    static const struct { Fl_AssetKind kind; const char *title, *note; } groups[] = {
        {FL_ASSET_IMAGE, "Images", "Drag onto the viewport or hierarchy to place."},
        {FL_ASSET_FONT, "Fonts", "Available in the Inspector's font list for text."},
        {FL_ASSET_VIDEO, "Video", "Stored in the project; placement arrives with the media decoder."},
        {FL_ASSET_AUDIO, "Audio", "Stored in the project; playback arrives with the timeline."}};
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); ++g) {
        size_t shown = 0;
        for (size_t i = 0; i < count; ++i) {
            const Fl_Asset *a = fl_project_asset_at(project, i);
            if (a->kind != groups[g].kind || !matches(a->name, p->query)) continue;
            if (!shown++) {
                ca_text(&(Ca_TextDesc){.text = groups[g].title, .style = FL_CLASS_HEADING});
                ca_text(&(Ca_TextDesc){.text = groups[g].note, .style = FL_CLASS_MUTED, .wrap = true});
            }
            emit_row(p, a);
        }
    }
    ca_div_end();
}
