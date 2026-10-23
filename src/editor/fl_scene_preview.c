#include "fl_render_host.h"
#include "fl_scene_preview.h"
#include "render/gpu/fl_scene_renderer.h"
#include "components/fl_component.h"
#include "components/fl_spatial.h"
#include "style.h"
#include <ca_gpu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct Fl_ScenePreview {
    Fl_SceneEdit *edit;
    Fl_Fonts *fonts;
    Fl_JobSystem *jobs;
    Fl_EventBus *bus;
    Fl_PreviewInvalidateFn invalidate;
    void *invalidate_data;
    Fl_SceneRenderer *renderer;     /* created with the first frame, when the GPU instance is known */
    double eye_x, eye_y, eye_z;     /* editor camera; not stored in the document */
    double yaw, pitch;
    Fl_Id framed_scene, view_camera; /* view_camera 0 is the editor camera */
    bool frame_pending, shot, flat;  /* shot looks through view_camera; flat locks the camera to the canvas */
    Fl_EditTool tool;
    bool gizmo_armed;
    Fl_EditGesture gizmo_gesture;
    Fl_Vec3 gizmo_origin, gizmo_axis, gizmo_normal, gizmo_hit, gizmo_u, gizmo_v;
};

typedef struct PreviewView {
    Fl_Mat4 view;
    Fl_Vec3 right, up, look, position;
    double bloom, fov_y, subject_distance;
    bool perspective;
} PreviewView;

static const Fl_Scene *preview_scene(Fl_ScenePreview *p, const Fl_SceneState **state_out)
{
    const Fl_Scene *scene = fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
    const Fl_SceneState *state = fl_scene_edit_state(p->edit);
    scene = fl_scene_state_scene(state, scene);
    if (state && scene && (state->scene != scene->id || state->count != scene->entity_count)) state = NULL;
    if (state_out) *state_out = state;
    return scene;
}

static const double EDITOR_FOV = 0.9;

static void editor_basis(double yaw, double pitch, Fl_Vec3 *right, Fl_Vec3 *up, Fl_Vec3 *look)
{
    double cp = cos(pitch), sp = sin(pitch), cy = cos(yaw), sy = sin(yaw);
    *look = (Fl_Vec3){sy * cp, -sp, -cy * cp};
    *right = (Fl_Vec3){cy, 0, sy};
    *up = (Fl_Vec3){sy * sp, cp, -cy * sp};
}

static Fl_Mat4 editor_proj(Fl_Vec3 right, Fl_Vec3 up, Fl_Vec3 look, Fl_Vec3 eye, double aspect)
{
    Fl_Mat4 world = fl_mat4_identity(), view;
    world.m[0] = right.x; world.m[1] = right.y; world.m[2] = right.z;
    world.m[4] = up.x; world.m[5] = up.y; world.m[6] = up.z;
    world.m[8] = -look.x; world.m[9] = -look.y; world.m[10] = -look.z;
    world.m[12] = eye.x; world.m[13] = eye.y; world.m[14] = eye.z;
    if (!fl_mat4_inverse(world, &view)) view = fl_mat4_identity();
    return fl_mat4_mul(fl_mat4_perspective(EDITOR_FOV, aspect > 0 ? aspect : 1, 0.1, 100000), view);
}

static void frame_editor(Fl_ScenePreview *p, const Fl_Scene *scene, const Fl_SceneFrame *gate)
{
    double height = gate && gate->height > 1 ? gate->height : (scene && scene->height ? (double)scene->height : 1080);
    double width = gate && gate->width > 1 ? gate->width : (scene && scene->width ? (double)scene->width : 1920);
    double cx = gate && gate->width > 0 ? gate->x + gate->width * 0.5 : width * 0.5;
    double cy = gate && gate->height > 0 ? gate->y + gate->height * 0.5 : height * 0.5;
    double dist = height * 0.5 / tan(EDITOR_FOV * 0.5) * 1.25;
    if (dist < 1) dist = 1;
    p->eye_x = cx; p->eye_y = cy; p->eye_z = dist;
    p->yaw = p->pitch = 0;
    p->framed_scene = scene ? scene->id : 0;
    p->frame_pending = false;
}

/* Picture rectangle for one camera's aspect, centered on the canvas. */
static Fl_SceneFrame camera_picture(const Fl_Scene *scene, const Fl_Camera *camera)
{
    double width = scene && scene->width ? (double)scene->width : 0, height = scene && scene->height ? (double)scene->height : 0;
    uint64_t aspect_w = camera && camera->aspect_w && camera->aspect_h ? camera->aspect_w : 0;
    uint64_t aspect_h = camera && camera->aspect_w && camera->aspect_h ? camera->aspect_h : 0;
    if (aspect_w && aspect_h && width > 0 && height > 0) {
        double ratio = (double)aspect_w / (double)aspect_h;
        if (width / height > ratio) width = height * ratio;
        else height = width / ratio;
    }
    return (Fl_SceneFrame){scene ? (scene->width - width) * .5 : 0, scene ? (scene->height - height) * .5 : 0, width, height};
}

/* Editor fly camera, or the hierarchy camera chosen in the toolbar. */
static bool preview_view(Fl_ScenePreview *p, const Fl_Scene *scene, const Fl_SceneState *state, double view_w, double view_h, Fl_SceneFrame *gate, PreviewView *out)
{
    memset(out, 0, sizeof(*out));
    out->right = (Fl_Vec3){1, 0, 0}; out->up = (Fl_Vec3){0, 1, 0}; out->look = (Fl_Vec3){0, 0, -1};
    Fl_SceneFrame picture = fl_scene_gate(scene, state);
    if (p->view_camera) {
        Fl_Pose pose = fl_pose_identity(); Fl_Camera camera = fl_camera_default(); double bloom = 0;
        if (fl_scene_camera_entity(scene, state, p->view_camera, &pose, &camera, &bloom)) {
            picture = camera_picture(scene, &camera);
            if (gate) *gate = picture;
            if (!(picture.width > 0) || !(picture.height > 0)) return false;
            Fl_Mat4 axes = fl_pose_model(&pose);
            out->right = (Fl_Vec3){axes.m[0], axes.m[1], axes.m[2]};
            out->up = (Fl_Vec3){axes.m[4], axes.m[5], axes.m[6]};
            out->look = (Fl_Vec3){-axes.m[8], -axes.m[9], -axes.m[10]};
            out->position = (Fl_Vec3){pose.x, pose.y, pose.z};
            out->fov_y = camera.fov_y;
            out->perspective = camera.perspective;
            out->bloom = bloom;
            out->view = fl_camera_view_proj(&pose, &camera, picture.x, picture.y, picture.width, picture.height);
            out->subject_distance = 1;
            p->shot = true;
            return true;
        }
    }
    p->shot = false;
    if (gate) *gate = picture;
    if (!(picture.width > 0) || !(picture.height > 0)) return false;
    if (!p->framed_scene || !scene || p->framed_scene != scene->id || p->frame_pending) frame_editor(p, scene, &picture);
    editor_basis(p->yaw, p->pitch, &out->right, &out->up, &out->look);
    out->position = (Fl_Vec3){p->eye_x, p->eye_y, p->eye_z};
    double dx = picture.x + picture.width * 0.5 - p->eye_x, dy = picture.y + picture.height * 0.5 - p->eye_y, dz = -p->eye_z;
    out->subject_distance = fabs(dx * out->look.x + dy * out->look.y + dz * out->look.z);
    if (out->subject_distance < 1) out->subject_distance = 1;
    out->fov_y = EDITOR_FOV;
    out->view = editor_proj(out->right, out->up, out->look, out->position, view_h > 0 ? view_w / view_h : 1);
    out->perspective = true;
    return true;
}

