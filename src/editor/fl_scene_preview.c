#include "fl_scene_preview.h"
#include "fl_scene_renderer.h"
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
};

/* Resolve the saved camera into a viewport-centered output frame. */
static Fl_CanvasMapping preview_mapping(const Fl_Scene *scene, double w, double h)
{
    return fl_canvas_mapping(fl_scene_frame(scene), w, h, FL_STYLE_ARTBOARD_MARGIN);
}

bool fl_scene_preview_point(Fl_ScenePreview *p,Ca_Viewport *viewport,double wx,double wy,
                            bool outside,double *sx,double *sy)
{
    if (!p || !viewport || !sx || !sy || !isfinite(wx) || !isfinite(wy)) return false;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),fl_scene_edit_scene(p->edit));
    float vx,vy,w,h;ca_viewport_screen_rect(viewport,&vx,&vy,&w,&h);
    Fl_CanvasMapping map=preview_mapping(scene,w,h);
    if (map.scale<=0) return false;
    *sx=map.world.x+(wx-vx)/map.scale; *sy=map.world.y+(wy-vy)/map.scale;
    return outside||(wx>=vx&&wy>=vy&&wx<=vx+w&&wy<=vy+h);
}

void fl_scene_preview_pan(Fl_ScenePreview *p, Ca_Viewport *viewport, double dx, double dy)
{
    if (!p || !viewport || !isfinite(dx) || !isfinite(dy) || (dx==0&&dy==0)) return;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),fl_scene_edit_scene(p->edit));
    float x,y,w,h;ca_viewport_screen_rect(viewport,&x,&y,&w,&h);
    Fl_CanvasMapping map=preview_mapping(scene,w,h);
    if (map.scale<=0) return;
    double px=scene->camera_x-dx/map.scale, py=scene->camera_y-dy/map.scale;
    if (!isfinite(px) || !isfinite(py)) return;
    if (!fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind=FL_OP_SCENE_CAMERA,
        .scene=scene->id,.camera_x=px,.camera_y=py}, NULL)) return;
    ca_viewport_request_redraw(viewport);
}

void fl_scene_preview_reset_view(Fl_ScenePreview *p)
{
    if (!p) return;
    fl_project_apply(fl_scene_edit_project(p->edit), &(Fl_Op){.kind=FL_OP_SCENE_CAMERA,
        .scene=fl_scene_edit_scene(p->edit)}, NULL);
}

/* Compute scene coordinates of a selected entity's corner/rotation handle. */
static bool handle_point(Fl_ScenePreview *p,const Fl_Scene *scene,const Fl_SceneGeometry *g,
                         unsigned handle,double pixels_per_scene,double *x,double *y)
{
    const Fl_Entity *e=fl_scene_entity(scene,fl_scene_edit_primary(p->edit));
    if (!e||fl_scene_edit_count(p->edit)!=1) return false;
    const Fl_ResolvedEntity *v=&g->entities[e-scene->entities];
    if (!v->visible||!v->invertible) return false;
    const double corners[4][2]={{0,0},{1,0},{1,1},{0,1}};
    if (handle<4) fl_affine_point(v->world,corners[handle][0]*v->local.width,corners[handle][1]*v->local.height,x,y);
    else {
        fl_affine_point(v->world,v->local.width*.5,0,x,y);
        double length=hypot(v->world.v[1],v->world.v[4]);
        if (length<1e-12) return false;
        double offset=FL_STYLE_ROTATE_OFFSET/(pixels_per_scene*length);
        *x-=v->world.v[1]*offset;*y-=v->world.v[4]*offset;
    }
    return true;
}

Fl_EditGesture fl_scene_preview_handle(Fl_ScenePreview *p,Ca_Viewport *viewport,double x,double y)
{
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),fl_scene_edit_scene(p->edit));
    const Fl_SceneGeometry *g=fl_scene_edit_geometry(p->edit);float vx,vy,w,h;
    ca_viewport_screen_rect(viewport,&vx,&vy,&w,&h);(void)vx;(void)vy;
    Fl_CanvasMapping map=preview_mapping(scene,w,h);
    if (!g||map.scale<=0) return FL_GESTURE_MOVE;
    double scale=map.scale;
    for (unsigned i=0;i<5;++i) {
        double hx,hy;
        if (handle_point(p,scene,g,i,scale,&hx,&hy)&&hypot(x-hx,y-hy)<=FL_STYLE_HANDLE_HIT/scale)
            return (Fl_EditGesture)(FL_GESTURE_NW+i);
    }
    return FL_GESTURE_MOVE;
}

/* Draw the repeating world grid in one bounded full-viewport pass, with a major line every five cells. */
static void draw_grid(Fl_ScenePreview *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame)
{
    const unsigned char minor[4]=FL_STYLE_CANVAS_GRID_RGBA, major[4]=FL_STYLE_CANVAS_GRID_MAJOR_RGBA;
    double period=FL_SCENE_GRID_SPACING*5;
    float push[28]={2,0,-1,(float)frame->width,0,2,-1,(float)frame->height,
        fl_scene_renderer_linear(minor[0]),fl_scene_renderer_linear(minor[1]),fl_scene_renderer_linear(minor[2]),minor[3]/255.f,
        fl_scene_renderer_linear(major[0]),fl_scene_renderer_linear(major[1]),fl_scene_renderer_linear(major[2]),major[3]/255.f,
        (float)fmod(frame->x,period),(float)fmod(frame->y,period),(float)FL_SCENE_GRID_SPACING,FL_RENDER_KIND_GRID,0,0,0,0,0,0,0,0};
    fl_scene_renderer_push(p->renderer,cmd,push,fl_scene_renderer_white(p->renderer));
}

