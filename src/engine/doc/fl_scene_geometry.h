#pragma once
#include "time/fl_scene_state.h"
#include "doc/fl_math.h"

/* XY footprint of a world matrix, used by planar tools and layer clips. [a, b, tx, c, d, ty]. */
typedef struct Fl_Affine { double v[6]; } Fl_Affine;
/* Resolved document geometry; entries correspond to scene entity indices. */
typedef struct Fl_ResolvedEntity {
    Fl_Mat4 model, inverse_model;
    Fl_Affine world, inverse;
    Fl_Pose pose;
    Fl_Bounds bounds;
    Fl_WidgetKind kind;
    double opacity;             /* own opacity times every ancestor's */
    bool visible, invertible, mesh;
} Fl_ResolvedEntity;
typedef struct Fl_SceneGeometry {
    Fl_Id scene;
    uint64_t revision;
    Fl_Tick tick;
    bool animated;              /* resolved from a scene state rather than base components */
    Fl_ResolvedEntity *entities;
    size_t *order;
    size_t count;
} Fl_SceneGeometry;
/* Return identity/local matrices and compose parent * local. */
Fl_Affine fl_affine_identity(void);
Fl_Affine fl_affine_from_mat4(Fl_Mat4 model);
Fl_Affine fl_affine_pose(const Fl_Pose *pose);
Fl_Affine fl_affine_compose(Fl_Affine parent, Fl_Affine local);
/* Invert matrix, rejecting singular/non-finite results; out is unchanged on failure. */
bool fl_affine_inverse(Fl_Affine matrix, Fl_Affine *out);
/* Map local x/y through matrix into out_x/out_y. */
void fl_affine_point(Fl_Affine matrix, double x, double y, double *out_x, double *out_y);
/* Resolve geometry transactionally in linear time; keep prior cache on allocation failure.
   state (optional) supplies evaluated local transforms/opacity at a time; NULL uses base components. */
bool fl_scene_geometry_update(Fl_SceneGeometry *geometry, const Fl_Scene *scene, const Fl_SceneState *state);
/* Free all cache storage and reset geometry. */
void fl_scene_geometry_clear(Fl_SceneGeometry *geometry);
/* Pick the topmost visible primitive at scene x/y; tolerance is measured in scene pixels. */
Fl_Id fl_scene_geometry_pick(const Fl_SceneGeometry *geometry, const Fl_Scene *scene,
                             double x, double y, double tolerance);
