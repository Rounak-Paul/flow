#include "fl_scene_renderer.h"
#include "fl_gpu_image.h"
#include "core/fl_log.h"
#include "media/fl_media.h"
#include "platform/fl_paths.h"
#include "text/fl_text_raster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "renderer"
#define KIND_IMAGE 6
#define IMAGE_MAX_SIDE 8192u
#define PIPELINE_FORMATS 4u

typedef struct Fl_CachedText {
    Fl_Id entity;
    unsigned char *text;
    size_t length;
    double width, height, font_size;
    Fl_Typography typography;
    Fl_GpuImage *image;
} Fl_CachedText;
/* Decode request owned by its image entry until the worker finishes. */
typedef struct Fl_ImageDecode {
    char path[FL_PATH_MAX];
    unsigned char *pixels;
    uint32_t width, height;
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
typedef struct Fl_Pipeline { VkFormat format; VkPipeline pipeline; } Fl_Pipeline;

struct Fl_SceneRenderer {
    Ca_Instance *instance;
    Fl_Fonts *fonts;
    Fl_JobSystem *jobs;
    Fl_EventBus *bus;
    void (*invalidate)(void *user_data);
    void *invalidate_data;
    const Fl_Project *project;      /* project of the latest prepare, for asset paths */
    VkDevice device;
    VkPipelineLayout layout;
    Fl_Pipeline pipelines[PIPELINE_FORMATS];
    size_t pipeline_count;
    Fl_GpuImages *images;
    Fl_TextRasterizer *rasterizer;
    Fl_CachedText *texts;
    size_t text_count;
    Fl_Id text_scene;
    uint64_t text_revision, text_generation;
    Fl_Tick text_tick;
    double text_scale;
    Fl_CachedImage *pictures;
    size_t picture_count, picture_capacity;
    Fl_Id picture_scene;
    uint64_t picture_revision;
};

float fl_scene_renderer_linear(unsigned char value)
{
    float v = value / 255.0f;
    return v <= .04045f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f);
}

/* Compile the primitive shaders and build one pipeline for a colour format. */
static bool create_pipeline(Fl_SceneRenderer *p, VkFormat format)
{
    const char *vs = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; } p;"
        "layout(location=0) out vec2 uv;vec2 v[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));"
        "void main(){uv=v[gl_VertexIndex];vec3 q=vec3(uv,1);gl_Position=vec4(dot(p.a.xyz,q),dot(p.b.xyz,q),0,1);}";
    const char *fs = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; } p;"
        "layout(set=0,binding=0) uniform sampler2D mask;layout(location=0) in vec2 uv;layout(location=0) out vec4 color;"
        "void main(){float kind=p.params.w;vec2 size=p.params.xy;float sw=p.params.z;"
        "if(kind==7){vec2 world=uv*vec2(p.a.w,p.b.w)+size;vec2 cell=world/sw;"
        "vec2 d=abs(fract(cell-.5)-.5)/max(fwidth(cell),vec2(.0001));"
        "vec2 big=cell/5;vec2 b=abs(fract(big-.5)-.5)/max(fwidth(big),vec2(.0001));"
        "float minor=(1-min(min(d.x,d.y),1))*smoothstep(2,6,1/max(fwidth(cell.x),fwidth(cell.y)));"
        "float major=(1-min(min(b.x,b.y),1))*smoothstep(2,6,1/max(fwidth(big.x),fwidth(big.y)));"
        "color=mix(p.fill,p.stroke,major);color.a*=max(minor,major);return;}"
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
    VkShaderModule vert = ca_shader_compile(p->instance, vs, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(p->instance, fs, VK_SHADER_STAGE_FRAGMENT_BIT);
    bool ok = vert && frag;
    if (ok && !p->layout) {
        VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
        VkDescriptorSetLayout mask_layout = fl_gpu_images_layout(p->images);
        VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &mask_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        ok = vkCreatePipelineLayout(p->device, &layout, NULL, &p->layout) == VK_SUCCESS;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (ok) {
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
        VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
        VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
            .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
            .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples,
            .pColorBlendState = &blends, .pDynamicState = &dynamic, .layout = p->layout};
        ok = vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) == VK_SUCCESS;
    }
    if (vert) vkDestroyShaderModule(p->device, vert, NULL);
    if (frag) vkDestroyShaderModule(p->device, frag, NULL);
    if (ok) p->pipelines[p->pipeline_count++] = (Fl_Pipeline){format, pipeline};
    return ok;
}