void fl_scene_preview_render(Ca_Viewport *viewport,void *data)
{
    Fl_ScenePreview *p=data;
    Fl_Project *project=fl_scene_edit_project(p->edit);
    const Fl_Scene *scene=fl_project_scene(project,fl_scene_edit_scene(p->edit));
    const Fl_SceneGeometry *g=fl_scene_edit_geometry(p->edit);
    const Fl_SceneState *state=fl_scene_edit_state(p->edit);
    if (state&&scene&&(state->scene!=scene->id||state->count!=scene->entity_count)) state=NULL;
    if (!p->renderer) p->renderer=fl_scene_renderer_create(&(Fl_SceneRendererDesc){.instance=ca_viewport_instance(viewport),.fonts=p->fonts,
        .jobs=p->jobs,.bus=p->bus,.invalidate=p->invalidate,.invalidate_data=p->invalidate_data});
    if (p->renderer&&scene) fl_scene_renderer_prepare(p->renderer,project,scene,state,1,false);
    uint32_t w=ca_viewport_width(viewport),h=ca_viewport_height(viewport);
    VkCommandBuffer cmd=ca_viewport_cmd(viewport);
    VkRenderingAttachmentInfo attachment={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=ca_viewport_image_view(viewport),.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.clearValue.color={.float32=FL_STYLE_VIEW_CLEAR}};
    VkRenderingInfo info={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea.extent={w,h},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&attachment};
    vkCmdBeginRendering(cmd,&info);
    float logical_x,logical_y,logical_w,logical_h;
    ca_viewport_screen_rect(viewport,&logical_x,&logical_y,&logical_w,&logical_h);
    Fl_CanvasMapping map=preview_mapping(scene,logical_w,logical_h);
    Fl_SceneFrame target=map.target, frame=map.world;
    if (g&&map.scale>0&&p->renderer&&fl_scene_renderer_begin(p->renderer,cmd,ca_viewport_format(viewport),w,h)) {
        fl_scene_renderer_background(p->renderer,cmd,&frame,&target,scene->background);
        draw_grid(p,cmd,&frame);
        fl_scene_renderer_entities(p->renderer,cmd,&frame,scene,state,g);
        VkDescriptorSet white=fl_scene_renderer_white(p->renderer);
        double logical_scale=map.scale;
        Fl_Appearance frame_edge={.stroke=FL_STYLE_CANVAS_FRAME_RGBA,.stroke_width=1/map.scale};
        Fl_Affine origin=fl_affine_identity(); origin.v[2]=target.x; origin.v[5]=target.y;
        fl_scene_renderer_primitive(p->renderer,cmd,&frame,origin,target.width,target.height,FL_WIDGET_RECTANGLE,frame_edge,white);
        Fl_Appearance outline={.stroke=FL_STYLE_GIZMO_RGBA,.font_size=48};
        for (size_t i=0;i<g->count;++i) {
            const Fl_ResolvedEntity *v=&g->entities[i];
            if (!v->visible||!fl_scene_edit_contains(p->edit,scene->entities[i].id)) continue;
            double local_scale=fmax(hypot(v->world.v[0],v->world.v[3]),hypot(v->world.v[1],v->world.v[4]));
            outline.stroke_width=FL_STYLE_OUTLINE_WIDTH/(logical_scale*fmax(local_scale,1e-9));
            fl_scene_renderer_primitive(p->renderer,cmd,&frame,v->world,v->local.width,v->local.height,FL_WIDGET_RECTANGLE,outline,white);
        }
        Fl_Appearance handle={.fill=FL_STYLE_GIZMO_RGBA,.font_size=48};double size=FL_STYLE_HANDLE_SIZE/logical_scale;
        for (unsigned i=0;i<5;++i) {
            double x,y;if (!handle_point(p,scene,g,i,logical_scale,&x,&y)) break;
            Fl_Affine matrix=fl_affine_identity();matrix.v[2]=x-size*.5;matrix.v[5]=y-size*.5;
            fl_scene_renderer_primitive(p->renderer,cmd,&frame,matrix,size,size,FL_WIDGET_RECTANGLE,handle,white);
        }
    }
    vkCmdEndRendering(cmd);
}

Fl_ScenePreview *fl_scene_preview_create(const Fl_ScenePreviewDesc *desc)
{
    if (!desc||!desc->edit||!desc->fonts||!desc->jobs||!desc->bus) return NULL;
    Fl_ScenePreview *p=calloc(1,sizeof(*p));if (!p) return NULL;
    p->edit=desc->edit;p->fonts=desc->fonts;p->jobs=desc->jobs;p->bus=desc->bus;
    p->invalidate=desc->invalidate;p->invalidate_data=desc->invalidate_data;
    return p;
}
void fl_scene_preview_destroy(Fl_ScenePreview *p)
{
    if (!p) return;
    fl_scene_renderer_destroy(p->renderer);
    free(p);
}
