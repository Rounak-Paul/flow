#pragma once
#include <causality.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * In-app file browser window: navigate folders (back/forward/up/home, typed
 * path), filter by extension and pick files or a folder. One browser serves
 * one request at a time; showing a new request replaces the previous one.
 */
typedef struct Fl_FileBrowser Fl_FileBrowser;
typedef enum Fl_FileBrowserMode {
    FL_FILE_BROWSER_OPEN_FILES,     /* choose one or more existing files */
    FL_FILE_BROWSER_SELECT_FOLDER   /* choose a folder (the selected one, else the current one) */
} Fl_FileBrowserMode;
/* Extension filter; extensions are lowercase without the dot. Zero extensions matches every file. */
typedef struct Fl_FileFilter {
    const char *label;
    const char *const *extensions;
    size_t extension_count;
} Fl_FileFilter;
/*
 * Confirmation callback, invoked after the browser window has been asked to close.
 *
 * paths      UTF-8 absolute paths, borrowed for the duration of the call.
 * count      Number of paths, always > 0.
 * user_data  Pointer supplied in Fl_FileBrowserDesc.
 */
typedef void (*Fl_FileBrowserFn)(const char *const *paths, size_t count, void *user_data);
typedef struct Fl_FileBrowserDesc {
    Fl_FileBrowserMode mode;
    const char *title;              /* window title; NULL picks one from mode */
    const char *confirm_label;      /* confirm button; NULL picks one from mode */
    const char *initial_path;       /* starting folder; NULL or unreadable starts at home */
    const Fl_FileFilter *filters;   /* copied; the first is active initially */
    size_t filter_count;            /* at most FL_FILE_BROWSER_MAX_FILTERS */
    bool multiple;                  /* OPEN_FILES: modifier-click selects several files */
    Fl_FileBrowserFn on_confirm;    /* required */
    void *user_data;
} Fl_FileBrowserDesc;
#define FL_FILE_BROWSER_MAX_FILTERS 8
#define FL_FILE_BROWSER_MAX_EXTENSIONS 32

/* Create a hidden browser on instance; NULL on failure. */
Fl_FileBrowser *fl_file_browser_create(Ca_Instance *instance);
/* Close the window, drop any pending request and free. NULL is safe. */
void fl_file_browser_destroy(Fl_FileBrowser *browser);
/* Open (or refocus) the browser window for a request; false on invalid input or window failure. */
bool fl_file_browser_show(Fl_FileBrowser *browser, const Fl_FileBrowserDesc *desc);
/* Close the window without confirming. */
void fl_file_browser_close(Fl_FileBrowser *browser);
/* Report whether the browser window is open. */
bool fl_file_browser_is_open(const Fl_FileBrowser *browser);
