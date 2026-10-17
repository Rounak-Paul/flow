#pragma once
#include "project/fl_project.h"
#include <causality.h>
typedef struct Fl_Launcher Fl_Launcher;
/* Create a separate project manager; successful create/open is returned by take_project. */
Fl_Launcher *fl_launcher_create(Ca_Instance *instance, Fl_EventBus *bus);
/* Transfer a selected, locked project to the editor, once. NULL while no selection exists. */
Fl_Project *fl_launcher_take_project(Fl_Launcher *launcher);
/* Report whether the user has closed the launcher. */
bool fl_launcher_is_open(const Fl_Launcher *launcher);
/* Close the manager and release recent-list/form storage. NULL is safe. */
void fl_launcher_destroy(Fl_Launcher *launcher);
