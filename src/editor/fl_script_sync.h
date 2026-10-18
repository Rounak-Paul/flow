#pragma once
#include "core/fl_event.h"
#include "project/fl_project.h"
#include <causality.h>

/*
 * Edit-in-your-editor for repository scripts. Opening a script writes its source to a private file inside the
 * project, hands it to the system's default editor and keeps the two in step: saves in the editor become one
 * document operation each, and changes made elsewhere (the inspector, MCP, undo) are written back to the file.
 * Sessions end when their script is removed. UI thread only.
 */
typedef struct Fl_ScriptSync Fl_ScriptSync;

/* Create the synchronizer for project, observing bus and polling on instance's frame loop. NULL on failure. */
Fl_ScriptSync *fl_script_sync_create(Fl_Project *project, Fl_EventBus *bus, Ca_Instance *instance);
/* End every session, deleting their files, and free. NULL is safe. */
void fl_script_sync_destroy(Fl_ScriptSync *sync);
/* Start (or resume) editing script of scene in the system editor. False with a reason in error on failure. */
bool fl_script_sync_open(Fl_ScriptSync *sync, Fl_Id scene, Fl_Id script, char *error, size_t error_size);