static bool window_ndc(Ca_Viewport *viewport, double wx, double wy, bool outside, double *ndc_x, double *ndc_y)
{
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    if (w <= 0 || h <= 0 || !isfinite(wx) || !isfinite(wy)) return false;
    if (!outside && (wx < vx || wy < vy || wx > vx + w || wy > vy + h)) return false;
    *ndc_x = ((wx - vx) / w) * 2 - 1;
    *ndc_y = ((wy - vy) / h) * 2 - 1;
    return true;
}

/* Playback draws the shot inside a letterbox. Pointers use that picture, not the full panel. */
static bool picture_ndc(Fl_ScenePreview *p, Ca_Viewport *viewport, double wx, double wy, bool outside, double *ndc_x, double *ndc_y)
{
    if (!p || !p->shot) return window_ndc(viewport, wx, wy, outside, ndc_x, ndc_y);
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    Fl_SceneFrame gate = fl_scene_gate(scene, state);
    if (!(gate.width > 0) || !(gate.height > 0) || w <= 0 || h <= 0 || !isfinite(wx) || !isfinite(wy)) return false;
    double picture = gate.width / gate.height, panel = (double)w / (double)h;
    double box_x, box_y, box_w, box_h;
    if (panel > picture) { box_h = h; box_w = h * picture; box_x = vx + ((double)w - box_w) * 0.5; box_y = vy; }
    else { box_w = w; box_h = (double)w / picture; box_x = vx; box_y = vy + ((double)h - box_h) * 0.5; }
    if (!outside && (wx < box_x || wy < box_y || wx > box_x + box_w || wy > box_y + box_h)) return false;
    if (!(box_w > 0) || !(box_h > 0)) return false;
    *ndc_x = ((wx - box_x) / box_w) * 2 - 1;
    *ndc_y = ((wy - box_y) / box_h) * 2 - 1;
    return isfinite(*ndc_x) && isfinite(*ndc_y);
}

static bool unproject(Fl_Mat4 vp, double ndc_x, double ndc_y, double ndc_z, Fl_Vec3 *out)
{
    Fl_Mat4 inverse;
    if (!fl_mat4_inverse(vp, &inverse)) return false;
    double x = inverse.m[0] * ndc_x + inverse.m[4] * ndc_y + inverse.m[8] * ndc_z + inverse.m[12];
    double y = inverse.m[1] * ndc_x + inverse.m[5] * ndc_y + inverse.m[9] * ndc_z + inverse.m[13];
    double z = inverse.m[2] * ndc_x + inverse.m[6] * ndc_y + inverse.m[10] * ndc_z + inverse.m[14];
    double w = inverse.m[3] * ndc_x + inverse.m[7] * ndc_y + inverse.m[11] * ndc_z + inverse.m[15];
    if (!isfinite(w) || fabs(w) < 1e-12) return false;
    *out = (Fl_Vec3){x / w, y / w, z / w};
    return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
}

static bool view_ray(const PreviewView *view, double ndc_x, double ndc_y, Fl_Vec3 *origin, Fl_Vec3 *direction)
{
    Fl_Vec3 near_point, far_point;
    if (!unproject(view->view, ndc_x, ndc_y, 0, &near_point) || !unproject(view->view, ndc_x, ndc_y, 1, &far_point)) return false;
    *origin = near_point;
    *direction = (Fl_Vec3){far_point.x - near_point.x, far_point.y - near_point.y, far_point.z - near_point.z};
    return true;
}

static void letterbox_rect(float vx, float vy, float vw, float vh, const Fl_SceneFrame *gate, float *x, float *y, float *w, float *h)
{
    *x = vx; *y = vy; *w = vw; *h = vh;
    if (!gate || !(gate->width > 0) || !(gate->height > 0) || vw <= 0 || vh <= 0) return;
    double picture = gate->width / gate->height, panel = (double)vw / (double)vh;
    if (panel > picture) { *h = vh; *w = (float)(vh * picture); *x = vx + (vw - *w) * 0.5f; }
    else { *w = vw; *h = (float)(vw / picture); *y = vy + (vh - *h) * 0.5f; }
}

static bool project_window(const PreviewView *view, Ca_Viewport *viewport, bool letterbox, const Fl_SceneFrame *gate, Fl_Vec3 world, double *sx, double *sy)
{
    double x = view->view.m[0] * world.x + view->view.m[4] * world.y + view->view.m[8] * world.z + view->view.m[12];
    double y = view->view.m[1] * world.x + view->view.m[5] * world.y + view->view.m[9] * world.z + view->view.m[13];
    double w = view->view.m[3] * world.x + view->view.m[7] * world.y + view->view.m[11] * world.z + view->view.m[15];
    float vx, vy, vw, vh, bx, by, bw, bh;
    ca_viewport_screen_rect(viewport, &vx, &vy, &vw, &vh);
    if (letterbox) letterbox_rect(vx, vy, vw, vh, gate, &bx, &by, &bw, &bh);
    else { bx = vx; by = vy; bw = vw; bh = vh; }
    if (!isfinite(w) || w <= 1e-8 || bw <= 0 || bh <= 0) return false;
    *sx = bx + (x / w + 1) * 0.5 * bw;
    *sy = by + (y / w + 1) * 0.5 * bh;
    return isfinite(*sx) && isfinite(*sy);
}

static double pixels_per_world(const PreviewView *view, float viewport_h, Fl_Vec3 point)
{
    double depth = (point.x - view->position.x) * view->look.x + (point.y - view->position.y) * view->look.y + (point.z - view->position.z) * view->look.z;
    if (depth < 0.25) depth = 0.25;
    double span = 2 * depth * tan(view->fov_y * 0.5);
    if (!(span > 1e-6) || viewport_h <= 0) return 0;
    return viewport_h / span;
}

