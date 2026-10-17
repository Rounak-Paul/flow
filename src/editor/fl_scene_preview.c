#include "fl_scene_preview.h"
#include "fl_gpu_image.h"
#include "style.h"
#include "core/fl_log.h"
#include "media/fl_media.h"
#include "platform/fl_paths.h"
#include "text/fl_text_raster.h"
#include <ca_gpu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define KIND_IMAGE 6
#define IMAGE_MAX_SIDE 8192u

typedef struct Fl_CachedText {
    Fl_Id entity;
    unsigned char *text;
    size_t length;
    double width,height,font_size;
    Fl_Typography typography;
    Fl_GpuImage *image;
} Fl_CachedText;
/* Decode request owned by its image entry until the worker finishes. */
typedef struct Fl_ImageDecode {
    char path[FL_PATH_MAX];
    unsigned char *pixels;
    uint32_t width,height;
} Fl_ImageDecode;
typedef enum Fl_ImageState { IMAGE_DECODING, IMAGE_DECODED, IMAGE_READY, IMAGE_FAILED } Fl_ImageState;
typedef struct Fl_CachedImage {
    Fl_Id asset;
    Fl_ImageState state;
    Fl_JobHandle job;
    Fl_ImageDecode *decode;
    Fl_GpuImage *image;
    bool orphaned;              /* no longer referenced; freed once its decode finishes */
} Fl_CachedImage;
struct Fl_ScenePreview {
    Fl_SceneEdit *edit;
    Fl_Fonts *fonts;
    Fl_JobSystem *jobs;
    Fl_EventBus *bus;
    Fl_PreviewInvalidateFn invalidate;
    void *invalidate_data;
    VkDevice device;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkFormat format;
    Fl_GpuImages *images;
    Fl_TextRasterizer *rasterizer;
    Fl_CachedText *texts;
    size_t text_count;
    Fl_Id text_scene;
    uint64_t text_revision, text_generation;
    Fl_CachedImage *pictures;
    size_t picture_count, picture_capacity;
    Fl_Id picture_scene;
    uint64_t picture_revision;
};

/* Build a compact solid-colour GPU pipeline on the viewport's native format. */
static bool create_pipeline(Fl_ScenePreview *p, Ca_Viewport *viewport)
{
    if (p->pipeline) return p->format == ca_viewport_format(viewport);
    Ca_Instance *instance = ca_viewport_instance(viewport); p->device = ca_gpu_device(instance);
    const char *vs = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; } p;"
        "layout(location=0) out vec2 uv;vec2 v[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));"
        "void main(){uv=v[gl_VertexIndex];vec3 q=vec3(uv,1);gl_Position=vec4(dot(p.a.xyz,q),dot(p.b.xyz,q),0,1);}";
    const char *fs = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; } p;"
        "layout(set=0,binding=0) uniform sampler2D mask;layout(location=0) in vec2 uv;layout(location=0) out vec4 color;"
        "void main(){float kind=p.params.w;vec2 size=p.params.xy;float sw=p.params.z;"
        "if(kind==5){color=p.fill;color.a*=texture(mask,uv).r;return;}"
        "if(kind==6){color=texture(mask,uv);color.a*=p.fill.a;return;}"
        "if(kind==3||kind==4){float shaft=abs((uv.y-.5)*size.y)-sw*.5;"
        "float head=max(.72-uv.x,abs(uv.y-.5)-(1-uv.x)/.28*.5);"
        "float cover=1-smoothstep(-fwidth(shaft),fwidth(shaft),shaft);"
        "if(kind==4){cover*=1-smoothstep(.79,.8,uv.x);cover=max(cover,1-smoothstep(-fwidth(head),fwidth(head),head));}color=p.stroke;color.a*=cover;return;}"
        "float edge;if(kind==2){vec2 q=(uv-.5)*2;edge=(length(q)-1)*min(size.x,size.y)*.5;}"
        "else{vec2 q=abs((uv-.5)*size)-size*.5;edge=max(q.x,q.y);}"
        "float aa=max(fwidth(edge),.001);float cover=1-smoothstep(-aa,aa,edge);"
        "float border=sw>0 ? smoothstep(-sw-aa,-sw+aa,edge) : 0;"
        "color=mix(p.fill,p.stroke,border);color.a*=cover;}";
    VkShaderModule vert = ca_shader_compile(instance, vs, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(instance, fs, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
    VkDescriptorSetLayout mask_layout=fl_gpu_images_layout(p->images);
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&mask_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    bool ok = vert && frag && vkCreatePipelineLayout(p->device, &layout, NULL, &p->layout) == VK_SUCCESS;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blends = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = states};
    p->format = ca_viewport_format(viewport);
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &p->format};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pColorBlendState = &blends, .pDynamicState = &dynamic, .layout = p->layout};
    if (ok) ok = vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &p->pipeline) == VK_SUCCESS;
    if (vert) vkDestroyShaderModule(p->device, vert, NULL);
    if (frag) vkDestroyShaderModule(p->device, frag, NULL);
    if (!ok && p->layout) { vkDestroyPipelineLayout(p->device, p->layout, NULL); p->layout = VK_NULL_HANDLE; }
    return ok;
}

