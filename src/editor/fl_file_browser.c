#include "fl_file_browser.h"
#include "style.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <ca_icons.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY 32
#define LIST_ID "file-browser-list"

typedef struct Fl_FileEntry { char *name; bool directory, selected; int64_t size; } Fl_FileEntry;
typedef struct Fl_FileRow { struct Fl_FileBrowser *browser; size_t index; } Fl_FileRow;
typedef struct Fl_StoredFilter {
    char label[64];
    char extensions[FL_FILE_BROWSER_MAX_EXTENSIONS][16];
    size_t count;
} Fl_StoredFilter;
struct Fl_FileBrowser {
    Ca_Instance *instance;
    Ca_Window *window;
    Ca_Div *root;
    Fl_FileBrowserMode mode;
    bool multiple, show_hidden;
    char title[96], confirm[32];
    Fl_FileBrowserFn on_confirm;
    void *user_data;
    Fl_StoredFilter filters[FL_FILE_BROWSER_MAX_FILTERS];
    const char *filter_labels[FL_FILE_BROWSER_MAX_FILTERS];
    size_t filter_count, active_filter;
    char current[FL_PATH_MAX], draft[FL_PATH_MAX];
    char history[HISTORY][FL_PATH_MAX];
    size_t history_count, history_pos;
    Fl_FileEntry *entries;
    size_t entry_count, entry_capacity, anchor;
    Fl_FileRow *rows;
    size_t row_capacity;
    char message[256];
};

/* Return whether c separates path components on this platform. */
static bool separator(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

/* Length of the root prefix ("/" or "C:\"), zero for relative paths. */
static size_t root_length(const char *path)
{
#ifdef _WIN32
    if (path[0] && path[1] == ':' && separator(path[2])) return 3;
#endif
    return separator(path[0]) ? 1 : 0;
}

/* Write the user's home folder, falling back to the filesystem root. */
static void home_folder(char *out, size_t capacity)
{
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
#ifdef _WIN32
    const char *fallback = "C:\\";
#else
    const char *fallback = "/";
#endif
    if (!home || !home[0] || !fl_fs_absolute(home, out, capacity)) snprintf(out, capacity, "%s", fallback);
}

/* Lowercase extension after the last dot, or "" when absent. */
static void extension_of(const char *name, char out[16])
{
    const char *dot = strrchr(name, '.');
    size_t n = 0;
    if (dot && dot != name) for (++dot; *dot && n < 15; ++dot) out[n++] = (char)(*dot >= 'A' && *dot <= 'Z' ? *dot + 32 : *dot);
    out[n] = 0;
}

/* Apply the active extension filter to a file name. */
static bool accepted(const Fl_FileBrowser *b, const char *name)
{
    if (!b->filter_count) return true;
    const Fl_StoredFilter *f = &b->filters[b->active_filter];
    if (!f->count) return true;
    char ext[16]; extension_of(name, ext);
    for (size_t i = 0; i < f->count; ++i) if (!strcmp(ext, f->extensions[i])) return true;
    return false;
}

/* Compare ASCII case-insensitively with a bytewise tiebreak for stable ordering. */
static int compare_names(const char *a, const char *b)
{
    for (const char *x = a, *y = b;; ++x, ++y) {
        int p = (unsigned char)*x, q = (unsigned char)*y;
        if (p >= 'A' && p <= 'Z') p += 32;
        if (q >= 'A' && q <= 'Z') q += 32;
        if (p != q) return p - q;
        if (!p) return strcmp(a, b);
    }
}

/* Folders first, then names. */
static int compare_entries(const void *x, const void *y)
{
    const Fl_FileEntry *a = x, *b = y;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    return compare_names(a->name, b->name);
}

/* Release every listed entry. */
static void clear_entries(Fl_FileBrowser *b)
{
    for (size_t i = 0; i < b->entry_count; ++i) free(b->entries[i].name);
    b->entry_count = 0;
}

typedef struct Fl_Listing { Fl_FileBrowser *browser; const char *folder; bool failed; } Fl_Listing;

/* Collect one directory entry that passes the hidden-file and mode/extension filters. */
static bool collect(const char *name, bool directory, void *data)
{
    Fl_Listing *l = data;
    Fl_FileBrowser *b = l->browser;
    if ((!b->show_hidden && name[0] == '.') || (!directory && (b->mode == FL_FILE_BROWSER_SELECT_FOLDER || !accepted(b, name)))) return true;
    if (b->entry_count == b->entry_capacity) {
        size_t capacity = b->entry_capacity ? b->entry_capacity * 2 : 64;
        Fl_FileEntry *grown = capacity < SIZE_MAX / sizeof(*grown) ? realloc(b->entries, capacity * sizeof(*grown)) : NULL;
        if (!grown) { l->failed = true; return false; }
        b->entries = grown; b->entry_capacity = capacity;
    }
    size_t size = strlen(name) + 1;
    char *copy = malloc(size), path[FL_PATH_MAX];
    if (!copy) { l->failed = true; return false; }
    memcpy(copy, name, size);
    int64_t bytes = !directory && fl_fs_join(path, sizeof(path), l->folder, name) ? fl_fs_file_size(path) : -1;
    b->entries[b->entry_count++] = (Fl_FileEntry){copy, directory, false, bytes};
    return true;
}

/* Re-list the current folder; on failure the listing is empty and the reason is shown. */
static void scan(Fl_FileBrowser *b)
{
    clear_entries(b);
    b->anchor = SIZE_MAX; b->message[0] = 0;
    Fl_Listing listing = {b, b->current, false};
    if (!fl_fs_list_dir(b->current, collect, &listing)) snprintf(b->message, sizeof(b->message), "This folder cannot be read.");
    else if (listing.failed) snprintf(b->message, sizeof(b->message), "Out of memory; the listing is incomplete.");
    if (b->entry_count) qsort(b->entries, b->entry_count, sizeof(*b->entries), compare_entries);
    snprintf(b->draft, sizeof(b->draft), "%s", b->current);
    if (b->root) ca_div_invalidate(b->root);
    if (b->window) ca_scroll_to_top(b->window, LIST_ID);
}

/* Resolve path (expanding "~") to an absolute readable folder; false leaves the browser unchanged. */
static bool enter(Fl_FileBrowser *b, const char *path, bool record)
{
    char expanded[FL_PATH_MAX], absolute[FL_PATH_MAX];
    while (*path == ' ') ++path;
    if (path[0] == '~' && (!path[1] || separator(path[1]))) {
        char home[FL_PATH_MAX]; home_folder(home, sizeof(home));
        if (snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1) >= (int)sizeof(expanded)) return false;
    } else snprintf(expanded, sizeof(expanded), "%s", path);
    if (!expanded[0] || !fl_fs_absolute(expanded, absolute, sizeof(absolute)) ||
        fl_fs_file_size(absolute) >= 0 || !fl_fs_exists(absolute)) return false;
    snprintf(b->current, sizeof(b->current), "%s", absolute);
    if (record) {
        size_t next = b->history_count ? b->history_pos + 1 : 0;
        if (next == HISTORY) { memmove(b->history, b->history + 1, sizeof(b->history[0]) * (HISTORY - 1)); next = HISTORY - 1; }
        snprintf(b->history[next], FL_PATH_MAX, "%s", absolute);
        b->history_pos = next; b->history_count = next + 1;
    }
    scan(b);
    return true;
}