static void preview_redraw(Fl_ScenePreview *p, Ca_Viewport *viewport)
{
    if (viewport) ca_viewport_request_redraw(viewport);
    else if (p->invalidate) p->invalidate(p->invalidate_data);
}

bool fl_scene_preview_shot(const Fl_ScenePreview *p) { return p && p->shot; }
bool fl_scene_preview_flat(const Fl_ScenePreview *p) { return p && p->flat; }
Fl_EditTool fl_scene_preview_tool(const Fl_ScenePreview *p) { return p ? p->tool : FL_TOOL_MOVE; }

void fl_scene_preview_set_tool(Fl_ScenePreview *p, Fl_EditTool tool)
{
    if (!p || tool > FL_TOOL_RECT || p->tool == tool) return;
    p->tool = tool;
    preview_redraw(p, NULL);
}

void fl_scene_preview_set_flat(Fl_ScenePreview *p, bool flat)
{
    if (!p || p->flat == flat) return;
    p->flat = flat;
    if (flat) p->frame_pending = true;
    preview_redraw(p, NULL);
}

void fl_scene_preview_set_camera(Fl_ScenePreview *p, Fl_Id camera)
{
    if (!p || p->view_camera == camera) return;
    p->view_camera = camera;
    p->shot = camera != 0;
    preview_redraw(p, NULL);
}

bool fl_scene_preview_point(Fl_ScenePreview *p, Ca_Viewport *viewport, double wx, double wy, bool outside, double *sx, double *sy)
{
    if (!p || !viewport || !sx || !sy || !isfinite(wx) || !isfinite(wy)) return false;
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    PreviewView view;
    if (w <= 0 || h <= 0 || !preview_view(p, scene, state, w, h, NULL, &view)) return false;
    double ndc_x, ndc_y;
    if (!picture_ndc(p, viewport, wx, wy, outside, &ndc_x, &ndc_y)) return false;
    Fl_Vec3 origin, direction;
    if (!view_ray(&view, ndc_x, ndc_y, &origin, &direction) || fabs(direction.z) < 1e-8) return false;
    double t = -origin.z / direction.z;
    if (t < 0) return false;
    *sx = origin.x + direction.x * t; *sy = origin.y + direction.y * t;
    return isfinite(*sx) && isfinite(*sy);
}

Fl_Id fl_scene_preview_pick(Fl_ScenePreview *p, Ca_Viewport *viewport, double wx, double wy)
{
    if (!p || !viewport) return 0;
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    PreviewView view;
    if (w <= 0 || h <= 0 || !preview_view(p, scene, state, w, h, NULL, &view)) return 0;
    double ndc_x, ndc_y;
    if (!picture_ndc(p, viewport, wx, wy, false, &ndc_x, &ndc_y)) return 0;
    Fl_Vec3 origin, direction;
    if (!view_ray(&view, ndc_x, ndc_y, &origin, &direction)) return 0;
    return fl_scene_edit_pick_ray(p->edit, origin, direction);
}

static bool editor_step(Fl_ScenePreview *p, Ca_Viewport *viewport, PreviewView *view, float *h)
{
    if (!p || p->shot || !viewport) return false;
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    float x, y, w;
    ca_viewport_screen_rect(viewport, &x, &y, &w, h);
    return *h > 0 && preview_view(p, scene, state, w, *h, NULL, view) && view->perspective;
}

void fl_scene_preview_pan(Fl_ScenePreview *p, Ca_Viewport *viewport, double dx, double dy)
{
    if (!p || !viewport || !isfinite(dx) || !isfinite(dy) || (dx == 0 && dy == 0)) return;
    PreviewView view; float h = 0;
    if (!editor_step(p, viewport, &view, &h)) return;
    double per = 2 * view.subject_distance * tan(view.fov_y * 0.5) / h;
    if (!(per > 0) || !isfinite(per)) return;
    p->eye_x -= (view.right.x * dx + view.up.x * dy) * per;
    p->eye_y -= (view.right.y * dx + view.up.y * dy) * per;
    p->eye_z -= (view.right.z * dx + view.up.z * dy) * per;
    preview_redraw(p, viewport);
}

void fl_scene_preview_look(Fl_ScenePreview *p, Ca_Viewport *viewport, double dx, double dy)
{
    if (!p || p->shot || p->flat || !isfinite(dx) || !isfinite(dy) || (dx == 0 && dy == 0)) return;
    p->yaw -= dx * 0.005; p->pitch -= dy * 0.005;
    if (p->pitch > 1.45) p->pitch = 1.45;
    if (p->pitch < -1.45) p->pitch = -1.45;
    preview_redraw(p, viewport);
}

void fl_scene_preview_fly(Fl_ScenePreview *p, Ca_Viewport *viewport, double forward, double right, double up)
{
    if (!p || p->shot || p->flat || !isfinite(forward) || !isfinite(right) || !isfinite(up)) return;
    PreviewView view; float h = 0;
    if (!editor_step(p, viewport, &view, &h)) return;
    double step = view.subject_distance * 0.02;
    p->eye_x += (view.look.x * forward + view.right.x * right + view.up.x * up) * step;
    p->eye_y += (view.look.y * forward + view.right.y * right + view.up.y * up) * step;
    p->eye_z += (view.look.z * forward + view.right.z * right + view.up.z * up) * step;
    preview_redraw(p, viewport);
}

void fl_scene_preview_zoom(Fl_ScenePreview *p, Ca_Viewport *viewport, double dy)
{
    if (!p || !viewport || !isfinite(dy) || dy == 0) return;
    PreviewView view; float h = 0;
    if (!editor_step(p, viewport, &view, &h)) return;
    double step = view.subject_distance * dy * 0.002;
    p->eye_x += view.look.x * step; p->eye_y += view.look.y * step; p->eye_z += view.look.z * step;
    preview_redraw(p, viewport);
}

void fl_scene_preview_reset_view(Fl_ScenePreview *p)
{
    if (!p) return;
    p->frame_pending = true;
    preview_redraw(p, NULL);
}

static Fl_Mat4 widget_face(const Fl_ResolvedEntity *v, double *width, double *height)
{
    if (v->mesh) { *width = 1; *height = 1; return fl_mat4_mul(v->model, fl_mat4_translation(-0.5, -0.5, 0)); }
    *width = v->bounds.width; *height = v->bounds.height;
    return v->model;
}