/* Decode stored sRGB channels for the linear-light scene target. */
static float linear_channel(unsigned char value)
{
    float v=value/255.0f;
    return v<=.04045f ? v/12.92f : powf((v+.055f)/1.055f,2.4f);
}

/* Resolve the same letterboxed artboard for rendering, picking and drag placement. */
static bool artboard(const Fl_Scene *scene,double w,double h,double *x,double *y,double *aw,double *ah)
{
    if (!scene||w<=0||h<=0) return false;
    double scale=fmin(w/scene->width,h/scene->height);
    *aw=floor(scene->width*scale);*ah=floor(scene->height*scale);
    *x=floor((w-*aw)*.5);*y=floor((h-*ah)*.5);
    return *aw>0&&*ah>0;
}

bool fl_scene_preview_point(Fl_ScenePreview *p,Ca_Viewport *viewport,double wx,double wy,
                            bool outside,double *sx,double *sy)
{
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),fl_scene_edit_scene(p->edit));
    float vx,vy,w,h;ca_viewport_screen_rect(viewport,&vx,&vy,&w,&h);
    double x,y,aw,ah;
    if (!artboard(scene,w,h,&x,&y,&aw,&ah)) return false;
    *sx=(wx-vx-x)*scene->width/aw;*sy=(wy-vy-y)*scene->height/ah;
    return outside||(*sx>=0&&*sy>=0&&*sx<=scene->width&&*sy<=scene->height);
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
    double ax,ay,aw,ah;
    if (!g||!artboard(scene,w,h,&ax,&ay,&aw,&ah)) return FL_GESTURE_MOVE;
    double scale=fmin(aw/scene->width,ah/scene->height);
    for (unsigned i=0;i<5;++i) {
        double hx,hy;
        if (handle_point(p,scene,g,i,scale,&hx,&hy)&&hypot(x-hx,y-hy)<=FL_STYLE_HANDLE_HIT/scale)
            return (Fl_EditGesture)(FL_GESTURE_NW+i);
    }
    return FL_GESTURE_MOVE;
}

/* Return whether two typography requests select the same face. */
static bool same_typography(const Fl_Typography *a,const Fl_Typography *b)
{
    return a->weight==b->weight&&a->italic==b->italic&&!strcmp(a->family,b->family);
}

/* Rasterize one text entity into a coverage texture using the resolved face. */
static Fl_GpuImage *rasterize_text(Fl_ScenePreview *p,const Fl_CachedText *entry,const unsigned char *text)
{
    Fl_FontMatch match=fl_fonts_match(p->fonts,&entry->typography);
    Fl_TextStyle style={match.face,entry->font_size,match.synthetic_bold,match.synthetic_italic};
    uint32_t w,h;
    unsigned char *mask=fl_text_rasterize(p->rasterizer,&style,text,entry->length,entry->width,entry->height,&w,&h);
    Fl_GpuImage *image=mask ? fl_gpu_image_create(p->images,FL_GPU_IMAGE_COVERAGE,mask,w,h) : NULL;
    free(mask);
    return image;
}

