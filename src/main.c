#include "fl_editor.h"

/*
 * Entry point: run the editor until every window is closed.
 *
 * Returns  0 on clean shutdown, 1 when the editor fails to start.
 */
int main(void)
{
    Fl_Editor *editor = fl_editor_create();
    if (!editor) return 1;
    int status = fl_editor_run(editor);
    fl_editor_destroy(editor);
    return status;
}