static bool handle_world(const Fl_ResolvedEntity *v, unsigned handle, double pixels_per_world_unit, Fl_Vec3 *out)
{
    double width, height;
    Fl_Mat4 face = widget_face(v, &width, &height);
    if (handle < 4) {
        const double corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        *out = fl_mat4_point(face, (Fl_Vec3){corners[handle][0] * width, corners[handle][1] * height, 0});
        return true;
    }
    Fl_Vec3 mid = fl_mat4_point(face, (Fl_Vec3){width * 0.5, 0, 0});
    Fl_Vec3 axis = fl_mat4_vector(face, (Fl_Vec3){0, -1, 0});
    double length = hypot(hypot(axis.x, axis.y), axis.z);
    if (length < 1e-12 || !(pixels_per_world_unit > 0)) return false;
    double offset = FL_STYLE_ROTATE_OFFSET / pixels_per_world_unit;
    *out = (Fl_Vec3){mid.x + axis.x / length * offset, mid.y + axis.y / length * offset, mid.z + axis.z / length * offset};
    return true;
}

static Fl_EditGesture gizmo_pick(Fl_ScenePreview *p, Ca_Viewport *viewport, double x, double y);

Fl_EditGesture fl_scene_preview_handle(Fl_ScenePreview *p, Ca_Viewport *viewport, double x, double y)
{
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    const Fl_SceneGeometry *g = fl_scene_edit_geometry(p->edit);
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    if (!g || w <= 0 || h <= 0) return FL_GESTURE_MOVE;
    PreviewView view; Fl_SceneFrame gate = {0};
    if (!preview_view(p, scene, state, w, h, &gate, &view) || !view.perspective || p->shot) return FL_GESTURE_MOVE;
    if (p->tool != FL_TOOL_RECT) return gizmo_pick(p, viewport, x, y);
    float box_x, box_y, box_w, box_h;
    letterbox_rect(vx, vy, w, h, p->shot ? &gate : NULL, &box_x, &box_y, &box_w, &box_h);
    (void)box_x; (void)box_y; (void)box_w;
    const Fl_Entity *e = fl_scene_entity(scene, fl_scene_edit_focus(p->edit));
    if (!e || fl_scene_edit_count(p->edit) != 1 || fl_entity_component(e, FL_COMPONENT_CAMERA)) return FL_GESTURE_MOVE;
    const Fl_ResolvedEntity *entity = &g->entities[e - scene->entities];
    if (!entity->visible || !entity->invertible) return FL_GESTURE_MOVE;
    for (unsigned i = 0; i < 5; ++i) {
        Fl_Vec3 probe = {0, 0, 0};
        if (i == 4 && !handle_world(entity, 4, 1, &probe)) continue;
        if (i < 4 && !handle_world(entity, i, 1, &probe)) continue;
        double ppw = pixels_per_world(&view, box_h, probe);
        Fl_Vec3 world;
        if (i == 4) { if (!handle_world(entity, 4, ppw, &world)) continue; }
        else world = probe;
        double hx, hy;
        if (project_window(&view, viewport, p->shot, &gate, world, &hx, &hy) && hypot(x - hx, y - hy) <= FL_STYLE_HANDLE_HIT)
            return (Fl_EditGesture)(FL_GESTURE_NW + i);
    }
    return FL_GESTURE_MOVE;
}