/* Prepare text masks only when content/box/font changes, preserving them on colour edits. */
static void prepare_texts(Fl_ScenePreview *p,const Fl_Scene *scene)
{
    uint64_t generation=fl_fonts_generation(p->fonts);
    if (p->text_scene==scene->id&&p->text_revision==scene->revision&&p->text_generation==generation) return;
    bool fonts_changed=p->text_generation!=generation;
    if (fonts_changed) fl_text_rasterizer_flush(p->rasterizer);
    size_t count=0;
    for (size_t i=0;i<scene->entity_count;++i) if (fl_entity_component(&scene->entities[i],FL_COMPONENT_TEXT)) ++count;
    if (count>SIZE_MAX/sizeof(Fl_CachedText)) return;
    Fl_CachedText *next=count ? calloc(count,sizeof(*next)) : NULL;
    if (count&&!next) return;
    size_t at=0;
    for (size_t i=0;i<scene->entity_count;++i) {
        const Fl_Entity *e=&scene->entities[i];const Fl_Component *text=fl_entity_component(e,FL_COMPONENT_TEXT);
        if (!text) continue;
        Fl_Transform t;Fl_CachedText *entry=&next[at++];entry->entity=e->id;
        if (!fl_transform_decode(fl_entity_component(e,FL_COMPONENT_TRANSFORM),&t)) continue;
        entry->length=text->size;entry->width=t.width;entry->height=t.height;entry->font_size=fl_widget_appearance(e).font_size;
        entry->typography=fl_widget_typography(e);
        entry->text=text->size ? malloc(text->size) : NULL;
        if (text->size&&!entry->text) continue;
        if (text->size) memcpy(entry->text,text->data,text->size);
        for (size_t j=0;!fonts_changed&&j<p->text_count;++j) {
            Fl_CachedText *old=&p->texts[j];
            if (old->entity==e->id&&p->text_scene==scene->id&&old->width==entry->width&&old->height==entry->height&&
                old->font_size==entry->font_size&&same_typography(&old->typography,&entry->typography)&&old->length==entry->length&&
                (!text->size||(old->text&&!memcmp(old->text,text->data,text->size)))) {
                entry->image=old->image;old->image=NULL;break;
            }
        }
        if (!entry->image&&t.width>0&&t.height>0) {
            entry->image=rasterize_text(p,entry,text->data);
            if (!entry->image) FL_LOG_ERROR("preview","could not rasterize text entity %llu",(unsigned long long)e->id);
        }
    }
    bool retired=false;
    for (size_t i=0;i<p->text_count;++i) retired|=p->texts[i].image!=NULL;
    if (retired) vkDeviceWaitIdle(p->device);
    for (size_t i=0;i<p->text_count;++i) {fl_gpu_image_destroy(p->texts[i].image);free(p->texts[i].text);}
    free(p->texts);p->texts=next;p->text_count=count;p->text_scene=scene->id;p->text_revision=scene->revision;p->text_generation=generation;
}

/* Worker body: decode one stored image; touches only its request. */
static void decode_image(void *data)
{
    Fl_ImageDecode *d=data;
    d->pixels=fl_image_decode(d->path,IMAGE_MAX_SIDE,&d->width,&d->height);
}

/* Release an entry's CPU/GPU storage; callers guarantee its decode finished and GPU reads completed. */
static void release_picture(Fl_CachedImage *entry)
{
    if (entry->decode) fl_image_free(entry->decode->pixels);
    free(entry->decode);fl_gpu_image_destroy(entry->image);
    *entry=(Fl_CachedImage){0};
}

/* Return the cache entry of asset, or NULL. */
static Fl_CachedImage *find_picture(Fl_ScenePreview *p,Fl_Id asset)
{
    for (size_t i=0;i<p->picture_count;++i) if (p->pictures[i].asset==asset&&!p->pictures[i].orphaned) return &p->pictures[i];
    return NULL;
}