/* Navigate to the parent folder, stopping at the filesystem root. */
static void go_up(Fl_FileBrowser *b)
{
    char parent[FL_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", b->current);
    size_t root = root_length(parent), length = strlen(parent);
    while (length > root && separator(parent[length - 1])) parent[--length] = 0;
    while (length > root && !separator(parent[length - 1])) parent[--length] = 0;
    while (length > root && separator(parent[length - 1])) parent[--length] = 0;
    if (length && strcmp(parent, b->current)) enter(b, parent, true);
}

/* Revisit a history slot without recording a new one. */
static void revisit(Fl_FileBrowser *b, size_t slot)
{
    size_t previous = b->history_pos;
    b->history_pos = slot;
    if (!enter(b, b->history[slot], false)) {
        b->history_pos = previous;
        snprintf(b->message, sizeof(b->message), "That folder is no longer available.");
        if (b->root) ca_div_invalidate(b->root);
    }
}

static void back_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_FileBrowser *b = data;
    if (b->history_pos) revisit(b, b->history_pos - 1);
}
static void forward_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_FileBrowser *b = data;
    if (b->history_pos + 1 < b->history_count) revisit(b, b->history_pos + 1);
}
static void up_clicked(Ca_Button *button, void *data) { (void)button; go_up(data); }
static void home_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_FileBrowser *b = data;
    char home[FL_PATH_MAX]; home_folder(home, sizeof(home));
    enter(b, home, true);
}
static void refresh_clicked(Ca_Button *button, void *data) { (void)button; scan(data); }
static void draft_changed(Ca_TextInput *input, void *data)
{
    Fl_FileBrowser *b = data; snprintf(b->draft, sizeof(b->draft), "%s", ca_input_text(input));
}
static void go_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_FileBrowser *b = data;
    char typed[FL_PATH_MAX]; snprintf(typed, sizeof(typed), "%s", b->draft);
    if (!enter(b, typed, true)) {
        snprintf(b->message, sizeof(b->message), "No readable folder exists at that path.");
        if (b->root) ca_div_invalidate(b->root);
    }
}
static void hidden_changed(Ca_Checkbox *checkbox, void *data)
{
    Fl_FileBrowser *b = data; b->show_hidden = ca_checkbox_get(checkbox); scan(b);
}
static void filter_changed(Ca_Select *select, void *data)
{
    Fl_FileBrowser *b = data; int index = ca_select_get(select);
    if (index >= 0 && (size_t)index < b->filter_count) { b->active_filter = (size_t)index; scan(b); }
}