static Fl_Vec3 vec_cross(Fl_Vec3 a, Fl_Vec3 b)
{
    return (Fl_Vec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static bool vec_unit(Fl_Vec3 v, Fl_Vec3 *out)
{
    double n = hypot(hypot(v.x, v.y), v.z);
    if (!(n > 1e-8)) return false;
    *out = (Fl_Vec3){v.x / n, v.y / n, v.z / n};
    return true;
}

static double vec_dot(Fl_Vec3 a, Fl_Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static bool gizmo_basis(Fl_Vec3 axis, Fl_Vec3 *u, Fl_Vec3 *v)
{
    Fl_Vec3 ref = fabs(axis.y) < 0.9 ? (Fl_Vec3){0, 1, 0} : (Fl_Vec3){1, 0, 0};
    if (!vec_unit(vec_cross(axis, ref), u)) return false;
    return vec_unit(vec_cross(axis, *u), v);
}

static bool facing_normal(Fl_Vec3 axis, Fl_Vec3 look, Fl_Vec3 *normal)
{
    double along = vec_dot(axis, look);
    if (vec_unit((Fl_Vec3){look.x - axis.x * along, look.y - axis.y * along, look.z - axis.z * along}, normal)) return true;
    Fl_Vec3 u, v;
    if (!gizmo_basis(axis, &u, &v)) return false;
    *normal = u;
    return true;
}

static bool ray_plane(Fl_Vec3 origin, Fl_Vec3 direction, Fl_Vec3 point, Fl_Vec3 normal, Fl_Vec3 *hit)
{
    double denom = vec_dot(direction, normal);
    if (fabs(denom) < 1e-8) return false;
    double t = vec_dot((Fl_Vec3){point.x - origin.x, point.y - origin.y, point.z - origin.z}, normal) / denom;
    *hit = (Fl_Vec3){origin.x + direction.x * t, origin.y + direction.y * t, origin.z + direction.z * t};
    return isfinite(hit->x) && isfinite(hit->y) && isfinite(hit->z);
}

static double wrap_pi(double angle)
{
    const double pi = 3.14159265358979323846;
    while (angle > pi) angle -= 2 * pi;
    while (angle < -pi) angle += 2 * pi;
    return angle;
}

static int gesture_axis(Fl_EditGesture gesture)
{
    if (gesture >= FL_GESTURE_AXIS_X && gesture <= FL_GESTURE_AXIS_Z) return (int)(gesture - FL_GESTURE_AXIS_X);
    if (gesture >= FL_GESTURE_RING_X && gesture <= FL_GESTURE_RING_Z) return (int)(gesture - FL_GESTURE_RING_X);
    if (gesture >= FL_GESTURE_SCALE_X && gesture <= FL_GESTURE_SCALE_Z) return (int)(gesture - FL_GESTURE_SCALE_X);
    return -1;
}

/* How directly an axis points at the camera. 2D hides those shafts and keeps the facing ring. */
static double axis_alignment(Fl_Vec3 axis, Fl_Vec3 look)
{
    double n = hypot(hypot(axis.x, axis.y), axis.z);
    if (n < 1e-8) return 1;
    return fabs(vec_dot(axis, look)) / n;
}

/* Local axes of the focused selection, and the point the gizmo spins around. */
static bool gizmo_frame(Fl_ScenePreview *p, Fl_Vec3 *origin, Fl_Vec3 axes[3])
{
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    const Fl_SceneGeometry *g = fl_scene_edit_geometry(p->edit);
    if (!scene || !g || g->count != scene->entity_count) return false;
    Fl_Id id = fl_scene_edit_focus(p->edit);
    if (!id || !fl_scene_edit_contains(p->edit, id)) id = fl_scene_edit_primary(p->edit);
    const Fl_Entity *entity = fl_scene_entity(scene, id);
    if (!entity) return false;
    const Fl_ResolvedEntity *v = &g->entities[entity - scene->entities];
    bool camera = fl_entity_component(entity, FL_COMPONENT_CAMERA) != NULL;
    Fl_Vec3 local = v->mesh || camera ? (Fl_Vec3){0, 0, 0} : (Fl_Vec3){v->bounds.width * 0.5, v->bounds.height * 0.5, 0};
    *origin = fl_mat4_point(v->model, local);
    Fl_Vec3 raw[3] = {{v->model.m[0], v->model.m[1], v->model.m[2]}, {v->model.m[4], v->model.m[5], v->model.m[6]}, {v->model.m[8], v->model.m[9], v->model.m[10]}};
    for (int i = 0; i < 3; ++i) if (!vec_unit(raw[i], &axes[i])) return false;
    return isfinite(origin->x) && isfinite(origin->y) && isfinite(origin->z);
}

static bool pointer_ray(Fl_ScenePreview *p, Ca_Viewport *viewport, const PreviewView *view, double x, double y, Fl_Vec3 *origin, Fl_Vec3 *direction)
{
    double ndc_x, ndc_y;
    return picture_ndc(p, viewport, x, y, true, &ndc_x, &ndc_y) && view_ray(view, ndc_x, ndc_y, origin, direction);
}

static double segment_px(double px, double py, double ax, double ay, double bx, double by)
{
    double abx = bx - ax, aby = by - ay, ab2 = abx * abx + aby * aby;
    double t = ab2 > 0 ? ((px - ax) * abx + (py - ay) * aby) / ab2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return hypot(px - (ax + abx * t), py - (ay + aby * t));
}

static Fl_EditGesture gizmo_pick(Fl_ScenePreview *p, Ca_Viewport *viewport, double x, double y)
{
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    (void)vx; (void)vy;
    PreviewView view; Fl_SceneFrame gate = {0};
    Fl_Vec3 origin, axes[3];
    if (w <= 0 || h <= 0 || !preview_view(p, scene, state, w, h, &gate, &view) || !gizmo_frame(p, &origin, axes)) return FL_GESTURE_MOVE;
    double ppw = pixels_per_world(&view, h, origin);
    if (!(ppw > 0)) return FL_GESTURE_MOVE;
    double reach = 78.0 / ppw, ring = 52.0 / ppw, ox, oy;
    if (!project_window(&view, viewport, false, &gate, origin, &ox, &oy)) return FL_GESTURE_MOVE;
    double best = 10;
    Fl_EditGesture hit = FL_GESTURE_MOVE;
    bool shafts = p->tool == FL_TOOL_MOVE || p->tool == FL_TOOL_SCALE;
    bool rings = p->tool == FL_TOOL_ROTATE;
    if (shafts) for (int i = 0; i < 3; ++i) {
        if (p->flat && axis_alignment(axes[i], view.look) > 0.65) continue;
        Fl_Vec3 tip = {origin.x + axes[i].x * reach, origin.y + axes[i].y * reach, origin.z + axes[i].z * reach};
        double tx, ty;
        if (!project_window(&view, viewport, false, &gate, tip, &tx, &ty)) continue;
        double distance = segment_px(x, y, ox, oy, tx, ty);
        if (distance < best) { best = distance; hit = (Fl_EditGesture)((p->tool == FL_TOOL_SCALE ? FL_GESTURE_SCALE_X : FL_GESTURE_AXIS_X) + i); }
    }
    if (p->tool == FL_TOOL_MOVE && hit == FL_GESTURE_MOVE && hypot(x - ox, y - oy) <= 12) return FL_GESTURE_PLANE;
    if (!rings || hit != FL_GESTURE_MOVE || fl_scene_edit_count(p->edit) != 1) return hit;
    Fl_Vec3 ray_o, ray_d;
    if (!pointer_ray(p, viewport, &view, x, y, &ray_o, &ray_d)) return hit;
    best = 8;
    for (int i = 0; i < 3; ++i) {
        if (p->flat && axis_alignment(axes[i], view.look) < 0.65) continue;
        Fl_Vec3 plane_hit;
        if (!ray_plane(ray_o, ray_d, origin, axes[i], &plane_hit)) continue;
        Fl_Vec3 offset = {plane_hit.x - origin.x, plane_hit.y - origin.y, plane_hit.z - origin.z};
        double radial = hypot(hypot(offset.x, offset.y), offset.z);
        if (fabs(radial - ring) * ppw < best && radial * ppw > 16) { best = fabs(radial - ring) * ppw; hit = (Fl_EditGesture)(FL_GESTURE_RING_X + i); }
    }
    return hit;
}

bool fl_scene_preview_gizmo_arm(Fl_ScenePreview *p, Ca_Viewport *viewport, Fl_EditGesture gesture, double x, double y)
{
    int index = gesture_axis(gesture);
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = p ? preview_scene(p, &state) : NULL;
    float vx, vy, w, h;
    if (!p || !viewport || p->shot || index < 0) return false;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    (void)vx; (void)vy;
    PreviewView view; Fl_SceneFrame gate = {0};
    Fl_Vec3 origin, axes[3], ray_o, ray_d, hit;
    if (w <= 0 || h <= 0 || !preview_view(p, scene, state, w, h, &gate, &view) || !gizmo_frame(p, &origin, axes)) return false;
    bool ring = gesture >= FL_GESTURE_RING_X && gesture <= FL_GESTURE_RING_Z;
    p->gizmo_origin = origin;
    p->gizmo_axis = axes[index];
    p->gizmo_gesture = gesture;
    if (ring) {
        p->gizmo_normal = axes[index];
        if (!gizmo_basis(axes[index], &p->gizmo_u, &p->gizmo_v)) return false;
    } else if (!facing_normal(axes[index], view.look, &p->gizmo_normal)) return false;
    if (!pointer_ray(p, viewport, &view, x, y, &ray_o, &ray_d) || !ray_plane(ray_o, ray_d, origin, p->gizmo_normal, &hit)) return false;
    p->gizmo_hit = hit;
    p->gizmo_armed = true;
    return true;
}

bool fl_scene_preview_gizmo_delta(Fl_ScenePreview *p, Ca_Viewport *viewport, double x, double y, Fl_Vec3 *world_delta, Fl_Vec3 *world_axis, double *radians)
{
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = p ? preview_scene(p, &state) : NULL;
    float vx, vy, w, h;
    if (!p || !p->gizmo_armed || !viewport || !world_delta || !world_axis || !radians) return false;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    (void)vx; (void)vy;
    PreviewView view; Fl_SceneFrame gate = {0};
    Fl_Vec3 ray_o, ray_d, hit;
    if (w <= 0 || h <= 0 || !preview_view(p, scene, state, w, h, &gate, &view)) return false;
    if (!pointer_ray(p, viewport, &view, x, y, &ray_o, &ray_d) || !ray_plane(ray_o, ray_d, p->gizmo_origin, p->gizmo_normal, &hit)) return false;
    *world_axis = p->gizmo_axis;
    if (p->gizmo_gesture >= FL_GESTURE_SCALE_X && p->gizmo_gesture <= FL_GESTURE_SCALE_Z) {
        Fl_Vec3 from_start = {p->gizmo_hit.x - p->gizmo_origin.x, p->gizmo_hit.y - p->gizmo_origin.y, p->gizmo_hit.z - p->gizmo_origin.z};
        Fl_Vec3 from_now = {hit.x - p->gizmo_origin.x, hit.y - p->gizmo_origin.y, hit.z - p->gizmo_origin.z};
        double start = vec_dot(from_start, p->gizmo_axis), now = vec_dot(from_now, p->gizmo_axis);
        *world_delta = (Fl_Vec3){0, 0, 0};
        if (fabs(start) < 1e-4) return false;
        *radians = now / start;
        return isfinite(*radians);
    }
    if (p->gizmo_gesture >= FL_GESTURE_RING_X && p->gizmo_gesture <= FL_GESTURE_RING_Z) {
        *world_delta = (Fl_Vec3){0, 0, 0};
        double start = atan2(vec_dot(p->gizmo_hit, p->gizmo_v), vec_dot(p->gizmo_hit, p->gizmo_u));
        double now = atan2(vec_dot(hit, p->gizmo_v), vec_dot(hit, p->gizmo_u));
        *radians = wrap_pi(now - start);
        return isfinite(*radians);
    }
    double distance = vec_dot((Fl_Vec3){hit.x - p->gizmo_hit.x, hit.y - p->gizmo_hit.y, hit.z - p->gizmo_hit.z}, p->gizmo_axis);
    *world_delta = (Fl_Vec3){p->gizmo_axis.x * distance, p->gizmo_axis.y * distance, p->gizmo_axis.z * distance};
    *radians = 0;
    return isfinite(distance);
}

/* Screen-facing ribbon from a to b, about 1.5 logical pixels wide at the nearer end. */
static void draw_frustum_edge(Fl_SceneRenderer *renderer, VkCommandBuffer cmd, const Fl_SceneFrame *gate, const PreviewView *view,
                              float viewport_h, VkDescriptorSet white, Fl_Vec3 a, Fl_Vec3 b, const unsigned char color[4])
{
    Fl_Vec3 dir = {b.x - a.x, b.y - a.y, b.z - a.z};
    if (!(hypot(hypot(dir.x, dir.y), dir.z) > 1e-6)) return;
    double ppw = fmax(pixels_per_world(view, viewport_h, a), pixels_per_world(view, viewport_h, b));
    if (!(ppw > 0)) return;
    Fl_Vec3 side = vec_cross(dir, view->look);
    if (!vec_unit(side, &side) && !vec_unit(vec_cross(dir, view->up), &side)) return;
    double thick = 1.5 / ppw;
    side.x *= thick; side.y *= thick; side.z *= thick;
    Fl_Vec3 normal = vec_cross(dir, side);
    Fl_Mat4 m = fl_mat4_identity();
    m.m[0] = dir.x; m.m[1] = dir.y; m.m[2] = dir.z;
    m.m[4] = side.x; m.m[5] = side.y; m.m[6] = side.z;
    m.m[8] = normal.x; m.m[9] = normal.y; m.m[10] = normal.z;
    m.m[12] = a.x - side.x * 0.5; m.m[13] = a.y - side.y * 0.5; m.m[14] = a.z - side.z * 0.5;
    Fl_Appearance stroke = {.stroke = {color[0], color[1], color[2], color[3]}, .stroke_width = 1};
    fl_scene_renderer_model(renderer, cmd, gate, m, 1, 1, FL_WIDGET_LINE, stroke, white, NULL);
}

static Fl_Vec3 frustum_corner(Fl_Vec3 origin, Fl_Vec3 right, Fl_Vec3 up, Fl_Vec3 look, double distance, double x, double y)
{
    return (Fl_Vec3){origin.x + look.x * distance + right.x * x + up.x * y,
                     origin.y + look.y * distance + right.y * x + up.y * y,
                     origin.z + look.z * distance + right.z * x + up.z * y};
}

/* View pyramid of one selected camera. The far face is the picture, placed along the camera look. */
static void draw_camera_frustum(Fl_SceneRenderer *renderer, VkCommandBuffer cmd, const Fl_Scene *scene, const Fl_SceneFrame *gate,
                                const PreviewView *view, float viewport_h, VkDescriptorSet white, Fl_Mat4 model, const Fl_Camera *camera)
{
    Fl_Vec3 right, up, look, origin;
    if (!vec_unit((Fl_Vec3){model.m[0], model.m[1], model.m[2]}, &right)) return;
    if (!vec_unit((Fl_Vec3){model.m[4], model.m[5], model.m[6]}, &up)) return;
    if (!vec_unit((Fl_Vec3){-model.m[8], -model.m[9], -model.m[10]}, &look)) return;
    origin = (Fl_Vec3){model.m[12], model.m[13], model.m[14]};
    Fl_SceneFrame picture = camera_picture(scene, camera);
    if (!(picture.width > 0) || !(picture.height > 0)) return;
    double aspect = picture.width / picture.height;
    double near_z = camera->near_z > 1e-4 ? camera->near_z : 0.1;
    double far_z = camera->far_z;
    if (!(far_z > near_z) || !isfinite(far_z)) return;
    double near_w, near_h, far_w, far_h, far_draw = far_z;
    if (camera->perspective) {
        if (!(camera->fov_y > 0.01) || camera->fov_y >= 3.1) return;
        double t = tan(camera->fov_y * 0.5);
        double fit = picture.height * 0.5 / t;
        if (fit > near_z && fit < far_z) far_draw = fit;
        near_h = near_z * t; far_h = far_draw * t;
        near_w = near_h * aspect; far_w = far_h * aspect;
    } else {
        double height = camera->ortho_height > 0 ? camera->ortho_height : picture.height;
        if (!(height > 0)) return;
        if (picture.height > near_z && picture.height < far_z) far_draw = picture.height;
        near_h = far_h = height * 0.5;
        near_w = far_w = near_h * aspect;
    }
    if (!isfinite(near_w) || !isfinite(far_w)) return;
    Fl_Vec3 near_mid = frustum_corner(origin, right, up, look, near_z, 0, 0);
    double near_px = pixels_per_world(view, viewport_h, near_mid) * near_h * 2;
    bool draw_near = near_px >= 8;
    const double sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    Fl_Vec3 near_corner[4], far_corner[4];
    for (int i = 0; i < 4; ++i) {
        near_corner[i] = frustum_corner(origin, right, up, look, near_z, sx[i] * near_w, sy[i] * near_h);
        far_corner[i] = frustum_corner(origin, right, up, look, far_draw, sx[i] * far_w, sy[i] * far_h);
    }
    for (int i = 0; i < 4; ++i) {
        const unsigned char blue[4] = {120, 196, 255, 230};
        if (draw_near) draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, near_corner[i], near_corner[(i + 1) % 4], blue);
        draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, far_corner[i], far_corner[(i + 1) % 4], blue);
        draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, draw_near ? near_corner[i] : origin, far_corner[i], blue);
    }
}