/* Queue a decode for asset; failures leave a FAILED entry so the placeholder is shown without retry loops. */
static void request_picture(Fl_ScenePreview *p,const Fl_Asset *asset)
{
    if (p->picture_count==p->picture_capacity) {
        size_t capacity=p->picture_capacity ? p->picture_capacity*2 : 8;
        Fl_CachedImage *grown=realloc(p->pictures,capacity*sizeof(*grown));
        if (!grown) return;
        p->pictures=grown;p->picture_capacity=capacity;
    }
    Fl_CachedImage *entry=&p->pictures[p->picture_count++];
    *entry=(Fl_CachedImage){.asset=asset->id,.state=IMAGE_FAILED};
    entry->decode=calloc(1,sizeof(*entry->decode));
    if (!entry->decode||!fl_project_asset_path(fl_scene_edit_project(p->edit),asset,entry->decode->path,sizeof(entry->decode->path))) return;
    entry->job=fl_job_submit(p->jobs,&(Fl_JobDesc){.fn=decode_image,.data=entry->decode,.priority=FL_JOB_PRIORITY_NORMAL,.notify=true});
    if (entry->job.generation) entry->state=IMAGE_DECODING;
}

/* Drop pictures the current scene no longer references; in-flight decodes are orphaned until they finish. */
static void prepare_pictures(Fl_ScenePreview *p,const Fl_Scene *scene)
{
    if (p->picture_scene==scene->id&&p->picture_revision==scene->revision) return;
    Fl_Project *project=fl_scene_edit_project(p->edit);
    bool waited=false;
    for (size_t i=0;i<p->picture_count;) {
        Fl_CachedImage *entry=&p->pictures[i];
        bool used=false;
        for (size_t j=0;!entry->orphaned&&!used&&j<scene->entity_count;++j) {
            Fl_Id asset;
            used=fl_image_asset_decode(fl_entity_component(&scene->entities[j],FL_COMPONENT_IMAGE),&asset)&&asset==entry->asset&&
                 fl_project_asset(project,asset);
        }
        if (used) {++i;continue;}
        if (entry->state==IMAGE_DECODING) {entry->orphaned=true;++i;continue;}
        if (entry->image&&!waited) {vkDeviceWaitIdle(p->device);waited=true;}
        release_picture(entry);
        p->pictures[i]=p->pictures[--p->picture_count];
    }
    p->picture_scene=scene->id;p->picture_revision=scene->revision;
}

/* Request missing decodes and upload finished ones for every image entity, before rendering begins. */
static void stage_pictures(Fl_ScenePreview *p,const Fl_Scene *scene)
{
    Fl_Project *project=fl_scene_edit_project(p->edit);
    for (size_t i=0;i<scene->entity_count;++i) {
        Fl_Id id;const Fl_Asset *asset;
        if (!fl_image_asset_decode(fl_entity_component(&scene->entities[i],FL_COMPONENT_IMAGE),&id)||
            !(asset=fl_project_asset(project,id))) continue;
        Fl_CachedImage *entry=find_picture(p,id);
        if (!entry) {request_picture(p,asset);continue;}
        if (entry->state!=IMAGE_DECODED) continue;
        entry->image=entry->decode->pixels ? fl_gpu_image_create(p->images,FL_GPU_IMAGE_SRGBA,entry->decode->pixels,
                                                                 entry->decode->width,entry->decode->height) : NULL;
        fl_image_free(entry->decode->pixels);free(entry->decode);entry->decode=NULL;
        entry->state=entry->image ? IMAGE_READY : IMAGE_FAILED;
        if (!entry->image) FL_LOG_ERROR("preview","could not load image asset %s",asset->name);
    }
}

/* Borrow an image entity's uploaded texture, or VK_NULL_HANDLE while loading or after failure. */
static VkDescriptorSet picture_descriptor(Fl_ScenePreview *p,const Fl_Entity *e)
{
    Fl_Id id;
    if (!fl_image_asset_decode(fl_entity_component(e,FL_COMPONENT_IMAGE),&id)) return VK_NULL_HANDLE;
    const Fl_CachedImage *entry=find_picture(p,id);
    return entry&&entry->state==IMAGE_READY ? fl_gpu_image_descriptor(entry->image) : VK_NULL_HANDLE;
}