/* Rasterize one text entity into a coverage texture using the resolved face. */
static Fl_GpuImage *rasterize_text(Fl_SceneRenderer *p, const Fl_CachedText *entry, const unsigned char *text, double scale)
{
    Fl_FontMatch match = fl_fonts_match(p->fonts, &entry->typography);
    Fl_TextStyle style = {match.face, entry->font_size * scale, match.synthetic_bold, match.synthetic_italic};
    uint32_t w, h;
    unsigned char *mask = fl_text_rasterize(p->rasterizer, &style, text, entry->length, entry->width * scale, entry->height * scale, &w, &h);
    Fl_GpuImage *image = mask ? fl_gpu_image_create(p->images, FL_GPU_IMAGE_COVERAGE, mask, w, h) : NULL;
    free(mask);
    return image;
}

static bool same_typography(const Fl_Typography *a, const Fl_Typography *b)
{
    return a->weight == b->weight && a->italic == b->italic && !strcmp(a->family, b->family);
}

/* Prepare text masks only when content/box/font/scale changes, preserving them on colour edits. */
static void prepare_texts(Fl_SceneRenderer *p, const Fl_Scene *scene, const Fl_SceneState *state, double scale)
{
    uint64_t generation = fl_fonts_generation(p->fonts);
    Fl_Tick tick = state ? state->tick : 0;
    if (p->text_scene == scene->id && p->text_revision == scene->revision && p->text_generation == generation && p->text_tick == tick && p->text_scale == scale) return;
    bool rebuild = p->text_generation != generation || p->text_scale != scale;
    if (p->text_generation != generation) fl_text_rasterizer_flush(p->rasterizer);
    size_t count = 0;
    for (size_t i = 0; i < scene->entity_count; ++i) if (fl_entity_component(&scene->entities[i], FL_COMPONENT_TEXT)) ++count;
    if (count > SIZE_MAX / sizeof(Fl_CachedText)) return;
    Fl_CachedText *next = count ? calloc(count, sizeof(*next)) : NULL;
    if (count && !next) return;
    size_t at = 0;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        const Fl_Entity *e = &scene->entities[i]; const Fl_Component *text = fl_entity_component(e, FL_COMPONENT_TEXT);
        if (!text) continue;
        Fl_Transform t; Fl_CachedText *entry = &next[at++]; entry->entity = e->id;
        if (state) t = state->entities[i].transform;
        else if (!fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &t)) continue;
        entry->length = text->size; entry->width = t.width; entry->height = t.height;
        entry->font_size = state ? state->entities[i].appearance.font_size : fl_widget_appearance(e).font_size;
        entry->typography = fl_widget_typography(e);
        entry->text = text->size ? malloc(text->size) : NULL;
        if (text->size && !entry->text) continue;
        if (text->size) memcpy(entry->text, text->data, text->size);
        for (size_t j = 0; !rebuild && j < p->text_count; ++j) {
            Fl_CachedText *old = &p->texts[j];
            if (old->entity == e->id && p->text_scene == scene->id && old->width == entry->width && old->height == entry->height &&
                old->font_size == entry->font_size && same_typography(&old->typography, &entry->typography) && old->length == entry->length &&
                (!text->size || (old->text && !memcmp(old->text, text->data, text->size)))) {
                entry->image = old->image; old->image = NULL; break;
            }
        }
        if (!entry->image && t.width > 0 && t.height > 0) {
            entry->image = rasterize_text(p, entry, text->data, scale);
            if (!entry->image) FL_LOG_ERROR(LOG_TAG, "could not rasterize text entity %llu", (unsigned long long)e->id);
        }
    }
    bool retired = false;
    for (size_t i = 0; i < p->text_count; ++i) retired |= p->texts[i].image != NULL;
    if (retired) vkDeviceWaitIdle(p->device);
    for (size_t i = 0; i < p->text_count; ++i) {fl_gpu_image_destroy(p->texts[i].image); free(p->texts[i].text);}
    free(p->texts); p->texts = next; p->text_count = count; p->text_scene = scene->id; p->text_revision = scene->revision;
    p->text_generation = generation; p->text_tick = tick; p->text_scale = scale;
}