static Fl_Mat4 billboard(Fl_Vec3 right, Fl_Vec3 up, Fl_Vec3 point, double size)
{
    Fl_Mat4 m = fl_mat4_identity();
    m.m[0] = right.x * size; m.m[1] = right.y * size; m.m[2] = right.z * size;
    m.m[4] = up.x * size; m.m[5] = up.y * size; m.m[6] = up.z * size;
    m.m[12] = point.x - (right.x + up.x) * size * 0.5;
    m.m[13] = point.y - (right.y + up.y) * size * 0.5;
    m.m[14] = point.z - (right.z + up.z) * size * 0.5;
    return m;
}

static void draw_gizmo(Fl_ScenePreview *p, Fl_SceneRenderer *renderer, VkCommandBuffer cmd, const Fl_SceneFrame *gate, const PreviewView *view, float viewport_h, VkDescriptorSet white)
{
    Fl_Vec3 origin, axes[3];
    if (!gizmo_frame(p, &origin, axes) || p->tool == FL_TOOL_RECT) return;
    double ppw = pixels_per_world(view, viewport_h, origin);
    if (!(ppw > 0)) return;
    double reach = 78.0 / ppw, ring = 52.0 / ppw;
    static const unsigned char colors[3][4] = {{232, 84, 84, 255}, {88, 196, 120, 255}, {84, 156, 240, 255}};
    if (p->tool == FL_TOOL_ROTATE && fl_scene_edit_count(p->edit) == 1) for (int i = 0; i < 3; ++i) {
        if (p->flat && axis_alignment(axes[i], view->look) < 0.65) continue;
        Fl_Vec3 u, v, prev;
        if (!gizmo_basis(axes[i], &u, &v)) continue;
        for (int step = 0; step <= 28; ++step) {
            double angle = step / 28.0 * 6.283185307179586;
            Fl_Vec3 point = {origin.x + (u.x * cos(angle) + v.x * sin(angle)) * ring,
                             origin.y + (u.y * cos(angle) + v.y * sin(angle)) * ring,
                             origin.z + (u.z * cos(angle) + v.z * sin(angle)) * ring};
            if (step) draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, prev, point, colors[i]);
            prev = point;
        }
    }
    if (p->tool == FL_TOOL_MOVE || p->tool == FL_TOOL_SCALE) for (int i = 0; i < 3; ++i) {
        if (p->flat && axis_alignment(axes[i], view->look) > 0.65) continue;
        Fl_Vec3 tip = {origin.x + axes[i].x * reach, origin.y + axes[i].y * reach, origin.z + axes[i].z * reach};
        Fl_Vec3 side = vec_cross(axes[i], view->look);
        if (!vec_unit(side, &side)) side = vec_cross(axes[i], view->up);
        if (!vec_unit(side, &side)) continue;
        draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, origin, tip, colors[i]);
        if (p->tool == FL_TOOL_MOVE) {
            Fl_Vec3 neck = {origin.x + axes[i].x * reach * 0.78, origin.y + axes[i].y * reach * 0.78, origin.z + axes[i].z * reach * 0.78};
            Fl_Vec3 wing = {side.x * reach * 0.09, side.y * reach * 0.09, side.z * reach * 0.09};
            Fl_Vec3 left = {neck.x + wing.x, neck.y + wing.y, neck.z + wing.z};
            Fl_Vec3 right = {neck.x - wing.x, neck.y - wing.y, neck.z - wing.z};
            draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, left, tip, colors[i]);
            draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, right, tip, colors[i]);
        } else {
            Fl_Vec3 across = vec_cross(axes[i], side);
            if (!vec_unit(across, &across)) continue;
            double s = reach * 0.07;
            Fl_Vec3 corner[4];
            for (int c = 0; c < 4; ++c) {
                double su = (c == 0 || c == 3) ? -s : s, sv = (c < 2) ? -s : s;
                corner[c] = (Fl_Vec3){tip.x + side.x * su + across.x * sv, tip.y + side.y * su + across.y * sv, tip.z + side.z * su + across.z * sv};
            }
            for (int c = 0; c < 4; ++c) draw_frustum_edge(renderer, cmd, gate, view, viewport_h, white, corner[c], corner[(c + 1) % 4], colors[i]);
        }
    }
    if (p->tool == FL_TOOL_MOVE) {
        const unsigned char center[4] = {240, 214, 96, 255};
        Fl_Appearance mark = {.fill = {center[0], center[1], center[2], center[3]}};
        fl_scene_renderer_model(renderer, cmd, gate, billboard(view->right, view->up, origin, 14.0 / ppw), 1, 1, FL_WIDGET_RECTANGLE, mark, white, NULL);
    }
}