/* Observe decode completion; orphaned entries are freed, others upload at the next render. */
static bool job_done(const Fl_Event *event,void *data)
{
    Fl_ScenePreview *p=data;const Fl_JobDoneEvent *done=event->payload;
    for (size_t i=0;i<p->picture_count;++i) {
        Fl_CachedImage *entry=&p->pictures[i];
        if (entry->state!=IMAGE_DECODING||entry->job.index!=done->job.index||entry->job.generation!=done->job.generation) continue;
        if (entry->orphaned) {release_picture(entry);p->pictures[i]=p->pictures[--p->picture_count];}
        else {entry->state=IMAGE_DECODED;if (p->invalidate) p->invalidate(p->invalidate_data);}
        break;
    }
    return false;
}

/* Record one affine primitive through the shared sampled-mask pipeline. */
static void draw_primitive(Fl_ScenePreview *p,VkCommandBuffer cmd,const Fl_Scene *scene,Fl_Affine matrix,
                           double width,double height,Fl_WidgetKind kind,Fl_Appearance appearance,VkDescriptorSet mask)
{
    const double *a=matrix.v;
    float push[20]={(float)(a[0]*width*2/scene->width),(float)(a[1]*height*2/scene->width),(float)(a[2]*2/scene->width-1),0,
        (float)(a[3]*width*2/scene->height),(float)(a[4]*height*2/scene->height),(float)(a[5]*2/scene->height-1),0,
        linear_channel(appearance.fill[0]),linear_channel(appearance.fill[1]),linear_channel(appearance.fill[2]),appearance.fill[3]/255.f,
        linear_channel(appearance.stroke[0]),linear_channel(appearance.stroke[1]),linear_channel(appearance.stroke[2]),appearance.stroke[3]/255.f,
        (float)width,(float)height,(float)appearance.stroke_width,(float)kind};
    for (size_t i=0;i<20;++i) if (!isfinite(push[i])) return;
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,0,1,&mask,0,NULL);
    vkCmdPushConstants(cmd,p->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),push);
    vkCmdDraw(cmd,6,1,0,0);
}

