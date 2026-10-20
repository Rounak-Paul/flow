#pragma once
#include "fl_project.h"

/* Release an entity's owned name and components; caller owns its record. */
void fl_entity_storage_clear(Fl_Entity *entity);
/* Release all owned scene storage and reset the record, including partially decoded documents. */
void fl_scene_storage_clear(Fl_Scene *scene);
/* Reserve index capacity for count entities before changing ordered storage. */
bool fl_scene_storage_reserve_index(Fl_Scene *scene, size_t count);
/* Refresh positions after mutations; requires a previously reserved index. */
void fl_scene_storage_rebuild_index(Fl_Scene *scene);
/* Insert one newly appended entity after index capacity was reserved; identities must be unique. */
void fl_scene_storage_index_entity(Fl_Scene *scene, size_t position);
