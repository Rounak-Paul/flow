#pragma once
#include "fl_scene_panel.h"
#include "project/fl_asset_import.h"
#include <ca_dock.h>

typedef struct Fl_AssetPanel Fl_AssetPanel;
/* Create the Assets panel controller borrowing edit, scene drop target, importer, jobs (previews) and bus. */
Fl_AssetPanel *fl_asset_panel_create(Fl_SceneEdit *edit, Fl_ScenePanel *scene, Fl_AssetImporter *importer,
                                     Fl_JobSystem *jobs, Fl_EventBus *bus);
/* Release subscriptions, retained drafts and the rename window after dock destruction. */
void fl_asset_panel_destroy(Fl_AssetPanel *panel);
/* Dock callbacks build the searchable asset library and detach borrowed window handles. */
void fl_asset_panel_build(Ca_Dock *dock, Ca_Window *window, void *data);
void fl_asset_panel_teardown(void *data);
/* Import OS-dropped files (directories contribute their visible files); images dropped over
   the scene are placed at the drop point once imported. */
void fl_asset_panel_drop_files(Fl_AssetPanel *panel, const char *const *paths, int count, double screen_x, double screen_y);