/* Worker body: decode one stored image; touches only its request. */
static void decode_image(void *data)
{
    Fl_ImageDecode *d = data;
    d->pixels = fl_image_decode(d->path, IMAGE_MAX_SIDE, &d->width, &d->height);
}

/* Release an entry's CPU/GPU storage; callers guarantee its decode finished and GPU reads completed. */
static void release_picture(Fl_CachedImage *entry)
{
    if (entry->decode) fl_image_free(entry->decode->pixels);
    free(entry->decode); fl_gpu_image_destroy(entry->image);
    *entry = (Fl_CachedImage){0};
}

static Fl_CachedImage *find_picture(Fl_SceneRenderer *p, Fl_Id asset)
{
    for (size_t i = 0; i < p->picture_count; ++i) if (p->pictures[i].asset == asset && !p->pictures[i].orphaned) return &p->pictures[i];
    return NULL;
}

/* Queue a decode for asset; failures leave a FAILED entry so the placeholder is shown without retry loops. */
static void request_picture(Fl_SceneRenderer *p, const Fl_Asset *asset)
{
    if (p->picture_count == p->picture_capacity) {
        size_t capacity = p->picture_capacity ? p->picture_capacity * 2 : 8;
        Fl_CachedImage *grown = realloc(p->pictures, capacity * sizeof(*grown));
        if (!grown) return;
        p->pictures = grown; p->picture_capacity = capacity;
    }
    Fl_CachedImage *entry = &p->pictures[p->picture_count++];
    *entry = (Fl_CachedImage){.asset = asset->id, .state = IMAGE_FAILED};
    entry->decode = calloc(1, sizeof(*entry->decode));
    if (!entry->decode || !fl_project_asset_path(p->project, asset, entry->decode->path, sizeof(entry->decode->path))) return;
    entry->job = fl_job_submit(p->jobs, &(Fl_JobDesc){.fn = decode_image, .data = entry->decode, .priority = FL_JOB_PRIORITY_NORMAL, .notify = p->bus != NULL});
    if (entry->job.generation) entry->state = IMAGE_DECODING;
}

/* Drop pictures the scene no longer references; in-flight decodes are orphaned until they finish. */
static void prepare_pictures(Fl_SceneRenderer *p, const Fl_Scene *scene)
{
    if (p->picture_scene == scene->id && p->picture_revision == scene->revision) return;
    bool waited = false;
    for (size_t i = 0; i < p->picture_count;) {
        Fl_CachedImage *entry = &p->pictures[i];
        bool used = false;
        for (size_t j = 0; !entry->orphaned && !used && j < scene->entity_count; ++j) {
            Fl_Id asset;
            used = fl_image_asset_decode(fl_entity_component(&scene->entities[j], FL_COMPONENT_IMAGE), &asset) && asset == entry->asset &&
                   fl_project_asset(p->project, asset);
        }
        if (used) {++i; continue;}
        if (entry->state == IMAGE_DECODING) {entry->orphaned = true; ++i; continue;}
        if (entry->image && !waited) {vkDeviceWaitIdle(p->device); waited = true;}
        release_picture(entry);
        p->pictures[i] = p->pictures[--p->picture_count];
    }
    p->picture_scene = scene->id; p->picture_revision = scene->revision;
}

/* Request missing decodes (waiting for them when asked) and upload finished ones for every image entity. */
static void stage_pictures(Fl_SceneRenderer *p, const Fl_Scene *scene, bool wait)
{
    for (size_t i = 0; i < scene->entity_count; ++i) {
        Fl_Id id; const Fl_Asset *asset;
        if (!fl_image_asset_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_IMAGE), &id) || !(asset = fl_project_asset(p->project, id))) continue;
        Fl_CachedImage *entry = find_picture(p, id);
        if (!entry) {request_picture(p, asset); entry = find_picture(p, id);}
        if (!entry) continue;
        if (wait && entry->state == IMAGE_DECODING) {fl_job_wait(p->jobs, entry->job); entry->state = IMAGE_DECODED;}
        if (entry->state != IMAGE_DECODED) continue;
        entry->image = entry->decode->pixels ? fl_gpu_image_create(p->images, FL_GPU_IMAGE_SRGBA, entry->decode->pixels,
                                                                   entry->decode->width, entry->decode->height) : NULL;
        fl_image_free(entry->decode->pixels); free(entry->decode); entry->decode = NULL;
        entry->state = entry->image ? IMAGE_READY : IMAGE_FAILED;
        if (!entry->image) FL_LOG_ERROR(LOG_TAG, "could not load image asset %s", asset->name);
    }
}

