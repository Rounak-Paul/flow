#include "fl_render_host.h"
#include "fl_scene_preview.h"
#include "render/gpu/fl_scene_renderer.h"
#include "fl_canvas_view.h"
#include "doc/fl_scene_frame.h"
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
    double nav_x, nav_y, nav_scale; /* viewport truck and dolly; identity leaves the authored camera untouched */
};

typedef struct PreviewView {
    Fl_Mat4 view;
    Fl_Vec3 right, up, look, position;
    double bloom, fov_y, subject_distance;
    bool perspective;
} PreviewView;

/* Resolve the saved camera into a viewport-centered output frame. */
static Fl_CanvasMapping preview_mapping(const Fl_Scene *scene, double w, double h)
{
    return fl_canvas_mapping(fl_scene_frame(scene), w, h, FL_STYLE_ARTBOARD_MARGIN);
}

static const Fl_Scene *preview_scene(Fl_ScenePreview *p, const Fl_SceneState **state_out)
{
    const Fl_Scene *scene = fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
    const Fl_SceneState *state = fl_scene_edit_state(p->edit);
    scene = fl_scene_state_scene(state, scene);
    if (state && scene && (state->scene != scene->id || state->count != scene->entity_count)) state = NULL;
    if (state_out) *state_out = state;
    return scene;
}

/* Authored camera, plus viewport truck and dolly. Navigation is not written into the document. */
static bool preview_view(Fl_ScenePreview *p, const Fl_Scene *scene, const Fl_SceneState *state, const Fl_SceneFrame *frame, PreviewView *out)
{
    memset(out, 0, sizeof(*out));
    out->right = (Fl_Vec3){1, 0, 0}; out->up = (Fl_Vec3){0, 1, 0}; out->look = (Fl_Vec3){0, 0, 1};
    fl_scene_camera(scene, state, frame->x, frame->y, frame->width, frame->height, &out->view, &out->right, &out->up, &out->bloom);
    Fl_Pose pose = fl_pose_identity(); Fl_Camera camera = fl_camera_default();
    if (!scene || !fl_scene_view_camera(scene, state, &pose, &camera, NULL) || !camera.perspective) return true;
    Fl_Mat4 axes = fl_pose_model(&pose);
    out->right = (Fl_Vec3){axes.m[0], axes.m[1], axes.m[2]};
    out->up = (Fl_Vec3){axes.m[4], axes.m[5], axes.m[6]};
    out->look = (Fl_Vec3){-axes.m[8], -axes.m[9], -axes.m[10]};
    Fl_Vec3 focus = {scene->width * 0.5, scene->height * 0.5, 0};
    double scale = p->nav_scale > 0 ? p->nav_scale : 1;
    double ox = (pose.x - focus.x) * scale, oy = (pose.y - focus.y) * scale, oz = (pose.z - focus.z) * scale;
    out->subject_distance = sqrt(ox * ox + oy * oy + oz * oz);
    if (out->subject_distance < 1) out->subject_distance = 1;
    pose.x = focus.x + ox + out->right.x * p->nav_x + out->up.x * p->nav_y;
    pose.y = focus.y + oy + out->right.y * p->nav_x + out->up.y * p->nav_y;
    pose.z = focus.z + oz + out->right.z * p->nav_x + out->up.z * p->nav_y;
    out->position = (Fl_Vec3){pose.x, pose.y, pose.z};
    out->fov_y = camera.fov_y;
    out->view = fl_camera_view_proj(&pose, &camera, frame->x, frame->y, frame->width, frame->height);
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

static bool project_window(const PreviewView *view, Ca_Viewport *viewport, Fl_Vec3 world, double *sx, double *sy)
{
    double x = view->view.m[0] * world.x + view->view.m[4] * world.y + view->view.m[8] * world.z + view->view.m[12];
    double y = view->view.m[1] * world.x + view->view.m[5] * world.y + view->view.m[9] * world.z + view->view.m[13];
    double w = view->view.m[3] * world.x + view->view.m[7] * world.y + view->view.m[11] * world.z + view->view.m[15];
    float vx, vy, vw, vh;
    ca_viewport_screen_rect(viewport, &vx, &vy, &vw, &vh);
    if (!isfinite(w) || w <= 1e-8 || vw <= 0 || vh <= 0) return false;
    *sx = vx + (x / w + 1) * 0.5 * vw;
    *sy = vy + (y / w + 1) * 0.5 * vh;
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

bool fl_scene_preview_camera_view(Fl_ScenePreview *p)
{
    if (!p) return false;
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    Fl_Pose pose; Fl_Camera camera;
    return scene && fl_scene_view_camera(scene, state, &pose, &camera, NULL) && camera.perspective;
}

bool fl_scene_preview_point(Fl_ScenePreview *p, Ca_Viewport *viewport, double wx, double wy, bool outside, double *sx, double *sy)
{
    if (!p || !viewport || !sx || !sy || !isfinite(wx) || !isfinite(wy)) return false;
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    Fl_CanvasMapping map = preview_mapping(scene, w, h);
    if (map.scale <= 0) return false;
    PreviewView view;
    if (!preview_view(p, scene, state, &map.world, &view)) return false;
    if (!view.perspective) {
        if (!outside && (wx < vx || wy < vy || wx > vx + w || wy > vy + h)) return false;
        *sx = map.world.x + (wx - vx) / map.scale; *sy = map.world.y + (wy - vy) / map.scale;
        return isfinite(*sx) && isfinite(*sy);
    }
    double ndc_x, ndc_y;
    if (!window_ndc(viewport, wx, wy, outside, &ndc_x, &ndc_y)) return false;
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
    Fl_CanvasMapping map = preview_mapping(scene, w, h);
    PreviewView view;
    if (map.scale <= 0 || !preview_view(p, scene, state, &map.world, &view) || !view.perspective) {
        double x, y;
        if (!fl_scene_preview_point(p, viewport, wx, wy, false, &x, &y)) return 0;
        return fl_scene_edit_pick(p->edit, x, y, 5);
    }
    double ndc_x, ndc_y;
    if (!window_ndc(viewport, wx, wy, false, &ndc_x, &ndc_y)) return 0;
    Fl_Vec3 origin, direction;
    if (!view_ray(&view, ndc_x, ndc_y, &origin, &direction)) return 0;
    return fl_scene_edit_pick_ray(p->edit, origin, direction);
}

void fl_scene_preview_pan(Fl_ScenePreview *p, Ca_Viewport *viewport, double dx, double dy)
{
    if (!p || !viewport || !isfinite(dx) || !isfinite(dy) || (dx == 0 && dy == 0)) return;
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    float x, y, w, h;
    ca_viewport_screen_rect(viewport, &x, &y, &w, &h);
    Fl_CanvasMapping map = preview_mapping(scene, w, h);
    PreviewView view;
    if (map.scale <= 0 || !preview_view(p, scene, state, &map.world, &view)) return;
    if (view.perspective) {
        double per = h > 0 ? 2 * view.subject_distance * tan(view.fov_y * 0.5) / h : 0;
        if (!(per > 0) || !isfinite(per)) return;
        p->nav_x -= dx * per; p->nav_y -= dy * per;
        preview_redraw(p, viewport);
        return;
    }
    const Fl_Scene *authored = fl_project_scene(fl_scene_edit_project(p->edit), fl_scene_edit_scene(p->edit));
    if (!authored) return;
    double px = authored->camera_x - dx / map.scale, py = authored->camera_y - dy / map.scale;
    if (!isfinite(px) || !isfinite(py)) return;
    if (!fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_SCENE_CAMERA, .scene = authored->id, .camera_x = px, .camera_y = py}, NULL)) return;
    ca_viewport_request_redraw(viewport);
}

