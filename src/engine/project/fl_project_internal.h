#pragma once
#include "fl_project.h"
/* Assign a private resource root before publishing a newly created immutable evaluation view. Owner thread only. */
bool fl_project_read_view_rebase(Fl_Project *view, const char *directory);
