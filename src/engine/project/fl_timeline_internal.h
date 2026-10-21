#pragma once
/* Project-module internals for scene timelines (beats, tracks, clips). Not part of the public API. */
#include "fl_project.h"
#include "doc/fl_binary.h"

/* One property value written as a keyframe by a keyed transform/configure operation. */
typedef struct Fl_KeyWrite { Fl_Id entity; Fl_AnimProperty property; double value[4]; } Fl_KeyWrite;

/* Release scene-owned beats and audio clips; entities own animation tracks. */
void fl_timeline_free(Fl_Scene *scene);
/* Apply a timeline operation (FL_OP_BEAT_* .. FL_OP_CLIP_*); false leaves the scene unchanged. */
bool fl_timeline_apply(const Fl_Project *project, Fl_Scene *scene, const Fl_Op *op, uint64_t *next_id, Fl_Id *created);
/* Make every write committable at tick without failing: creates missing tracks and grows key storage.
   On false, the scene is restored exactly. */
bool fl_timeline_reserve(Fl_Scene *scene, const Fl_KeyWrite *writes, size_t count, uint64_t next_id);
/* Commit reserved writes at tick; infallible after fl_timeline_reserve succeeded. */
void fl_timeline_commit(Fl_Scene *scene, const Fl_KeyWrite *writes, size_t count, Fl_Tick tick, uint64_t *next_id);
/* Count clips referencing asset. */
size_t fl_timeline_asset_uses(const Fl_Scene *scene, Fl_Id asset);
/* Validate loaded references and values (beats, tracks, clips) against the scene and project. */
bool fl_timeline_valid(const Fl_Project *project, const Fl_Scene *scene, uint64_t next_id);
/* Number of persistent ids owned by the timeline, and copy them into ids. */
size_t fl_timeline_id_count(const Fl_Scene *scene);
void fl_timeline_ids(const Fl_Scene *scene, Fl_Id *ids);
/* Serialize/deserialize scene beats and audio clips; entity components carry the tracks. */
void fl_timeline_save(const Fl_Scene *scene, Fl_Binary *stream);
bool fl_timeline_load(Fl_Scene *scene, Fl_Binary *stream);

/* Split animated configure channels into native key writes while retaining authored bases.
   Returns SIZE_MAX for invalid/duplicate input components.
   Scratch byte arrays remain borrowed by rewritten components until the caller commits them. */
size_t fl_keyed_components(const Fl_Scene *scene, const Fl_Entity *entity, Fl_Component *components, size_t count,
                            unsigned char transform[56], unsigned char appearance[24], unsigned char opacity[8], unsigned char style[91],
                            Fl_KeyWrite *writes, Fl_Tick tick);

/* Whether a proposed native key value differs from the track sample at tick. */
bool fl_keyed_value_changed(const Fl_Scene *scene, const Fl_Track *track, Fl_Tick tick, const double value[4]);