static VkDescriptorSet picture_descriptor(Fl_SceneRenderer *p, const Fl_Entity *e)
{
    Fl_Id id;
    if (!fl_image_asset_decode(fl_entity_component(e, FL_COMPONENT_IMAGE), &id)) return VK_NULL_HANDLE;
    const Fl_CachedImage *entry = find_picture(p, id);
    return entry && entry->state == IMAGE_READY ? fl_gpu_image_descriptor(entry->image) : VK_NULL_HANDLE;
}

/* Observe decode completion; orphaned entries are freed, others upload at the next prepare. */
static bool job_done(const Fl_Event *event, void *data)
{
    Fl_SceneRenderer *p = data; const Fl_JobDoneEvent *done = event->payload;
    for (size_t i = 0; i < p->picture_count; ++i) {
        Fl_CachedImage *entry = &p->pictures[i];
        if (entry->state != IMAGE_DECODING || entry->job.index != done->job.index || entry->job.generation != done->job.generation) continue;
        if (entry->orphaned) {release_picture(entry); p->pictures[i] = p->pictures[--p->picture_count];}
        else {entry->state = IMAGE_DECODED; if (p->invalidate) p->invalidate(p->invalidate_data);}
        break;
    }
    return false;
}

void fl_scene_renderer_prepare(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene, const Fl_SceneState *state, double text_scale, bool wait)
{
    if (!p || !project || !scene) return;
    p->project = project;
    prepare_texts(p, scene, state, text_scale > 0 ? text_scale : 1);
    prepare_pictures(p, scene);
    stage_pictures(p, scene, wait);
}

/* Pipeline for format, built on first use; VK_NULL_HANDLE when it cannot be created. */
static VkPipeline pipeline_for(Fl_SceneRenderer *p, VkFormat format)
{
    for (size_t i = 0; i < p->pipeline_count; ++i) if (p->pipelines[i].format == format) return p->pipelines[i].pipeline;
    if (p->pipeline_count == PIPELINE_FORMATS || !create_pipeline(p, format)) return VK_NULL_HANDLE;
    return p->pipelines[p->pipeline_count - 1].pipeline;
}

bool fl_scene_renderer_begin(Fl_SceneRenderer *p, VkCommandBuffer cmd, VkFormat format, uint32_t width, uint32_t height)
{
    if (!p || !cmd) return false;
    VkPipeline pipeline = pipeline_for(p, format);
    if (!pipeline) return false;
    VkRect2D area = {{0, 0}, {width, height}};
    VkViewport view = {0, 0, (float)width, (float)height, 0, 1};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); vkCmdSetViewport(cmd, 0, 1, &view); vkCmdSetScissor(cmd, 0, 1, &area);
    return true;
}

void fl_scene_renderer_push(Fl_SceneRenderer *p, VkCommandBuffer cmd, const float push[20], VkDescriptorSet mask)
{
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &mask, 0, NULL);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

VkDescriptorSet fl_scene_renderer_white(const Fl_SceneRenderer *p) { return p ? fl_gpu_images_white(p->images) : VK_NULL_HANDLE; }

void fl_scene_renderer_primitive(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Affine matrix,
                                 double width, double height, Fl_WidgetKind kind, Fl_Appearance appearance, VkDescriptorSet mask)
{
    const double *a = matrix.v;
    float push[20] = {(float)(a[0] * width * 2 / frame->width), (float)(a[1] * height * 2 / frame->width), (float)((a[2] - frame->x) * 2 / frame->width - 1), 0,
        (float)(a[3] * width * 2 / frame->height), (float)(a[4] * height * 2 / frame->height), (float)((a[5] - frame->y) * 2 / frame->height - 1), 0,
        fl_scene_renderer_linear(appearance.fill[0]), fl_scene_renderer_linear(appearance.fill[1]), fl_scene_renderer_linear(appearance.fill[2]), appearance.fill[3] / 255.f,
        fl_scene_renderer_linear(appearance.stroke[0]), fl_scene_renderer_linear(appearance.stroke[1]), fl_scene_renderer_linear(appearance.stroke[2]), appearance.stroke[3] / 255.f,
        (float)width, (float)height, (float)appearance.stroke_width, (float)kind};
    for (size_t i = 0; i < 20; ++i) if (!isfinite(push[i])) return;
    fl_scene_renderer_push(p, cmd, push, mask);
}

