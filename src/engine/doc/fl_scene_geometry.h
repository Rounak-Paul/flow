#pragma once
#include "schema/fl_widgets.h"

/* Local-to-scene affine matrix [a,b,tx,c,d,ty]. */
typedef struct Fl_Affine { double v[6]; } Fl_Affine;
/* Resolved document geometry; entries correspond to scene entity indices. */
typedef struct Fl_ResolvedEntity {
    Fl_Affine world, inverse;
    Fl_Transform local;
    Fl_WidgetKind kind;
    bool visible, invertible;
} Fl_ResolvedEntity;
typedef struct Fl_SceneGeometry {
    Fl_Id scene;
    uint64_t revision;
    Fl_ResolvedEntity *entities;
    size_t *order;
    size_t count;
} Fl_SceneGeometry;
/* Return identity/local matrices and compose parent * local. */
Fl_Affine fl_affine_identity(void);
Fl_Affine fl_affine_transform(const Fl_Transform *transform);
Fl_Affine fl_affine_compose(Fl_Affine parent, Fl_Affine local);
/* Invert matrix, rejecting singular/non-finite results; out is unchanged on failure. */
bool fl_affine_inverse(Fl_Affine matrix, Fl_Affine *out);
/* Map local x/y through matrix into out_x/out_y. */
void fl_affine_point(Fl_Affine matrix, double x, double y, double *out_x, double *out_y);
/* Resolve geometry transactionally in linear time; keep prior cache on allocation failure. */
bool fl_scene_geometry_update(Fl_SceneGeometry *geometry, const Fl_Scene *scene);
/* Free all cache storage and reset geometry. */
void fl_scene_geometry_clear(Fl_SceneGeometry *geometry);
/* Pick the topmost visible primitive at scene x/y; tolerance is measured in scene pixels. */
Fl_Id fl_scene_geometry_pick(const Fl_SceneGeometry *geometry, const Fl_Scene *scene,
                             double x, double y, double tolerance);