void fl_scene_preview_zoom(Fl_ScenePreview *p, Ca_Viewport *viewport, double dy)
{
    if (!p || !viewport || !isfinite(dy) || dy == 0 || !fl_scene_preview_camera_view(p)) return;
    double scale = (p->nav_scale > 0 ? p->nav_scale : 1) * exp(-dy * 0.008);
    if (scale < 0.08) scale = 0.08;
    if (scale > 12) scale = 12;
    p->nav_scale = scale;
    preview_redraw(p, viewport);
}

void fl_scene_preview_reset_view(Fl_ScenePreview *p)
{
    if (!p) return;
    p->nav_x = p->nav_y = 0; p->nav_scale = 1;
    fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind = FL_OP_SCENE_CAMERA, .scene = fl_scene_edit_scene(p->edit)}, NULL);
    preview_redraw(p, NULL);
}

static Fl_Mat4 widget_face(const Fl_ResolvedEntity *v, double *width, double *height)
{
    if (v->mesh) { *width = 1; *height = 1; return fl_mat4_mul(v->model, fl_mat4_translation(-0.5, -0.5, 0)); }
    *width = v->bounds.width; *height = v->bounds.height;
    return v->model;
}

/* Compute scene coordinates of a selected entity's corner/rotation handle. */
static bool handle_point(Fl_ScenePreview *p, const Fl_Scene *scene, const Fl_SceneGeometry *g, unsigned handle, double pixels_per_scene, double *x, double *y)
{
    const Fl_Entity *e = fl_scene_entity(scene, fl_scene_edit_focus(p->edit));
    if (!e || fl_scene_edit_count(p->edit) != 1) return false;
    const Fl_ResolvedEntity *v = &g->entities[e - scene->entities];
    if (!v->visible || !v->invertible) return false;
    const double corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    if (handle < 4) fl_affine_point(v->world, corners[handle][0] * v->bounds.width, corners[handle][1] * v->bounds.height, x, y);
    else {
        fl_affine_point(v->world, v->bounds.width * .5, 0, x, y);
        double length = hypot(v->world.v[1], v->world.v[4]);
        if (length < 1e-12) return false;
        double offset = FL_STYLE_ROTATE_OFFSET / (pixels_per_scene * length);
        *x -= v->world.v[1] * offset; *y -= v->world.v[4] * offset;
    }
    return true;
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

Fl_EditGesture fl_scene_preview_handle(Fl_ScenePreview *p, Ca_Viewport *viewport, double x, double y)
{
    const Fl_SceneState *state = NULL;
    const Fl_Scene *scene = preview_scene(p, &state);
    const Fl_SceneGeometry *g = fl_scene_edit_geometry(p->edit);
    float vx, vy, w, h;
    ca_viewport_screen_rect(viewport, &vx, &vy, &w, &h);
    Fl_CanvasMapping map = preview_mapping(scene, w, h);
    if (!g || map.scale <= 0) return FL_GESTURE_MOVE;
    PreviewView view;
    if (!preview_view(p, scene, state, &map.world, &view)) return FL_GESTURE_MOVE;
    if (!view.perspective) {
        double sx, sy;
        if (!fl_scene_preview_point(p, viewport, x, y, true, &sx, &sy)) return FL_GESTURE_MOVE;
        for (unsigned i = 0; i < 5; ++i) {
            double hx, hy;
            if (handle_point(p, scene, g, i, map.scale, &hx, &hy) && hypot(sx - hx, sy - hy) <= FL_STYLE_HANDLE_HIT / map.scale)
                return (Fl_EditGesture)(FL_GESTURE_NW + i);
        }
        return FL_GESTURE_MOVE;
    }
    const Fl_Entity *e = fl_scene_entity(scene, fl_scene_edit_focus(p->edit));
    if (!e || fl_scene_edit_count(p->edit) != 1) return FL_GESTURE_MOVE;
    const Fl_ResolvedEntity *entity = &g->entities[e - scene->entities];
    if (!entity->visible || !entity->invertible) return FL_GESTURE_MOVE;
    for (unsigned i = 0; i < 5; ++i) {
        Fl_Vec3 probe = {0, 0, 0};
        if (i == 4 && !handle_world(entity, 4, 1, &probe)) continue;
        if (i < 4 && !handle_world(entity, i, 1, &probe)) continue;
        double ppw = pixels_per_world(&view, h, probe);
        Fl_Vec3 world;
        if (i == 4) { if (!handle_world(entity, 4, ppw, &world)) continue; }
        else world = probe;
        double hx, hy;
        if (project_window(&view, viewport, world, &hx, &hy) && hypot(x - hx, y - hy) <= FL_STYLE_HANDLE_HIT)
            return (Fl_EditGesture)(FL_GESTURE_NW + i);
    }
    return FL_GESTURE_MOVE;
}

/* Draw the repeating world grid in one bounded full-viewport pass, with a major line every five cells. */
static void draw_grid(Fl_ScenePreview *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame)
{
    const unsigned char minor[4] = FL_STYLE_CANVAS_GRID_RGBA, major[4] = FL_STYLE_CANVAS_GRID_MAJOR_RGBA;
    double period = FL_SCENE_GRID_SPACING * 5;
    float push[28] = {2, 0, -1, (float)frame->width, 0, 2, -1, (float)frame->height,
        fl_scene_renderer_linear(minor[0]), fl_scene_renderer_linear(minor[1]), fl_scene_renderer_linear(minor[2]), minor[3] / 255.f,
        fl_scene_renderer_linear(major[0]), fl_scene_renderer_linear(major[1]), fl_scene_renderer_linear(major[2]), major[3] / 255.f,
        (float)fmod(frame->x, period), (float)fmod(frame->y, period), (float)FL_SCENE_GRID_SPACING, FL_RENDER_KIND_GRID, 0, 0, 0, 0, 0, 0, 0, 0};
    fl_scene_renderer_push(p->renderer, cmd, push, fl_scene_renderer_white(p->renderer));
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
    Fl_CanvasMapping map = preview_mapping(scene, logical_w, logical_h);
    Fl_SceneFrame target = map.target, frame = map.world;
    PreviewView view;
    if (p->renderer && scene && preview_view(p, scene, state, &frame, &view))
        fl_scene_renderer_set_view(p->renderer, view.view, view.right, view.up, view.bloom);
    if (g && map.scale > 0 && depth && p->renderer && scene && fl_scene_renderer_begin(p->renderer, cmd, ca_viewport_format(viewport), w, h, true)) {
        fl_scene_renderer_background(p->renderer, cmd, &frame, &target, scene->background);
        if (view.perspective) {
            const unsigned char minor[4] = FL_STYLE_CANVAS_GRID_RGBA, major[4] = FL_STYLE_CANVAS_GRID_MAJOR_RGBA;
            double extent = fmax(scene->width, scene->height) * 8;
            if (extent < 4096) extent = 4096;
            fl_scene_renderer_grid_plane(p->renderer, cmd, scene->width * 0.5 - extent * 0.5, scene->height * 0.5 - extent * 0.5, extent, FL_SCENE_GRID_SPACING, minor, major);
        } else draw_grid(p, cmd, &frame);
        Fl_RenderTarget surface = {.view = ca_viewport_image_view(viewport), .format = ca_viewport_format(viewport), .width = w, .height = h, .slot = ca_viewport_frame_index(viewport), .image = ca_viewport_image(viewport), .depth = depth};
        fl_scene_renderer_entities(p->renderer, cmd, &surface, &frame, scene, state, g);
        VkDescriptorSet white = fl_scene_renderer_white(p->renderer);
        double logical_scale = map.scale;
        Fl_Appearance frame_edge = {.stroke = FL_STYLE_CANVAS_FRAME_RGBA, .stroke_width = 1 / map.scale};
        Fl_Affine origin = fl_affine_identity(); origin.v[2] = target.x; origin.v[5] = target.y;
        fl_scene_renderer_primitive(p->renderer, cmd, &frame, origin, target.width, target.height, FL_WIDGET_RECTANGLE, frame_edge, white);
        Fl_Appearance outline = {.stroke = FL_STYLE_GIZMO_RGBA, .font_size = 48};
        for (size_t i = 0; i < g->count; ++i) {
            const Fl_ResolvedEntity *v = &g->entities[i];
            if (!v->visible || !fl_scene_edit_contains(p->edit, scene->entities[i].id)) continue;
            double gw, gh;
            Fl_Mat4 face = widget_face(v, &gw, &gh);
            double axis = hypot(hypot(face.m[0], face.m[1]), face.m[2]);
            if (view.perspective) {
                Fl_Vec3 center = fl_mat4_point(face, (Fl_Vec3){gw * 0.5, gh * 0.5, 0});
                double ppw = pixels_per_world(&view, logical_h, center);
                outline.stroke_width = ppw > 0 && axis > 1e-9 ? FL_STYLE_OUTLINE_WIDTH / (ppw * axis) : 0;
            } else {
                double local_scale = v->mesh ? axis : fmax(hypot(v->world.v[0], v->world.v[3]), hypot(v->world.v[1], v->world.v[4]));
                outline.stroke_width = FL_STYLE_OUTLINE_WIDTH / (logical_scale * fmax(local_scale, 1e-9));
            }
            fl_scene_renderer_model(p->renderer, cmd, &frame, face, gw, gh, FL_WIDGET_RECTANGLE, outline, white, NULL);
        }
        Fl_Appearance handle = {.fill = FL_STYLE_GIZMO_RGBA, .font_size = 48};
        double size = FL_STYLE_HANDLE_SIZE / logical_scale;
        for (unsigned i = 0; i < 5; ++i) {
            if (view.perspective) {
                const Fl_Entity *e = fl_scene_entity(scene, fl_scene_edit_focus(p->edit));
                if (!e || fl_scene_edit_count(p->edit) != 1) break;
                const Fl_ResolvedEntity *entity = &g->entities[e - scene->entities];
                Fl_Vec3 probe;
                if (!handle_world(entity, i < 4 ? i : 4, 1, &probe)) break;
                double ppw = pixels_per_world(&view, logical_h, probe);
                Fl_Vec3 world;
                if (!handle_world(entity, i, ppw, &world) || !(ppw > 0)) break;
                fl_scene_renderer_model(p->renderer, cmd, &frame, billboard(view.right, view.up, world, FL_STYLE_HANDLE_SIZE / ppw), 1, 1, FL_WIDGET_RECTANGLE, handle, white, NULL);
            } else {
                double x, y;
                if (!handle_point(p, scene, g, i, logical_scale, &x, &y)) break;
                Fl_Affine matrix = fl_affine_identity(); matrix.v[2] = x - size * .5; matrix.v[5] = y - size * .5;
                fl_scene_renderer_primitive(p->renderer, cmd, &frame, matrix, size, size, FL_WIDGET_RECTANGLE, handle, white);
            }
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
    p->nav_scale = 1;
    return p;
}
void fl_scene_preview_destroy(Fl_ScenePreview *p)
{
    if (!p) return;
    fl_scene_renderer_destroy(p->renderer);
    free(p);
}