void fl_scene_renderer_background(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_SceneFrame *target)
{
    Fl_Affine background = fl_affine_identity(); background.v[2] = target->x; background.v[5] = target->y;
    Fl_Appearance board = {.fill = FL_RENDER_BACKGROUND_RGBA};
    fl_scene_renderer_primitive(p, cmd, frame, background, target->width, target->height, FL_WIDGET_RECTANGLE, board, fl_scene_renderer_white(p));
}

void fl_scene_renderer_entities(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_Scene *scene,
                                const Fl_SceneState *state, const Fl_SceneGeometry *g)
{
    VkDescriptorSet white = fl_scene_renderer_white(p);
    const Fl_Appearance placeholder = {.fill = FL_RENDER_PLACEHOLDER_FILL_RGBA, .stroke = FL_RENDER_PLACEHOLDER_STROKE_RGBA, .stroke_width = FL_RENDER_PLACEHOLDER_STROKE};
    for (size_t i = 0; i < g->count; ++i) {
        size_t index = g->order[i]; const Fl_ResolvedEntity *v = &g->entities[index]; const Fl_Entity *e = &scene->entities[index];
        if (!v->visible || v->kind == FL_WIDGET_GROUP || v->opacity <= 0) continue;
        Fl_Appearance look = state ? state->entities[index].appearance : fl_widget_appearance(e);
        look.fill[3] = (unsigned char)lround(look.fill[3] * v->opacity);
        look.stroke[3] = (unsigned char)lround(look.stroke[3] * v->opacity);
        Fl_Appearance picture = {.fill = {255, 255, 255, (unsigned char)lround(255 * v->opacity)}};
        VkDescriptorSet mask = white;
        if (v->kind == FL_WIDGET_TEXT) {
            mask = VK_NULL_HANDLE;
            if (p->text_scene == scene->id) for (size_t j = 0; j < p->text_count; ++j)
                if (p->texts[j].entity == e->id) mask = fl_gpu_image_descriptor(p->texts[j].image);
        } else if (v->kind == FL_WIDGET_IMAGE) {
            mask = picture_descriptor(p, e);
            if (mask) fl_scene_renderer_primitive(p, cmd, frame, v->world, v->local.width, v->local.height, (Fl_WidgetKind)KIND_IMAGE, picture, mask);
            else fl_scene_renderer_primitive(p, cmd, frame, v->world, v->local.width, v->local.height, FL_WIDGET_RECTANGLE, placeholder, white);
            continue;
        }
        if (mask) fl_scene_renderer_primitive(p, cmd, frame, v->world, v->local.width, v->local.height, v->kind, look, mask);
    }
}

Fl_SceneRenderer *fl_scene_renderer_create(const Fl_SceneRendererDesc *d)
{
    if (!d || !d->instance || !d->fonts || !d->jobs) return NULL;
    Fl_SceneRenderer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->instance = d->instance; p->fonts = d->fonts; p->jobs = d->jobs; p->bus = d->bus;
    p->invalidate = d->invalidate; p->invalidate_data = d->invalidate_data;
    p->device = ca_gpu_device(d->instance);
    p->images = fl_gpu_images_create(d->instance);
    p->rasterizer = fl_text_rasterizer_create();
    if (!p->images || !p->rasterizer || (d->bus && !fl_event_subscribe(d->bus, &(Fl_SubscribeDesc){.type = fl_jobs_done_event(d->jobs),
        .fn = job_done, .user_data = p, .owner = p}).generation)) {
        fl_text_rasterizer_destroy(p->rasterizer); fl_gpu_images_destroy(p->images); free(p); return NULL;
    }
    return p;
}