/* Count selected entries, optionally only files. */
static size_t selected_count(const Fl_FileBrowser *b, bool files_only)
{
    size_t n = 0;
    for (size_t i = 0; i < b->entry_count; ++i) n += b->entries[i].selected && (!files_only || !b->entries[i].directory);
    return n;
}

/* Close the window and deliver the chosen paths; the browser is not touched after the callback. */
static void confirm(Fl_FileBrowser *b)
{
    size_t count = b->mode == FL_FILE_BROWSER_SELECT_FOLDER ? 1 : selected_count(b, true);
    if (!count || !b->on_confirm) return;
    char (*paths)[FL_PATH_MAX] = malloc(count * sizeof(*paths));
    const char **list = malloc(count * sizeof(*list));
    if (!paths || !list) { free(paths); free(list); snprintf(b->message, sizeof(b->message), "Out of memory."); return; }
    size_t n = 0;
    if (b->mode == FL_FILE_BROWSER_SELECT_FOLDER) {
        const char *folder = NULL;
        for (size_t i = 0; i < b->entry_count && !folder; ++i) if (b->entries[i].selected) folder = b->entries[i].name;
        if (!folder) snprintf(paths[n], FL_PATH_MAX, "%s", b->current);
        if (folder ? fl_fs_join(paths[n], FL_PATH_MAX, b->current, folder) : true) { list[n] = paths[n]; ++n; }
    } else {
        for (size_t i = 0; i < b->entry_count; ++i)
            if (b->entries[i].selected && !b->entries[i].directory && fl_fs_join(paths[n], FL_PATH_MAX, b->current, b->entries[i].name)) {
                list[n] = paths[n]; ++n;
            }
    }
    Fl_FileBrowserFn callback = b->on_confirm; void *user_data = b->user_data;
    fl_file_browser_close(b);
    if (n) callback(list, n, user_data);
    free(paths); free(list);
}

static void cancel_clicked(Ca_Button *button, void *data) { (void)button; fl_file_browser_close(data); }
static void confirm_clicked(Ca_Button *button, void *data) { (void)button; confirm(data); }

/* Select on click; clicking the sole selection again opens folders or confirms files.
   With multiple selection, Cmd/Ctrl toggles a file and Shift extends a range of files. */
static void entry_clicked(Ca_Button *button, void *data)
{
    (void)button; Fl_FileRow *row = data; Fl_FileBrowser *b = row->browser;
    if (row->index >= b->entry_count) return;
    Fl_FileEntry *e = &b->entries[row->index];
    int mods = b->window ? ca_window_mouse_mods(b->window) : 0;
    bool many = b->multiple && b->mode == FL_FILE_BROWSER_OPEN_FILES && !e->directory;
    if (many && (mods & (CA_MOD_SUPER | CA_MOD_CONTROL))) e->selected = !e->selected;
    else if (many && (mods & CA_MOD_SHIFT) && b->anchor < b->entry_count) {
        size_t from = b->anchor < row->index ? b->anchor : row->index, to = b->anchor < row->index ? row->index : b->anchor;
        for (size_t i = 0; i < b->entry_count; ++i) b->entries[i].selected = i >= from && i <= to && !b->entries[i].directory;
    } else if (e->selected && selected_count(b, false) == 1) {
        if (e->directory) {
            char path[FL_PATH_MAX];
            if (fl_fs_join(path, sizeof(path), b->current, e->name) && !enter(b, path, true)) {
                snprintf(b->message, sizeof(b->message), "This folder cannot be opened.");
                if (b->root) ca_div_invalidate(b->root);
            }
        } else confirm(b);
        return;
    } else for (size_t i = 0; i < b->entry_count; ++i) b->entries[i].selected = i == row->index;
    b->anchor = row->index;
    if (b->root) ca_div_invalidate(b->root);
}

