#include "fl_editor.h"
#include "core/fl_log.h"

/*
 * Entry point: run the editor until every window is closed. An optional argument names a project folder to open
 * directly instead of showing the project launcher.
 *
 * Returns  0 on clean shutdown, 1 when the editor fails to start or the named project cannot be opened.
 */
int main(int argc, char **argv)
{
    Fl_Editor *editor = fl_editor_create();
    if (!editor) return 1;
    int status = argc > 1 && !fl_editor_open_project(editor, argv[1]) ? 1 : fl_editor_run(editor);
    fl_editor_destroy(editor);
    return status;
}