void fl_scene_preview_render(Ca_Viewport *viewport, void *data)
{
    Fl_ScenePreview *p = data;
    Fl_Project *project = fl_scene_edit_project(p->edit);
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    const Fl_SceneGeometry *g = fl_scene_edit_geometry(p->edit);
    if (!p->renderer) p->renderer = fl_scene_renderer_create(&(Fl_SceneRendererDesc){.gpu = fl_render_host_context(ca_viewport_instance(viewport)), .fonts = p->fonts,
        .jobs = p->jobs, .bus = p->bus, .invalidate = p->invalidate, .invalidate_data = p->invalidate_data});
    if (p->renderer && scene) fl_scene_renderer_prepare(p->renderer, project, scene, state, g, 1, false);
    uint32_t w = ca_viewport_width(viewport), h = ca_viewport_height(viewport);
    VkCommandBuffer cmd = ca_viewport_cmd(viewport);
    VkImageView depth = p->renderer ? fl_scene_renderer_depth(p->renderer, cmd, w, h) : VK_NULL_HANDLE;
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ca_viewport_image_view(viewport), .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color = {.float32 = FL_STYLE_VIEW_CLEAR}};
    VkRenderingAttachmentInfo depth_info = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.depthStencil.depth = 1};
    VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {w, h}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &attachment, .pDepthAttachment = depth ? &depth_info : NULL};
    if (p->renderer) fl_scene_renderer_sync_uniforms(p->renderer, cmd);
    vkCmdBeginRendering(cmd, &info);
    float logical_x, logical_y, logical_w, logical_h;
    ca_viewport_screen_rect(viewport, &logical_x, &logical_y, &logical_w, &logical_h);
    Fl_SceneFrame gate = {0};
    PreviewView view;
    bool viewed = p->renderer && scene && logical_w > 0 && logical_h > 0 && preview_view(p, scene, state, logical_w, logical_h, &gate, &view);
    if (viewed) fl_scene_renderer_set_view(p->renderer, view.view, view.right, view.up, view.bloom);
    if (p->shot && gate.width > 0 && gate.height > 0 && w > 0 && h > 0) {
        double picture = gate.width / gate.height, panel = (double)w / (double)h;
        float box_w, box_h, box_x, box_y;
        if (panel > picture) { box_h = (float)h; box_w = (float)(h * picture); box_x = ((float)w - box_w) * 0.5f; box_y = 0; }
        else { box_w = (float)w; box_h = (float)(w / picture); box_y = ((float)h - box_h) * 0.5f; box_x = 0; }
        fl_scene_renderer_set_viewport(p->renderer, box_x, box_y, box_w, box_h);
    } else if (p->renderer) fl_scene_renderer_set_viewport(p->renderer, 0, 0, 0, 0);
    if (g && viewed && depth && fl_scene_renderer_begin(p->renderer, cmd, ca_viewport_format(viewport), w, h, true)) {
        fl_scene_renderer_background(p->renderer, cmd, &gate, &gate, scene->background);
        if (!p->shot && view.perspective) {
            const unsigned char minor[4] = FL_STYLE_CANVAS_GRID_RGBA, major[4] = FL_STYLE_CANVAS_GRID_MAJOR_RGBA;
            double extent = fmax(scene->width, scene->height) * 8;
            if (extent < 4096) extent = 4096;
            fl_scene_renderer_grid_plane(p->renderer, cmd, scene->width * 0.5 - extent * 0.5, scene->height * 0.5 - extent * 0.5, extent, FL_SCENE_GRID_SPACING, minor, major);
        }
        Fl_RenderTarget surface = {.view = ca_viewport_image_view(viewport), .format = ca_viewport_format(viewport), .width = w, .height = h, .slot = ca_viewport_frame_index(viewport), .image = ca_viewport_image(viewport), .depth = depth};
        fl_scene_renderer_entities(p->renderer, cmd, &surface, &gate, scene, state, g);
        VkDescriptorSet white = fl_scene_renderer_white(p->renderer);
        if (!p->shot) for (size_t i = 0; i < g->count; ++i) {
            if (!fl_scene_edit_contains(p->edit, scene->entities[i].id)) continue;
            Fl_Pose pose = fl_pose_identity(); Fl_Camera camera;
            if (!fl_scene_camera_entity(scene, state, scene->entities[i].id, &pose, &camera, NULL)) continue;
            draw_camera_frustum(p->renderer, cmd, scene, &gate, &view, logical_h, white, g->entities[i].model, &camera);
        }
        if (view.perspective) {
            float box_x, box_y, box_w, box_h;
            letterbox_rect(0, 0, logical_w, logical_h, p->shot ? &gate : NULL, &box_x, &box_y, &box_w, &box_h);
            (void)box_x; (void)box_y; (void)box_w;
            Fl_Appearance outline = {.stroke = FL_STYLE_GIZMO_RGBA, .font_size = 48};
            for (size_t i = 0; i < g->count; ++i) {
                const Fl_ResolvedEntity *v = &g->entities[i];
                if (!v->visible || !fl_scene_edit_contains(p->edit, scene->entities[i].id) || fl_entity_component(&scene->entities[i], FL_COMPONENT_CAMERA)) continue;
                double gw, gh;
                Fl_Mat4 face = widget_face(v, &gw, &gh);
                double axis = hypot(hypot(face.m[0], face.m[1]), face.m[2]);
                Fl_Vec3 center = fl_mat4_point(face, (Fl_Vec3){gw * 0.5, gh * 0.5, 0});
                double edge = pixels_per_world(&view, box_h, center);
                outline.stroke_width = edge > 0 && axis > 1e-9 ? FL_STYLE_OUTLINE_WIDTH / (edge * axis) : 0;
                fl_scene_renderer_model(p->renderer, cmd, &gate, face, gw, gh, FL_WIDGET_RECTANGLE, outline, white, NULL);
            }
            if (!p->shot && p->tool == FL_TOOL_RECT) {
            Fl_Appearance handle = {.fill = FL_STYLE_GIZMO_RGBA, .font_size = 48};
            for (unsigned i = 0; i < 5; ++i) {
                const Fl_Entity *e = fl_scene_entity(scene, fl_scene_edit_focus(p->edit));
                if (!e || fl_scene_edit_count(p->edit) != 1 || fl_entity_component(e, FL_COMPONENT_CAMERA)) break;
                const Fl_ResolvedEntity *entity = &g->entities[e - scene->entities];
                Fl_Vec3 probe;
                if (!handle_world(entity, i < 4 ? i : 4, 1, &probe)) break;
                double handle_scale = pixels_per_world(&view, box_h, probe);
                Fl_Vec3 world;
                if (!handle_world(entity, i, handle_scale, &world) || !(handle_scale > 0)) break;
                fl_scene_renderer_model(p->renderer, cmd, &gate, billboard(view.right, view.up, world, FL_STYLE_HANDLE_SIZE / handle_scale), 1, 1, FL_WIDGET_RECTANGLE, handle, white, NULL);
            }
            } else if (!p->shot) draw_gizmo(p, p->renderer, cmd, &gate, &view, logical_h, white);
        }
    }
    vkCmdEndRendering(cmd);
}

Fl_ScenePreview *fl_scene_preview_create(const Fl_ScenePreviewDesc *desc)
{
    if (!desc || !desc->edit || !desc->fonts || !desc->jobs || !desc->bus) return NULL;
    Fl_ScenePreview *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->edit = desc->edit; p->fonts = desc->fonts; p->jobs = desc->jobs; p->bus = desc->bus;
    p->invalidate = desc->invalidate; p->invalidate_data = desc->invalidate_data;
    p->frame_pending = true;
    return p;
}
void fl_scene_preview_destroy(Fl_ScenePreview *p)
{
    if (!p) return;
    fl_scene_renderer_destroy(p->renderer);
    free(p);
}