/* Format a byte count with binary units. */
static void format_size(int64_t size, char *out, size_t capacity)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = size < 0 ? 0 : (double)size; size_t unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    snprintf(out, capacity, unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
}

/* Emit one icon toolbar button. */
static void tool(const char *icon, Ca_ClickFn fn, Fl_FileBrowser *b, bool disabled)
{
    ca_btn_begin(&(Ca_BtnDesc){.text = icon, .on_click = fn, .click_data = b, .style = FL_CLASS_ICON_ACTION, .disabled = disabled});
    ca_btn_end();
}

/* Rebuild the whole browser from controller state. */
static void build(Ca_Div *root, void *data)
{
    (void)root; Fl_FileBrowser *b = data;
    size_t shown = b->entry_count < FL_STYLE_FILE_BROWSER_LIMIT ? b->entry_count : FL_STYLE_FILE_BROWSER_LIMIT;
    if (shown > b->row_capacity) {
        Fl_FileRow *rows = realloc(b->rows, shown * sizeof(*rows));
        if (rows) { b->rows = rows; b->row_capacity = shown; } else shown = b->row_capacity;
    }
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_FILE_BROWSER});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TOOLBAR});
    tool(CA_ICON_NF_FA_ARROW_LEFT, back_clicked, b, b->history_pos == 0);
    tool(CA_ICON_NF_FA_ARROW_RIGHT, forward_clicked, b, b->history_pos + 1 >= b->history_count);
    tool(CA_ICON_NF_FA_LEVEL_UP, up_clicked, b, false);
    tool(CA_ICON_NF_FA_HOME, home_clicked, b, false);
    tool(CA_ICON_NF_FA_REFRESH, refresh_clicked, b, false);
    ca_input(&(Ca_InputDesc){.text = b->draft, .id = "file-browser-path", .style = FL_CLASS_PROPERTY_INPUT,
        .on_change = draft_changed, .change_data = b});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Go", .on_click = go_clicked, .click_data = b, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TOOLBAR});
    if (b->filter_count > 1)
        ca_select(&(Ca_SelectDesc){.options = b->filter_labels, .option_count = (int)b->filter_count, .selected = (int)b->active_filter,
            .id = "file-browser-filter", .style = FL_CLASS_PROPERTY_SELECT, .on_change = filter_changed, .change_data = b});
    ca_checkbox(&(Ca_CheckboxDesc){.text = "Show hidden", .checked = b->show_hidden, .on_change = hidden_changed, .change_data = b});
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = LIST_ID, .style = FL_CLASS_FILE_LIST});
    if (!b->entry_count) ca_text(&(Ca_TextDesc){.text = b->message[0] ? b->message : "Nothing to show here.", .style = FL_CLASS_MUTED});
    for (size_t i = 0; i < shown; ++i) {
        const Fl_FileEntry *e = &b->entries[i];
        b->rows[i] = (Fl_FileRow){b, i};
        char label[320], size[32];
        snprintf(label, sizeof(label), "%s  %s", e->directory ? CA_ICON_NF_FA_FOLDER : CA_ICON_NF_FA_FILE, e->name);
        ca_btn_begin(&(Ca_BtnDesc){.direction = CA_HORIZONTAL, .on_click = entry_clicked, .click_data = &b->rows[i],
            .style = e->selected ? FL_CLASS_FILE_ROW_SELECTED : FL_CLASS_FILE_ROW});
        ca_text(&(Ca_TextDesc){.text = label, .style = FL_CLASS_LABEL});
        if (!e->directory) { format_size(e->size, size, sizeof(size)); ca_text(&(Ca_TextDesc){.text = size, .style = FL_CLASS_MUTED}); }
        ca_btn_end();
    }
    ca_div_end();
    char summary[320];
    size_t files = selected_count(b, true);
    if (b->entry_count > shown) snprintf(summary, sizeof(summary), "Showing %zu of %zu entries. Narrow with the filter.", shown, b->entry_count);
    else if (b->entry_count && b->message[0]) snprintf(summary, sizeof(summary), "%s", b->message);
    else if (b->mode == FL_FILE_BROWSER_SELECT_FOLDER) snprintf(summary, sizeof(summary), "Double-click to open a folder; confirm chooses the selected or current folder.");
    else if (files == 1) for (size_t i = 0; i < b->entry_count; ++i) { if (b->entries[i].selected && !b->entries[i].directory) snprintf(summary, sizeof(summary), "%s", b->entries[i].name); }
    else if (files) snprintf(summary, sizeof(summary), "%zu files selected", files);
    else snprintf(summary, sizeof(summary), b->multiple ? "Select files (Cmd/Ctrl or Shift for several)." : "Select a file.");
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_FILE_FOOTER});
    ca_text(&(Ca_TextDesc){.text = summary, .style = FL_CLASS_FILE_SUMMARY});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Cancel", .on_click = cancel_clicked, .click_data = b, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){.text = b->confirm, .on_click = confirm_clicked, .click_data = b, .style = FL_CLASS_ACTION,
        .disabled = b->mode == FL_FILE_BROWSER_OPEN_FILES && !files}); ca_btn_end();
    ca_div_end();
    ca_div_end();
}

