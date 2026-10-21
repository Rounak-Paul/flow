#include "fl_scene_geometry.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

Fl_Affine fl_affine_identity(void) { return (Fl_Affine){{1,0,0,0,1,0}}; }
Fl_Affine fl_affine_transform(const Fl_Transform *t)
{
    double c = cos(t->rotation), s = sin(t->rotation);
    return (Fl_Affine){{c*t->scale_x,-s*t->scale_y,t->x,s*t->scale_x,c*t->scale_y,t->y}};
}
Fl_Affine fl_affine_compose(Fl_Affine parent, Fl_Affine local)
{
    const double *a = parent.v, *b = local.v;
    return (Fl_Affine){{a[0]*b[0]+a[1]*b[3], a[0]*b[1]+a[1]*b[4], a[0]*b[2]+a[1]*b[5]+a[2],
        a[3]*b[0]+a[4]*b[3], a[3]*b[1]+a[4]*b[4], a[3]*b[2]+a[4]*b[5]+a[5]}};
}
void fl_affine_point(Fl_Affine m, double x, double y, double *ox, double *oy)
{
    *ox = m.v[0]*x + m.v[1]*y + m.v[2]; *oy = m.v[3]*x + m.v[4]*y + m.v[5];
}
bool fl_affine_inverse(Fl_Affine m, Fl_Affine *out)
{
    const double *a = m.v; double det = a[0]*a[4]-a[1]*a[3];
    if (!out || !isfinite(det) || fabs(det) < 1e-12) return false;
    Fl_Affine inv = {{a[4]/det,-a[1]/det,0,-a[3]/det,a[0]/det,0}};
    inv.v[2] = -inv.v[0]*a[2]-inv.v[1]*a[5]; inv.v[5] = -inv.v[3]*a[2]-inv.v[4]*a[5];
    for (size_t i = 0; i < 6; ++i) if (!isfinite(inv.v[i])) return false;
    *out = inv; return true;
}
void fl_scene_geometry_clear(Fl_SceneGeometry *g)
{
    if (!g) return;
    free(g->entities); free(g->order); *g = (Fl_SceneGeometry){0};
}

bool fl_scene_geometry_update(Fl_SceneGeometry *g, const Fl_Scene *s, const Fl_SceneState *state)
{
    if (!g) return false;
    s=fl_scene_state_scene(state,s);
    if (!s) { fl_scene_geometry_clear(g); return true; }
    if (state && (!state->valid || state->scene != s->id || state->revision != s->revision || state->count != s->entity_count)) state = NULL;
    Fl_Tick tick = state ? state->tick : 0;
    if (g->entities && g->scene == s->id && g->revision == s->revision && g->animated == (state != NULL) && g->tick == tick) return true;
    size_t n = s->entity_count;
    if (n > SIZE_MAX / sizeof(Fl_ResolvedEntity) || n >= SIZE_MAX / sizeof(size_t) / 4) return false;
    Fl_SceneGeometry next = {.scene = s->id, .revision = s->revision, .tick = tick, .animated = state != NULL, .count = n};
    next.entities = n ? calloc(n, sizeof(*next.entities)) : NULL;
    next.order = n ? malloc(n * sizeof(*next.order)) : NULL;
    size_t *work = n ? malloc(n * 4 * sizeof(*work)) : NULL;
    unsigned char *resolved = n ? calloc(n, 1) : NULL;
    if (n && (!next.entities || !next.order || !work || !resolved)) {
        fl_scene_geometry_clear(&next); free(work); free(resolved); return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const Fl_Entity *e = &s->entities[i]; size_t depth = 0;
        while (e && !resolved[e - s->entities] && depth < n) {
            work[depth++] = (size_t)(e - s->entities); e = fl_scene_entity(s, e->parent);
        }
        while (depth) {
            size_t index = work[--depth]; e = &s->entities[index];
            const Fl_Entity *parent = fl_scene_entity(s, e->parent);
            Fl_ResolvedEntity *v = &next.entities[index];
            v->local = (Fl_Transform){.scale_x = 1, .scale_y = 1};
            double opacity = 1;
            if (state) { v->local = state->entities[index].transform; opacity = state->entities[index].opacity; }
            else { fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &v->local); opacity = fl_widget_opacity(e); }
            v->opacity = opacity * (parent ? next.entities[parent-s->entities].opacity : 1);
            v->world = fl_affine_compose(parent ? next.entities[parent-s->entities].world : fl_affine_identity(), fl_affine_transform(&v->local));
            v->invertible = fl_affine_inverse(v->world, &v->inverse);
            v->visible = e->visible && (!parent || next.entities[parent-s->entities].visible);
            const Fl_WidgetDef *def = fl_widget_for_entity(e); v->kind = def ? def->kind : FL_WIDGET_GROUP;
            resolved[index] = 1;
        }
    }
    size_t root = SIZE_MAX, *first = work, *sibling = work ? work + n : NULL, *stack = work ? work + n*2 : NULL;
    for (size_t i = 0; i < n; ++i) first[i] = sibling[i] = SIZE_MAX;
    for (size_t i = n; i; ) {
        --i; const Fl_Entity *parent = fl_scene_entity(s, s->entities[i].parent);
        size_t *head = parent ? &first[parent-s->entities] : &root;
        sibling[i] = *head; *head = i;
    }
    size_t index = root, count = 0, depth = 0;
    while (index != SIZE_MAX) {
        next.order[count++] = index;
        if (first[index] != SIZE_MAX) { stack[depth++] = index; index = first[index]; }
        else { while (sibling[index] == SIZE_MAX && depth) index = stack[--depth]; index = sibling[index]; }
    }
    free(work); free(resolved);
    fl_scene_geometry_clear(g); *g = next; return true;
}

Fl_Id fl_scene_geometry_pick(const Fl_SceneGeometry *g, const Fl_Scene *s, double x, double y, double tolerance)
{
    if (!g || !s || g->count!=s->entity_count || g->scene != s->id || g->revision != s->revision || !isfinite(x) || !isfinite(y)) return 0;
    for (size_t i = g->count; i; ) {
        size_t index = g->order[--i]; const Fl_ResolvedEntity *v = &g->entities[index];
        if (!v->visible || !v->invertible || v->kind == FL_WIDGET_GROUP) continue;
        double lx, ly; fl_affine_point(v->inverse, x, y, &lx, &ly);
        double w = v->local.width, h = v->local.height;
        if (w <= 0 || h <= 0) continue;
        if (v->kind == FL_WIDGET_ELLIPSE || fl_widget_is_polygon(v->kind)) {
            if (!fl_widget_shape_contains(v->kind, lx/w*2-1, ly/h*2-1)) continue;
        } else if (v->kind == FL_WIDGET_LINE || v->kind == FL_WIDGET_ARROW) {
            Fl_Appearance appearance = fl_widget_appearance(&s->entities[index]);
            double local_tolerance = fmax(hypot(v->inverse.v[0], v->inverse.v[3]), hypot(v->inverse.v[1], v->inverse.v[4])) * tolerance;
            double half = fmax(appearance.stroke_width*.5, local_tolerance);
            bool shaft = lx >= -local_tolerance && lx <= (v->kind==FL_WIDGET_ARROW ? w*.8 : w)+local_tolerance && fabs(ly-h*.5) <= half;
            bool head = v->kind == FL_WIDGET_ARROW && lx >= w*.72 && lx <= w && fabs(ly-h*.5) <= (w-lx)/(w*.28)*h*.5;
            if (!shaft && !head) continue;
        } else if (lx < 0 || ly < 0 || lx > w || ly > h) continue;
        return s->entities[index].id;
    }
    return 0;
}
