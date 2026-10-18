#pragma once
/* Project-module internals for the scene script repository. Not part of the public API. */
#include "fl_project.h"
#include "doc/fl_binary.h"

/* Release scene-owned script sources. */
void fl_scripts_free(Fl_Scene *scene);
/* Apply FL_OP_SCRIPT_SET or FL_OP_SCRIPT_REMOVE; false leaves the scene unchanged. created receives a new script's id. */
bool fl_scripts_apply(Fl_Scene *scene, const Fl_Op *op, uint64_t *next_id, Fl_Id *created);
/* Require every Script component in components to name a script of scene. */
bool fl_scripts_references_valid(const Fl_Scene *scene, const Fl_Component *components, size_t count);
/* Validate a loaded repository: ids below next_id, unique names, storable sources, resolvable entity references. */
bool fl_scripts_valid(const Fl_Scene *scene, uint64_t next_id);
/* Copy the repository's persistent ids into ids (script_count of them). */
void fl_scripts_ids(const Fl_Scene *scene, Fl_Id *ids);
/* Serialize or deserialize the repository. */
void fl_scripts_save(const Fl_Scene *scene, Fl_Binary *stream);
bool fl_scripts_load(Fl_Scene *scene, Fl_Binary *stream);