/* Forget window handles when the OS or the user closes the browser. */
static void closed(Ca_Window *window, void *data)
{
    (void)window; Fl_FileBrowser *b = data; b->window = NULL; b->root = NULL;
}

Fl_FileBrowser *fl_file_browser_create(Ca_Instance *instance)
{
    if (!instance) return NULL;
    Fl_FileBrowser *b = calloc(1, sizeof(*b));
    if (b) b->instance = instance;
    return b;
}

void fl_file_browser_destroy(Fl_FileBrowser *b)
{
    if (!b) return;
    if (b->window) ca_window_destroy(b->window);
    clear_entries(b);
    free(b->entries); free(b->rows); free(b);
}

bool fl_file_browser_show(Fl_FileBrowser *b, const Fl_FileBrowserDesc *desc)
{
    if (!b || !desc || !desc->on_confirm || desc->filter_count > FL_FILE_BROWSER_MAX_FILTERS || (desc->filter_count && !desc->filters) ||
        (desc->mode != FL_FILE_BROWSER_OPEN_FILES && desc->mode != FL_FILE_BROWSER_SELECT_FOLDER)) return false;
    for (size_t i = 0; i < desc->filter_count; ++i)
        if (desc->filters[i].extension_count > FL_FILE_BROWSER_MAX_EXTENSIONS || (desc->filters[i].extension_count && !desc->filters[i].extensions)) return false;
    bool folder = desc->mode == FL_FILE_BROWSER_SELECT_FOLDER;
    b->mode = desc->mode; b->multiple = desc->multiple && !folder;
    b->on_confirm = desc->on_confirm; b->user_data = desc->user_data;
    snprintf(b->title, sizeof(b->title), "%s", desc->title ? desc->title : folder ? "Select folder" : "Open files");
    snprintf(b->confirm, sizeof(b->confirm), "%s", desc->confirm_label ? desc->confirm_label : folder ? "Select folder" : "Open");
    b->filter_count = desc->filter_count; b->active_filter = 0;
    for (size_t i = 0; i < desc->filter_count; ++i) {
        Fl_StoredFilter *f = &b->filters[i];
        snprintf(f->label, sizeof(f->label), "%s", desc->filters[i].label ? desc->filters[i].label : "Files");
        f->count = desc->filters[i].extension_count;
        for (size_t j = 0; j < f->count; ++j) snprintf(f->extensions[j], sizeof(f->extensions[j]), "%s", desc->filters[i].extensions[j] ? desc->filters[i].extensions[j] : "");
        b->filter_labels[i] = f->label;
    }
    b->history_count = 0; b->history_pos = 0;
    char home[FL_PATH_MAX];
    if (!desc->initial_path || !enter(b, desc->initial_path, true)) { home_folder(home, sizeof(home)); if (!enter(b, home, true)) return false; }
    if (!b->window) {
        b->window = ca_window_create(b->instance, &(Ca_WindowDesc){.title = b->title, .width = FL_STYLE_FILE_BROWSER_W,
            .height = FL_STYLE_FILE_BROWSER_H, .on_close = closed, .on_close_data = b});
        b->root = b->window ? ca_window_content_div(b->window) : NULL;
        if (!b->root || !ca_div_set_builder(b->root, build, b)) { if (b->window) ca_window_destroy(b->window); b->window = NULL; b->root = NULL; return false; }
    } else {
        ca_window_set_title(b->window, b->title);
        ca_window_focus(b->window);
        ca_div_invalidate(b->root);
    }
    return true;
}

void fl_file_browser_close(Fl_FileBrowser *b)
{
    if (b && b->window) ca_window_close(b->window);
}

bool fl_file_browser_is_open(const Fl_FileBrowser *b) { return b && b->window; }