void fl_scene_preview_render(Ca_Viewport *viewport,void *data)
{
    Fl_ScenePreview *p=data;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),fl_scene_edit_scene(p->edit));
    const Fl_SceneGeometry *g=fl_scene_edit_geometry(p->edit);
    p->device=ca_gpu_device(ca_viewport_instance(viewport));
    if (!p->images) p->images=fl_gpu_images_create(ca_viewport_instance(viewport));
    if (p->images&&scene) {prepare_texts(p,scene);prepare_pictures(p,scene);stage_pictures(p,scene);}
    uint32_t w=ca_viewport_width(viewport),h=ca_viewport_height(viewport);
    VkCommandBuffer cmd=ca_viewport_cmd(viewport);
    VkRenderingAttachmentInfo attachment={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView=ca_viewport_image_view(viewport),.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.clearValue.color={.float32=FL_STYLE_VIEW_CLEAR}};
    VkRenderingInfo info={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea.extent={w,h},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&attachment};
    vkCmdBeginRendering(cmd,&info);
    double ax,ay,aw,ah;
    if (g&&artboard(scene,w,h,&ax,&ay,&aw,&ah)) {
        VkRect2D area={{(int32_t)ax,(int32_t)ay},{(uint32_t)aw,(uint32_t)ah}};
        VkClearAttachment clear={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.clearValue.color={.float32=FL_STYLE_ARTBOARD_CLEAR}};
        VkClearRect rect={.rect=area,.layerCount=1};vkCmdClearAttachments(cmd,1,&clear,1,&rect);
        if (p->images&&create_pipeline(p,viewport)) {
            VkViewport view={(float)ax,(float)ay,(float)aw,(float)ah,0,1};
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipeline);vkCmdSetViewport(cmd,0,1,&view);vkCmdSetScissor(cmd,0,1,&area);
            VkDescriptorSet white=fl_gpu_images_white(p->images);
            const Fl_Appearance picture={.fill={255,255,255,255}},placeholder={.fill=FL_STYLE_IMAGE_PLACEHOLDER_RGBA,
                .stroke=FL_STYLE_IMAGE_PLACEHOLDER_STROKE_RGBA,.stroke_width=FL_STYLE_IMAGE_PLACEHOLDER_STROKE};
            for (size_t i=0;i<g->count;++i) {
                size_t index=g->order[i];const Fl_ResolvedEntity *v=&g->entities[index];const Fl_Entity *e=&scene->entities[index];
                if (!v->visible||v->kind==FL_WIDGET_GROUP) continue;
                VkDescriptorSet mask=white;
                if (v->kind==FL_WIDGET_TEXT) {
                    mask=VK_NULL_HANDLE;
                    if (p->text_scene==scene->id) for (size_t j=0;j<p->text_count;++j)
                        if (p->texts[j].entity==e->id) mask=fl_gpu_image_descriptor(p->texts[j].image);
                } else if (v->kind==FL_WIDGET_IMAGE) {
                    mask=picture_descriptor(p,e);
                    if (mask) draw_primitive(p,cmd,scene,v->world,v->local.width,v->local.height,(Fl_WidgetKind)KIND_IMAGE,picture,mask);
                    else draw_primitive(p,cmd,scene,v->world,v->local.width,v->local.height,FL_WIDGET_RECTANGLE,placeholder,white);
                    continue;
                }
                if (mask) draw_primitive(p,cmd,scene,v->world,v->local.width,v->local.height,v->kind,fl_widget_appearance(e),mask);
            }
            double scale=fmin(aw/scene->width,ah/scene->height);
            float node_x,node_y,node_width,node_height;
            ca_viewport_screen_rect(viewport,&node_x,&node_y,&node_width,&node_height);
            (void)node_x;(void)node_y;(void)node_height;
            double logical_scale=scale/(node_width>0 ? w/node_width : 1);
            Fl_Appearance outline={.stroke=FL_STYLE_GIZMO_RGBA,.font_size=48};
            for (size_t i=0;i<g->count;++i) {
                const Fl_ResolvedEntity *v=&g->entities[i];
                if (!v->visible||!fl_scene_edit_contains(p->edit,scene->entities[i].id)) continue;
                double local_scale=fmax(hypot(v->world.v[0],v->world.v[3]),hypot(v->world.v[1],v->world.v[4]));
                outline.stroke_width=FL_STYLE_OUTLINE_WIDTH/(logical_scale*fmax(local_scale,1e-9));
                draw_primitive(p,cmd,scene,v->world,v->local.width,v->local.height,FL_WIDGET_RECTANGLE,outline,white);
            }
            Fl_Appearance handle={.fill=FL_STYLE_GIZMO_RGBA,.font_size=48};double size=FL_STYLE_HANDLE_SIZE/logical_scale;
            for (unsigned i=0;i<5;++i) {
                double x,y;if (!handle_point(p,scene,g,i,logical_scale,&x,&y)) break;
                Fl_Affine matrix=fl_affine_identity();matrix.v[2]=x-size*.5;matrix.v[5]=y-size*.5;
                draw_primitive(p,cmd,scene,matrix,size,size,FL_WIDGET_RECTANGLE,handle,white);
            }
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
    p->rasterizer=fl_text_rasterizer_create();
    if (!p->rasterizer||!fl_event_subscribe(p->bus,&(Fl_SubscribeDesc){.type=fl_jobs_done_event(p->jobs),.fn=job_done,
        .user_data=p,.owner=p}).generation) {fl_text_rasterizer_destroy(p->rasterizer);free(p);return NULL;}
    return p;
}
void fl_scene_preview_destroy(Fl_ScenePreview *p)
{
    if (!p) return;
    fl_event_unsubscribe_owner(p->bus,p);
    for (size_t i=0;i<p->picture_count;++i) if (p->pictures[i].state==IMAGE_DECODING) fl_job_wait(p->jobs,p->pictures[i].job);
    if (p->device) vkDeviceWaitIdle(p->device);
    for (size_t i=0;i<p->picture_count;++i) release_picture(&p->pictures[i]);
    for (size_t i=0;i<p->text_count;++i) {fl_gpu_image_destroy(p->texts[i].image);free(p->texts[i].text);}
    free(p->pictures);free(p->texts);
    if (p->pipeline) vkDestroyPipeline(p->device,p->pipeline,NULL);
    if (p->layout) vkDestroyPipelineLayout(p->device,p->layout,NULL);
    fl_gpu_images_destroy(p->images);fl_text_rasterizer_destroy(p->rasterizer);free(p);
}