void fl_scene_renderer_destroy(Fl_SceneRenderer *p)
{
    if (!p) return;
    if (p->bus) fl_event_unsubscribe_owner(p->bus, p);
    for (size_t i = 0; i < p->picture_count; ++i) if (p->pictures[i].state == IMAGE_DECODING) fl_job_wait(p->jobs, p->pictures[i].job);
    vkDeviceWaitIdle(p->device);
    for (size_t i = 0; i < p->picture_count; ++i) release_picture(&p->pictures[i]);
    for (size_t i = 0; i < p->text_count; ++i) {fl_gpu_image_destroy(p->texts[i].image); free(p->texts[i].text);}
    free(p->pictures); free(p->texts);
    for (size_t i = 0; i < p->pipeline_count; ++i) vkDestroyPipeline(p->device, p->pipelines[i].pipeline, NULL);
    if (p->layout) vkDestroyPipelineLayout(p->device, p->layout, NULL);
    fl_gpu_images_destroy(p->images); fl_text_rasterizer_destroy(p->rasterizer); free(p);
}

/* Offscreen target: a colour image and a host-readable buffer for the result. */
typedef struct Offscreen {
    VkImage image; VmaAllocation image_memory; VkImageView view;
    VkBuffer buffer; VmaAllocation buffer_memory; VmaAllocationInfo buffer_info;
} Offscreen;

static void offscreen_destroy(VmaAllocator allocator, VkDevice device, Offscreen *o)
{
    if (o->view) vkDestroyImageView(device, o->view, NULL);
    if (o->image) vmaDestroyImage(allocator, o->image, o->image_memory);
    if (o->buffer) vmaDestroyBuffer(allocator, o->buffer, o->buffer_memory);
    *o = (Offscreen){0};
}

/* Image layout transition covering the whole colour image. */
static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

bool fl_scene_renderer_render(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene, Fl_Tick tick, uint32_t width, Fl_Frame *out)
{
    uint32_t w, h;
    if (!p || !project || !scene || !out || tick < 0 || !fl_frame_dimensions(scene, width, &w, &h)) return false;
    *out = (Fl_Frame){0};
    Fl_SceneState state = {0};
    Fl_SceneGeometry geometry = {0};
    Fl_SceneFrame frame = fl_scene_frame(scene);
    bool ok = fl_scene_state_update(&state, scene, tick) && fl_scene_geometry_update(&geometry, scene, &state);
    VmaAllocator allocator = ca_gpu_allocator(p->instance);
    Offscreen o = {0};
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    if (ok) {
        fl_scene_renderer_prepare(p, project, scene, &state, (double)w / frame.width, true);
        VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo device_memory = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo host = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        ok = vmaCreateImage(allocator, &image, &device_memory, &o.image, &o.image_memory, NULL) == VK_SUCCESS &&
             vmaCreateBuffer(allocator, &buffer, &host, &o.buffer, &o.buffer_memory, &o.buffer_info) == VK_SUCCESS;
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        ok = ok && vkCreateImageView(p->device, &view, NULL, &o.view) == VK_SUCCESS;
    }
    ok = ok && pipeline_for(p, format) != VK_NULL_HANDLE;
    VkCommandBuffer cmd = ok ? ca_gpu_begin_transfer(p->instance) : VK_NULL_HANDLE;
    if (ok && cmd) {
        transition(cmd, o.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        const unsigned char backdrop[4] = FL_RENDER_BACKGROUND_RGBA;
        VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = o.view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color = {.float32 = {fl_scene_renderer_linear(backdrop[0]), fl_scene_renderer_linear(backdrop[1]), fl_scene_renderer_linear(backdrop[2]), 1}}};
        VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {w, h}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
        vkCmdBeginRendering(cmd, &info);
        ok = fl_scene_renderer_begin(p, cmd, format, w, h);
        if (ok) {
            fl_scene_renderer_background(p, cmd, &frame, &frame);
            fl_scene_renderer_entities(p, cmd, &frame, scene, &state, &geometry);
        }
        vkCmdEndRendering(cmd);
        transition(cmd, o.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.buffer, 1, &copy);
        VkBufferMemoryBarrier host_read = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = o.buffer, .size = VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host_read, 0, NULL);
        ca_gpu_end_transfer(p->instance, cmd);
        ok = ok && vmaInvalidateAllocation(allocator, o.buffer_memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS;
    } else ok = false;
    if (ok) {
        out->rgba = malloc((size_t)w * h * 4);
        if (out->rgba) {memcpy(out->rgba, o.buffer_info.pMappedData, (size_t)w * h * 4); out->width = w; out->height = h;}
        ok = out->rgba != NULL;
    }
    offscreen_destroy(allocator, p->device, &o);
    fl_scene_state_clear(&state); fl_scene_geometry_clear(&geometry);
    return ok;
}
