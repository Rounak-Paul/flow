#include "fl_scene_renderer.h"
#include "fl_gpu_image.h"
#include "core/fl_log.h"
#include "components/fl_style.h"
#include "components/fl_video_clip.h"
#include "media/fl_media.h"
#include "media/fl_video_cache.h"
#include "platform/fl_paths.h"
#include "text/fl_text_raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "renderer"
#define KIND_IMAGE 6
#define IMAGE_MAX_SIDE 8192u
#define PIPELINE_FORMATS 4u
#define LAYER_SLOTS 4u
#define LAYER_DEPTH 3u
#define LAYER_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define KIND_COMPOSITE 17
#define KIND_BLUR 18
#define MAX_BLUR_TAPS 96

typedef struct Fl_CachedText {
    Fl_Id entity;
    unsigned char *text;
    size_t length;
    double width, height, font_size;
    Fl_Typography typography;
    int align;
    Fl_GpuImage *image;
} Fl_CachedText;
/* Current frame of one video entity, uploaded as a picture. Replaced images wait in the graveyard until the GPU is done. */
typedef struct Fl_VideoFrame {
    Fl_Id entity;
    char path[FL_PATH_MAX];
    Fl_GpuImage *image;
    bool used;
} Fl_VideoFrame;
typedef struct Fl_Retired { Fl_GpuImage *image; unsigned age; } Fl_Retired;
#define GRAVEYARD_FRAMES 4u
/* Decode request owned by its image entry until the worker finishes. */
typedef struct Fl_ImageDecode {
    char path[FL_PATH_MAX];
    unsigned char *pixels;
    uint32_t width, height;
} Fl_ImageDecode;
typedef enum Fl_ImageState { IMAGE_DECODING, IMAGE_DECODED, IMAGE_READY, IMAGE_FAILED } Fl_ImageState;
typedef struct Fl_CachedImage {
    Fl_Id asset;
    unsigned char hash[32];
    Fl_ImageState state;
    Fl_JobHandle job;
    Fl_ImageDecode *decode;
    Fl_GpuImage *image;
    bool orphaned;              /* no longer referenced; freed once its decode finishes */
} Fl_CachedImage;
typedef struct Fl_Pipeline { VkFormat format; VkPipeline pipeline; } Fl_Pipeline;

struct Fl_SceneRenderer {
    Fl_GpuContext gpu;
    Fl_Fonts *fonts;
    Fl_JobSystem *jobs;
    Fl_EventBus *bus;
    void (*invalidate)(void *user_data);
    void *invalidate_data;
    uint64_t project_identity, render_project_identity;
    Fl_SceneState render_states[2];
    unsigned render_last;
    char error[256];
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
    Fl_GpuImage *layers[LAYER_SLOTS][LAYER_DEPTH], *scratch[LAYER_SLOTS];
    uint32_t layer_width, layer_height;
    Fl_VideoFrame *videos;
    size_t video_count, video_capacity;
    Fl_Retired *graveyard;
    size_t graveyard_count, graveyard_capacity;
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

/* GLSL declaring every polygon outline of the widget registry (as vert/vertex_count), so the shader never restates a shape. */
static void polygon_glsl(char *out, size_t size)
{
    char arrays[1536] = "", vert[256] = "vec2 vert(int k,float kind){", count[256] = "int vertex_count(float kind){";
    for (int kind = FL_WIDGET_TRIANGLE; kind < FL_WIDGET_COUNT; ++kind) {
        double v[FL_POLYGON_MAX_VERTICES][2];
        const size_t n = fl_widget_polygon((Fl_WidgetKind)kind, v);
        size_t used = strlen(arrays);
        used += (size_t)snprintf(arrays + used, sizeof(arrays) - used, "const vec2 outline%d[%zu]=vec2[%zu](", kind, n, n);
        for (size_t i = 0; i < n; ++i) used += (size_t)snprintf(arrays + used, sizeof(arrays) - used, "%svec2(%.9g,%.9g)", i ? "," : "", v[i][0], v[i][1]);
        snprintf(arrays + used, sizeof(arrays) - used, ");");
        size_t at = strlen(vert);
        snprintf(vert + at, sizeof(vert) - at, "if(kind==%d.)return outline%d[k];", kind, kind);
        at = strlen(count);
        snprintf(count + at, sizeof(count) - at, "if(kind==%d.)return %zu;", kind, n);
    }
    snprintf(out, size, "%s%sreturn vec2(0.);}%sreturn 0;}", arrays, vert, count);
}

/* Compile the primitive shaders and build one pipeline for a colour format. */
static bool create_pipeline(Fl_SceneRenderer *p, VkFormat format)
{
    const char *vs = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; vec4 fill2; vec4 extra; } p;"
        "layout(location=0) out vec2 uv;vec2 v[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));"
        "void main(){uv=v[gl_VertexIndex];vec3 q=vec3(uv,1);gl_Position=vec4(dot(p.a.xyz,q),dot(p.b.xyz,q),0,1);}";
    static const char fs_head[] = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; vec4 fill2; vec4 extra; } p;"
        "layout(set=0,binding=0) uniform sampler2D mask;layout(location=0) in vec2 uv;layout(location=0) out vec4 color;"
        "float rbox(vec2 q0,vec2 half_size,float r){vec2 q=abs(q0)-half_size+r;return length(max(q,0.))+min(max(q.x,q.y),0.)-r;}";
    static const char fs_tail[] =
        "float poly(vec2 p,float kind){int n=vertex_count(kind);vec2 v0=vert(0,kind);float d=dot(p-v0,p-v0);float s=1.;"
        "for(int i=0,j=n-1;i<n;j=i++){vec2 vi=vert(i,kind),vj=vert(j,kind);vec2 e=vj-vi,w=p-vi;vec2 b=w-e*clamp(dot(w,e)/dot(e,e),0.,1.);d=min(d,dot(b,b));"
        "bvec3 c=bvec3(p.y>=vi.y,p.y<vj.y,e.x*w.y>e.y*w.x);if(all(c)||all(not(c)))s=-s;}return s*sqrt(d);}"
        "void main(){float kind=p.params.w;vec2 size=p.params.xy;float sw=p.params.z;float r=min(p.extra.x,min(size.x,size.y)*.5);"
        "if(kind==15.){vec2 world=uv*vec2(p.a.w,p.b.w)+size;vec2 cell=world/sw;"
        "vec2 d=abs(fract(cell-.5)-.5)/max(fwidth(cell),vec2(.0001));"
        "vec2 big=cell/5;vec2 b=abs(fract(big-.5)-.5)/max(fwidth(big),vec2(.0001));"
        "float minor=(1-min(min(d.x,d.y),1))*smoothstep(2,6,1/max(fwidth(cell.x),fwidth(cell.y)));"
        "float major=(1-min(min(b.x,b.y),1))*smoothstep(2,6,1/max(fwidth(big.x),fwidth(big.y)));"
        "color=mix(p.fill,p.stroke,major);color.a*=max(minor,major);return;}"
        "if(kind==17.){vec2 uv0=gl_FragCoord.xy/size;vec4 c=texture(mask,uv0);float cover=1.;"
        "if(p.fill2.y>.5){vec2 px=gl_FragCoord.xy;vec2 l=vec2(p.fill.x*px.x+p.fill.y*px.y+p.fill.z,p.stroke.x*px.x+p.stroke.y*px.y+p.stroke.z);"
        "vec2 box=vec2(p.fill.w,p.stroke.w);float d;if(p.fill2.z>.5){vec2 q=(l-box*.5)/(box*.5);d=(length(q)-1.)*min(box.x,box.y)*.5;}"
        "else d=rbox(l-box*.5,box*.5,min(p.fill2.x,min(box.x,box.y)*.5));float w=max(fwidth(d),.001);cover=1.-smoothstep(-w,w,d);}"
        "color=vec4(c.a>1e-5?c.rgb/c.a:vec3(0.),c.a*cover);return;}"
        "if(kind==18.){float sigma=max(p.extra.x,.01);vec2 dir=p.extra.yz;vec2 uv0=gl_FragCoord.xy/size;int n=int(min(ceil(sigma*3.),96.));"
        "vec4 sum=vec4(0.);float total=0.;for(int i=-n;i<=n;++i){float w=exp(-.5*float(i*i)/(sigma*sigma));sum+=texture(mask,uv0+dir*float(i)/size)*w;total+=w;}"
        "sum/=total;color=vec4(sum.a>1e-5?sum.rgb/sum.a:vec3(0.),sum.a);return;}"
        "if(kind==16.){float m=p.extra.w;float d=rbox((uv-.5)*size,size*.5-m,r);color=p.fill;color.a*=1-smoothstep(-sw,sw,d);return;}"
        "if(kind==5.){color=p.fill;color.a*=texture(mask,uv).r;return;}"
        "if(kind==6.){color=texture(mask,p.fill2.xy+uv*p.fill2.zw);color.a*=p.fill.a;"
        "if(r>0.){float e=rbox((uv-.5)*size,size*.5,r);float w=max(fwidth(e),.001);color.a*=1-smoothstep(-w,w,e);}return;}"
        "if(kind==3.||kind==4.){float shaft=abs((uv.y-.5)*size.y)-sw*.5;"
        "float head=max(.72-uv.x,abs(uv.y-.5)-(1-uv.x)/.28*.5);"
        "float cover=1-smoothstep(-fwidth(shaft),fwidth(shaft),shaft);"
        "if(kind==4.){cover*=1-smoothstep(.79,.8,uv.x);cover=max(cover,1-smoothstep(-fwidth(head),fwidth(head),head));}color=p.stroke;color.a*=cover;return;}"
        "float edge;if(kind==2.){vec2 q=(uv-.5)*2;edge=(length(q)-1)*min(size.x,size.y)*.5;}"
        "else if(kind>=8.){edge=poly((uv-.5)*2,kind)*min(size.x,size.y)*.5;}"
        "else edge=rbox((uv-.5)*size,size*.5,r);"
        "float aa=max(fwidth(edge),.001);float cover=1-smoothstep(-aa,aa,edge);"
        "float border=sw>0 ? smoothstep(-sw-aa,-sw+aa,edge) : 0;"
        "vec4 base=p.fill;"
        "if(p.extra.z>.5){vec2 dir=vec2(cos(p.extra.y),sin(p.extra.y));float t=dot((uv-.5)*size,dir)/max(abs(size.x*dir.x)+abs(size.y*dir.y),.0001)+.5;"
        "base=mix(p.fill,p.fill2,clamp(t,0.,1.));float n=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453)-.5;base.rgb+=n*.004;}"
        "color=mix(base,p.stroke,border);color.a*=cover;}";
    char outlines[2048], fs[sizeof(outlines) + sizeof(fs_head) + sizeof(fs_tail)];
    polygon_glsl(outlines, sizeof(outlines));
    snprintf(fs, sizeof(fs), "%s%s%s", fs_head, outlines, fs_tail);
    VkShaderModule vert = p->gpu.compile_shader(p->gpu.host,vs,VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = p->gpu.compile_shader(p->gpu.host,fs,VK_SHADER_STAGE_FRAGMENT_BIT);
    bool ok = vert && frag;
    if (ok && !p->layout) {
        VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
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
    Fl_TextStyle style = {match.face, entry->font_size * scale, match.synthetic_bold, match.synthetic_italic, entry->align};
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
        const Fl_Entity *e = &scene->entities[i]; const Fl_Component *component = fl_entity_component(e, FL_COMPONENT_TEXT);
        if (!component) continue;
        bool overridden = state && state->entities[i].text_override;
        const Fl_Component text_view = overridden ? (Fl_Component){.type = FL_COMPONENT_TEXT, .version = 1, .size = state->entities[i].text_size, .data = (const unsigned char *)state->entities[i].text} : *component;
        const Fl_Component *text = &text_view;
        Fl_Transform t; Fl_CachedText *entry = &next[at++]; entry->entity = e->id;
        if (state) t = state->entities[i].transform;
        else if (!fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &t)) continue;
        entry->length = text->size; entry->width = t.width; entry->height = t.height;
        entry->font_size = state ? state->entities[i].appearance.font_size : fl_widget_appearance(e).font_size;
        entry->typography = fl_widget_typography(e);
        entry->align = (int)fl_entity_style(e).align;
        entry->text = text->size ? malloc(text->size) : NULL;
        if (text->size && !entry->text) continue;
        if (text->size) memcpy(entry->text, text->data, text->size);
        for (size_t j = 0; !rebuild && j < p->text_count; ++j) {
            Fl_CachedText *old = &p->texts[j];
            if (old->entity == e->id && p->text_scene == scene->id && old->width == entry->width && old->height == entry->height &&
                old->font_size == entry->font_size && old->align == entry->align && same_typography(&old->typography, &entry->typography) && old->length == entry->length &&
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
static void request_picture(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Asset *asset)
{
    if (p->picture_count == p->picture_capacity) {
        size_t capacity = p->picture_capacity ? p->picture_capacity * 2 : 8;
        Fl_CachedImage *grown = realloc(p->pictures, capacity * sizeof(*grown));
        if (!grown) return;
        p->pictures = grown; p->picture_capacity = capacity;
    }
    Fl_CachedImage *entry = &p->pictures[p->picture_count++];
    *entry = (Fl_CachedImage){.asset = asset->id, .state = IMAGE_FAILED};
    memcpy(entry->hash,asset->hash,sizeof(entry->hash));
    entry->decode = calloc(1, sizeof(*entry->decode));
    if (!entry->decode || !fl_project_asset_path(project, asset, entry->decode->path, sizeof(entry->decode->path))) return;
    entry->job = fl_job_submit(p->jobs, &(Fl_JobDesc){.fn = decode_image, .data = entry->decode, .priority = FL_JOB_PRIORITY_NORMAL, .notify = p->bus != NULL});
    if (entry->job.generation) entry->state = IMAGE_DECODING;
}

/* Drop pictures the scene no longer references; in-flight decodes are orphaned until they finish. */
static void prepare_pictures(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene)
{
    if (p->picture_scene == scene->id && p->picture_revision == scene->revision) return;
    bool waited = false;
    for (size_t i = 0; i < p->picture_count;) {
        Fl_CachedImage *entry = &p->pictures[i];
        bool used = false;
        for (size_t j = 0; !entry->orphaned && !used && j < scene->entity_count; ++j) {
            Fl_Id asset;
            const Fl_Asset *resource=NULL;
            used = fl_image_asset_decode(fl_entity_component(&scene->entities[j], FL_COMPONENT_IMAGE), &asset) && asset == entry->asset &&
                   (resource=fl_project_asset(project,asset)) && !memcmp(entry->hash,resource->hash,sizeof(entry->hash));
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
static void stage_pictures(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene, bool wait)
{
    for (size_t i = 0; i < scene->entity_count; ++i) {
        Fl_Id id; const Fl_Asset *asset;
        if (!fl_image_asset_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_IMAGE), &id) || !(asset = fl_project_asset(project, id))) continue;
        Fl_CachedImage *entry = find_picture(p, id);
        if (!entry) {request_picture(p, project, asset); entry = find_picture(p, id);}
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

/* Keep a replaced image alive until the frames that may still sample it have finished. */
static void retire_image(Fl_SceneRenderer *p, Fl_GpuImage *image)
{
    if (!image) return;
    if (p->graveyard_count == p->graveyard_capacity) {
        size_t capacity = p->graveyard_capacity ? p->graveyard_capacity * 2 : 8;
        Fl_Retired *grown = realloc(p->graveyard, capacity * sizeof(*grown));
        if (!grown) {vkDeviceWaitIdle(p->device); fl_gpu_image_destroy(image); return;}
        p->graveyard = grown; p->graveyard_capacity = capacity;
    }
    p->graveyard[p->graveyard_count++] = (Fl_Retired){image, 0};
}

static void age_graveyard(Fl_SceneRenderer *p)
{
    size_t kept = 0;
    for (size_t i = 0; i < p->graveyard_count; ++i) {
        if (++p->graveyard[i].age > GRAVEYARD_FRAMES) fl_gpu_image_destroy(p->graveyard[i].image);
        else p->graveyard[kept++] = p->graveyard[i];
    }
    p->graveyard_count = kept;
}

/* Upload the frame each video entity shows at tick, reusing the previous upload while the frame is unchanged. */
static void prepare_videos(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene, const Fl_SceneState *state, Fl_Tick tick)
{
    for (size_t i = 0; i < p->video_count; ++i) p->videos[i].used = false;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        const Fl_Entity *e = &scene->entities[i];
        Fl_VideoClip clip;
        const Fl_Asset *asset;
        char path[FL_PATH_MAX];
        if (!e->visible || (state && state->entities[i].opacity <= 0)) continue;
        if (!fl_video_clip_decode(fl_entity_component(e, FL_COMPONENT_VIDEO), &clip) || !(asset = fl_project_asset(project, clip.asset)) ||
            !fl_video_cache_frame(project, asset, fl_video_clip_source_seconds(&clip, tick, asset->duration), path, sizeof(path))) continue;
        Fl_VideoFrame *slot = NULL;
        for (size_t j = 0; j < p->video_count && !slot; ++j) if (p->videos[j].entity == e->id) slot = &p->videos[j];
        if (slot && slot->image && !strcmp(slot->path, path)) {slot->used = true; continue;}
        uint32_t w, h;
        unsigned char *pixels = fl_image_decode(path, 4096, &w, &h);
        Fl_GpuImage *image = pixels ? fl_gpu_image_create(p->images, FL_GPU_IMAGE_SRGBA, pixels, w, h) : NULL;
        fl_image_free(pixels);
        if (!image) {FL_LOG_ERROR(LOG_TAG, "could not load video frame %s", path); continue;}
        if (!slot) {
            if (p->video_count == p->video_capacity) {
                size_t capacity = p->video_capacity ? p->video_capacity * 2 : 4;
                Fl_VideoFrame *grown = realloc(p->videos, capacity * sizeof(*grown));
                if (!grown) {fl_gpu_image_destroy(image); continue;}
                p->videos = grown; p->video_capacity = capacity;
            }
            slot = &p->videos[p->video_count++]; *slot = (Fl_VideoFrame){.entity = e->id};
        }
        retire_image(p, slot->image);
        slot->image = image; slot->used = true; snprintf(slot->path, sizeof(slot->path), "%s", path);
    }
    size_t kept = 0;
    for (size_t i = 0; i < p->video_count; ++i) {
        if (p->videos[i].used) p->videos[kept++] = p->videos[i];
        else retire_image(p, p->videos[i].image);
    }
    p->video_count = kept;
}

static VkDescriptorSet video_descriptor(const Fl_SceneRenderer *p, Fl_Id entity)
{
    for (size_t i = 0; i < p->video_count; ++i) if (p->videos[i].entity == entity) return fl_gpu_image_descriptor(p->videos[i].image);
    return VK_NULL_HANDLE;
}

void fl_scene_renderer_prepare(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene, const Fl_SceneState *state, double text_scale, bool wait)
{
    if (!p || !project || !scene) return;
    uint64_t identity=fl_project_identity(project);
    if (p->project_identity!=identity) {p->text_scene=0; p->picture_scene=0;}
    p->project_identity=identity;
    prepare_texts(p, scene, state, text_scale > 0 ? text_scale : 1);
    prepare_pictures(p, project, scene);
    stage_pictures(p, project, scene, wait);
    age_graveyard(p);
    prepare_videos(p, project, scene, state, state ? state->tick : 0);
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

void fl_scene_renderer_push(Fl_SceneRenderer *p, VkCommandBuffer cmd, const float push[28], VkDescriptorSet mask)
{
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &mask, 0, NULL);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112, push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

VkDescriptorSet fl_scene_renderer_white(const Fl_SceneRenderer *p) { return p ? fl_gpu_images_white(p->images) : VK_NULL_HANDLE; }

/* Fill the push block: affine to clip space, linear-light colours, sizes, kind and style parameters. */
static void push_block(float push[28], const Fl_SceneFrame *frame, Fl_Affine matrix, double width, double height, Fl_WidgetKind kind,
                       Fl_Appearance appearance, const Fl_PrimitiveStyle *style)
{
    const double *a = matrix.v;
    const float base[20] = {(float)(a[0] * width * 2 / frame->width), (float)(a[1] * height * 2 / frame->width), (float)((a[2] - frame->x) * 2 / frame->width - 1), 0,
        (float)(a[3] * width * 2 / frame->height), (float)(a[4] * height * 2 / frame->height), (float)((a[5] - frame->y) * 2 / frame->height - 1), 0,
        fl_scene_renderer_linear(appearance.fill[0]), fl_scene_renderer_linear(appearance.fill[1]), fl_scene_renderer_linear(appearance.fill[2]), appearance.fill[3] / 255.f,
        fl_scene_renderer_linear(appearance.stroke[0]), fl_scene_renderer_linear(appearance.stroke[1]), fl_scene_renderer_linear(appearance.stroke[2]), appearance.stroke[3] / 255.f,
        (float)width, (float)height, (float)appearance.stroke_width, (float)kind};
    memcpy(push, base, sizeof(base));
    const unsigned char *to = style ? style->gradient_to : (const unsigned char[4]){0};
    push[20] = fl_scene_renderer_linear(to[0]); push[21] = fl_scene_renderer_linear(to[1]); push[22] = fl_scene_renderer_linear(to[2]); push[23] = to[3] / 255.f;
    if (kind == (Fl_WidgetKind)6) {
        const double *crop = style ? style->crop : (const double[4]){0, 0, 1, 1};
        push[20] = (float)crop[0]; push[21] = (float)crop[1]; push[22] = (float)crop[2]; push[23] = (float)crop[3];
    }
    push[24] = style ? (float)style->radius : 0; push[25] = style ? (float)style->gradient_angle : 0; push[26] = style && style->gradient ? 1.f : 0.f; push[27] = 0;
}

void fl_scene_renderer_styled(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Affine matrix,
                              double width, double height, Fl_WidgetKind kind, Fl_Appearance appearance, VkDescriptorSet mask,
                              const Fl_PrimitiveStyle *style)
{
    float push[28];
    push_block(push, frame, matrix, width, height, kind, appearance, style);
    for (size_t i = 0; i < 28; ++i) if (!isfinite(push[i])) return;
    fl_scene_renderer_push(p, cmd, push, mask);
}

void fl_scene_renderer_primitive(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Affine matrix,
                                 double width, double height, Fl_WidgetKind kind, Fl_Appearance appearance, VkDescriptorSet mask)
{
    fl_scene_renderer_styled(p, cmd, frame, matrix, width, height, kind, appearance, mask, NULL);
}

/* Soft drop shadow of a (rounded) box: a larger quad whose alpha falls off with the distance to the box. */
static void draw_shadow(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Affine matrix, double width, double height,
                        double radius, const Fl_Style *style, double opacity)
{
    double margin = style->shadow_blur + 2, shift_x = style->shadow_x - margin, shift_y = style->shadow_y - margin;
    matrix.v[2] += matrix.v[0] * shift_x + matrix.v[1] * shift_y;
    matrix.v[5] += matrix.v[3] * shift_x + matrix.v[4] * shift_y;
    Fl_Appearance color = {.stroke_width = style->shadow_blur};
    memcpy(color.fill, style->shadow_color, 4);
    color.fill[3] = (unsigned char)lround(color.fill[3] * opacity);
    float push[28];
    push_block(push, frame, matrix, width + 2 * margin, height + 2 * margin, (Fl_WidgetKind)16, color, NULL);
    push[24] = (float)fmin(radius, fmin(width, height) * .5); push[27] = (float)margin;
    for (size_t i = 0; i < 28; ++i) if (!isfinite(push[i])) return;
    fl_scene_renderer_push(p, cmd, push, fl_scene_renderer_white(p));
}

void fl_scene_renderer_background(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_SceneFrame *target, const unsigned char color[4])
{
    Fl_Affine background = fl_affine_identity(); background.v[2] = target->x; background.v[5] = target->y;
    Fl_Appearance board = {.fill = {color[0], color[1], color[2], color[3]}};
    fl_scene_renderer_primitive(p, cmd, frame, background, target->width, target->height, FL_WIDGET_RECTANGLE, board, fl_scene_renderer_white(p));
}

/* Which pixels of an entity to draw: everything, only its shadow (which a layer must not cut), or only its body. */
typedef enum Fl_EntityPart { PART_ALL, PART_SHADOW, PART_BODY } Fl_EntityPart;

/* Draw one entity's own pixels; children are drawn by the caller. */
static void draw_entity(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_Scene *scene,
                        const Fl_SceneState *state, const Fl_SceneGeometry *g, size_t index, Fl_EntityPart part)
{
    VkDescriptorSet white = fl_scene_renderer_white(p);
    const Fl_Appearance placeholder = {.fill = FL_RENDER_PLACEHOLDER_FILL_RGBA, .stroke = FL_RENDER_PLACEHOLDER_STROKE_RGBA, .stroke_width = FL_RENDER_PLACEHOLDER_STROKE};
    const Fl_ResolvedEntity *v = &g->entities[index]; const Fl_Entity *e = &scene->entities[index];
    if (!v->visible || v->kind == FL_WIDGET_GROUP || v->opacity <= 0) return;
    Fl_Appearance look = state ? state->entities[index].appearance : fl_widget_appearance(e);
    look.fill[3] = (unsigned char)lround(look.fill[3] * v->opacity);
    look.stroke[3] = (unsigned char)lround(look.stroke[3] * v->opacity);
    Fl_Appearance picture = {.fill = {255, 255, 255, (unsigned char)lround(255 * v->opacity)}};
    Fl_Style style = fl_entity_style(e);
    Fl_PrimitiveStyle shape = {.radius = style.radius, .gradient = style.gradient, .gradient_angle = style.gradient_angle};
    memcpy(shape.gradient_to, style.gradient_to, 4);
    shape.gradient_to[3] = (unsigned char)lround(style.gradient_to[3] * v->opacity);
    memcpy(shape.crop, state ? state->entities[index].crop : style.crop, sizeof(shape.crop));
    bool boxed = v->kind == FL_WIDGET_RECTANGLE || v->kind == FL_WIDGET_ELLIPSE || v->kind == FL_WIDGET_IMAGE || v->kind == FL_WIDGET_VIDEO;
    if (part != PART_BODY && boxed && style.shadow_blur > 0 && style.shadow_color[3])
        draw_shadow(p, cmd, frame, v->world, v->local.width, v->local.height, v->kind == FL_WIDGET_ELLIPSE ? 1e9 : style.radius, &style, v->opacity);
    if (part == PART_SHADOW) return;
    VkDescriptorSet mask = white;
    if (v->kind == FL_WIDGET_TEXT) {
        mask = VK_NULL_HANDLE;
        if (p->text_scene == scene->id) for (size_t j = 0; j < p->text_count; ++j)
            if (p->texts[j].entity == e->id) mask = fl_gpu_image_descriptor(p->texts[j].image);
    } else if (v->kind == FL_WIDGET_IMAGE || v->kind == FL_WIDGET_VIDEO) {
        mask = v->kind == FL_WIDGET_IMAGE ? picture_descriptor(p, e) : video_descriptor(p, e->id);
        if (mask) fl_scene_renderer_styled(p, cmd, frame, v->world, v->local.width, v->local.height, (Fl_WidgetKind)KIND_IMAGE, picture, mask, &shape);
        else fl_scene_renderer_styled(p, cmd, frame, v->world, v->local.width, v->local.height, FL_WIDGET_RECTANGLE, placeholder, white, &shape);
        return;
    }
    if (mask) fl_scene_renderer_styled(p, cmd, frame, v->world, v->local.width, v->local.height, v->kind, look, mask, boxed || fl_widget_is_polygon(v->kind) ? &shape : NULL);
}

/* Full-access barrier with a layout change; layers are small in number, so simplicity beats precise stages. */
static void layer_transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void begin_pass(VkCommandBuffer cmd, VkImageView view, uint32_t width, uint32_t height, bool clear)
{
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color = {.float32 = {0, 0, 0, 0}}};
    VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {width, height}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
    vkCmdBeginRendering(cmd, &info);
}

/* Layer images for a frame slot, recreated when the target size changes. */
static bool ensure_layers(Fl_SceneRenderer *p, const Fl_RenderTarget *t)
{
    if (p->layer_width != t->width || p->layer_height != t->height) {
        vkDeviceWaitIdle(p->device);
        for (size_t s = 0; s < LAYER_SLOTS; ++s) {
            for (size_t d = 0; d < LAYER_DEPTH; ++d) {fl_gpu_image_destroy(p->layers[s][d]); p->layers[s][d] = NULL;}
            fl_gpu_image_destroy(p->scratch[s]); p->scratch[s] = NULL;
        }
        p->layer_width = t->width; p->layer_height = t->height;
    }
    return true;
}

static Fl_GpuImage *layer_image(Fl_SceneRenderer *p, const Fl_RenderTarget *t, size_t level)
{
    Fl_GpuImage **slot = &p->layers[t->slot % LAYER_SLOTS][level];
    if (!*slot) *slot = fl_gpu_target_create(p->images, LAYER_FORMAT, t->width, t->height);
    return *slot;
}

static Fl_GpuImage *scratch_image(Fl_SceneRenderer *p, const Fl_RenderTarget *t)
{
    Fl_GpuImage **slot = &p->scratch[t->slot % LAYER_SLOTS];
    if (!*slot) *slot = fl_gpu_target_create(p->images, LAYER_FORMAT, t->width, t->height);
    return *slot;
}

/* Push block for a full-target pass over layer content (composite or one blur direction). */
static void layer_push(float push[28], const Fl_RenderTarget *t, int kind)
{
    memset(push, 0, 28 * sizeof(float));
    push[0] = 2; push[2] = -1; push[5] = 2; push[6] = -1;
    push[16] = (float)t->width; push[17] = (float)t->height; push[19] = (float)kind;
}

/* Blur layer in place with two separable passes through the scratch image. */
static bool blur_layer(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_RenderTarget *t, Fl_GpuImage *layer, double sigma)
{
    Fl_GpuImage *scratch = scratch_image(p, t);
    if (!scratch) return false;
    for (int pass = 0; pass < 2; ++pass) {
        Fl_GpuImage *from = pass ? scratch : layer, *to = pass ? layer : scratch;
        layer_transition(cmd, fl_gpu_image_handle(to), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        begin_pass(cmd, fl_gpu_image_view(to), t->width, t->height, true);
        float push[28];
        layer_push(push, t, KIND_BLUR);
        push[24] = (float)fmin(sigma, MAX_BLUR_TAPS / 3.0); push[25] = pass ? 0.f : 1.f; push[26] = pass ? 1.f : 0.f;
        bool ok = fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, t->width, t->height);
        if (ok) fl_scene_renderer_push(p, cmd, push, fl_gpu_image_descriptor(from));
        vkCmdEndRendering(cmd);
        layer_transition(cmd, fl_gpu_image_handle(to), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!ok) return false;
    }
    return true;
}

/* Resume rendering into the layer below (or the base target when level is zero). */
static bool resume(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_RenderTarget *t, size_t level)
{
    if (level == 0) {
        begin_pass(cmd, t->view, t->width, t->height, false);
        return fl_scene_renderer_begin(p, cmd, t->format, t->width, t->height);
    }
    Fl_GpuImage *below = layer_image(p, t, level - 1);
    begin_pass(cmd, fl_gpu_image_view(below), t->width, t->height, false);
    return fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, t->width, t->height);
}

/* Finish the layer at level: blur it, return to the layer below and composite it there, cut to the root's box. */
static void close_layer(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_RenderTarget *t, const Fl_SceneFrame *frame, size_t level,
                        const Fl_ResolvedEntity *v, const Fl_Style *style)
{
    Fl_GpuImage *layer = layer_image(p, t, level);
    vkCmdEndRendering(cmd);
    layer_transition(cmd, fl_gpu_image_handle(layer), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    double sigma = style->blur * t->width / frame->width;
    if (sigma > 0.05) blur_layer(p, cmd, t, layer, sigma);
    if (!resume(p, cmd, t, level)) return;
    float push[28];
    layer_push(push, t, KIND_COMPOSITE);
    const double *i = v->inverse.v, kx = frame->width / t->width, ky = frame->height / t->height;
    push[8] = (float)(i[0] * kx); push[9] = (float)(i[1] * ky); push[10] = (float)(i[0] * frame->x + i[1] * frame->y + i[2]); push[11] = (float)v->local.width;
    push[12] = (float)(i[3] * kx); push[13] = (float)(i[4] * ky); push[14] = (float)(i[3] * frame->x + i[4] * frame->y + i[5]); push[15] = (float)v->local.height;
    push[20] = (float)fmin(style->radius, fmin(v->local.width, v->local.height) * .5); push[21] = style->clip ? 1.f : 0.f; push[22] = v->kind == FL_WIDGET_ELLIPSE ? 1.f : 0.f;
    for (size_t k = 0; k < 28; ++k) if (!isfinite(push[k])) return;
    fl_scene_renderer_push(p, cmd, push, fl_gpu_image_descriptor(layer));
}

void fl_scene_renderer_entities(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_RenderTarget *target, const Fl_SceneFrame *frame,
                                const Fl_Scene *scene, const Fl_SceneState *state, const Fl_SceneGeometry *g)
{
    size_t *parent = g->count ? malloc(g->count * sizeof(*parent)) : NULL;
    for (size_t i = 0; parent && i < g->count; ++i) {
        const Fl_Entity *above = fl_scene_entity(scene, scene->entities[i].parent);
        parent[i] = above ? (size_t)(above - scene->entities) : SIZE_MAX;
    }
    struct {size_t entity, end; Fl_Style style;} open[LAYER_DEPTH];
    size_t depth = 0;
    ensure_layers(p, target);
    for (size_t k = 0; k < g->count; ++k) {
        while (depth && k >= open[depth - 1].end) {
            --depth; close_layer(p, cmd, target, frame, depth, &g->entities[open[depth].entity], &open[depth].style);
        }
        size_t index = g->order[k];
        const Fl_ResolvedEntity *v = &g->entities[index];
        Fl_Style style = fl_entity_style(&scene->entities[index]);
        bool layered = parent && (style.clip || style.blur > 0) && v->visible && v->invertible && v->opacity > 0 && depth < LAYER_DEPTH &&
                       layer_image(p, target, depth);
        if (!layered) {draw_entity(p, cmd, frame, scene, state, g, index, PART_ALL); continue;}
        draw_entity(p, cmd, frame, scene, state, g, index, PART_SHADOW);
        Fl_GpuImage *layer = layer_image(p, target, depth);
        vkCmdEndRendering(cmd);
        layer_transition(cmd, fl_gpu_image_handle(layer), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        begin_pass(cmd, fl_gpu_image_view(layer), target->width, target->height, true);
        fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, target->width, target->height);
        draw_entity(p, cmd, frame, scene, state, g, index, PART_BODY);
        size_t end = k + 1;
        while (end < g->count) {
            size_t walk = g->order[end];
            while (walk != SIZE_MAX && walk != index) walk = parent[walk];
            if (walk == SIZE_MAX) break;
            ++end;
        }
        open[depth].entity = index; open[depth].end = end; open[depth].style = style; ++depth;
    }
    while (depth) {--depth; close_layer(p, cmd, target, frame, depth, &g->entities[open[depth].entity], &open[depth].style);}
    free(parent);
}

Fl_SceneRenderer *fl_scene_renderer_create(const Fl_SceneRendererDesc *d)
{
    if (!d || (!d->gpu.device || !d->gpu.allocator || !d->gpu.compile_shader || !d->gpu.begin_transfer || !d->gpu.end_transfer) || !d->fonts || !d->jobs) return NULL;
    Fl_SceneRenderer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->gpu=d->gpu; p->fonts = d->fonts; p->jobs = d->jobs; p->bus = d->bus;
    p->invalidate = d->invalidate; p->invalidate_data = d->invalidate_data;
    p->device = d->gpu.device;
    p->images = fl_gpu_images_create(&d->gpu);
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
    for (unsigned i=0;i<2;++i) fl_scene_state_clear(&p->render_states[i]);
    for (size_t i = 0; i < p->picture_count; ++i) if (p->pictures[i].state == IMAGE_DECODING) fl_job_wait(p->jobs, p->pictures[i].job);
    vkDeviceWaitIdle(p->device);
    for (size_t i = 0; i < p->picture_count; ++i) release_picture(&p->pictures[i]);
    for (size_t i = 0; i < p->text_count; ++i) {fl_gpu_image_destroy(p->texts[i].image); free(p->texts[i].text);}
    for (size_t s = 0; s < LAYER_SLOTS; ++s) {
        for (size_t d = 0; d < LAYER_DEPTH; ++d) fl_gpu_image_destroy(p->layers[s][d]);
        fl_gpu_image_destroy(p->scratch[s]);
    }
    for (size_t i = 0; i < p->video_count; ++i) fl_gpu_image_destroy(p->videos[i].image);
    for (size_t i = 0; i < p->graveyard_count; ++i) fl_gpu_image_destroy(p->graveyard[i].image);
    free(p->videos); free(p->graveyard); free(p->pictures); free(p->texts);
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
    p->error[0]=0;
    if (p->render_project_identity!=fl_project_identity(project)) {
        for (unsigned i=0;i<2;++i) fl_scene_state_clear(&p->render_states[i]);
        p->render_project_identity=fl_project_identity(project);
    }
    unsigned selected=p->render_last^1u;
    for (unsigned i=0;i<2;++i) if (p->render_states[i].valid && p->render_states[i].scene==scene->id &&
        p->render_states[i].revision==scene->revision) {selected=i; break;}
    p->render_last=selected;
    Fl_SceneState *state=&p->render_states[selected];
    Fl_SceneGeometry geometry = {0};
    Fl_SceneFrame frame = fl_scene_frame(scene);
    bool ok = fl_scene_state_update(state, scene, tick);
    for (size_t i=0; ok && i<scene->entity_count; ++i) {
        const char *diagnostic=fl_script_runtime_error(state->scripts,i);
        if (diagnostic[0]) {snprintf(p->error,sizeof(p->error),"%s",diagnostic); ok=false;}
    }
    ok=ok && fl_scene_geometry_update(&geometry,scene,state);
    VmaAllocator allocator = p->gpu.allocator;
    Offscreen o = {0};
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    if (ok) {
        fl_scene_renderer_prepare(p, project, scene, state, (double)w / frame.width, true);
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
    VkCommandBuffer cmd = ok ? p->gpu.begin_transfer(p->gpu.host) : VK_NULL_HANDLE;
    if (ok && cmd) {
        transition(cmd, o.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        const unsigned char *backdrop = scene->background;
        VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = o.view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color = {.float32 = {fl_scene_renderer_linear(backdrop[0]), fl_scene_renderer_linear(backdrop[1]), fl_scene_renderer_linear(backdrop[2]), 1}}};
        VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {w, h}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
        vkCmdBeginRendering(cmd, &info);
        ok = fl_scene_renderer_begin(p, cmd, format, w, h);
        if (ok) {
            fl_scene_renderer_background(p, cmd, &frame, &frame, scene->background);
            Fl_RenderTarget target = {o.view, format, w, h, 0};
            fl_scene_renderer_entities(p, cmd, &target, &frame, scene, state, &geometry);
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
        p->gpu.end_transfer(p->gpu.host,cmd);
        ok = ok && vmaInvalidateAllocation(allocator, o.buffer_memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS;
    } else ok = false;
    if (ok) {
        out->rgba = malloc((size_t)w * h * 4);
        if (out->rgba) {memcpy(out->rgba, o.buffer_info.pMappedData, (size_t)w * h * 4); out->width = w; out->height = h;}
        ok = out->rgba != NULL;
    }
    offscreen_destroy(allocator, p->device, &o);
    fl_scene_geometry_clear(&geometry);
    if (!ok && !p->error[0]) snprintf(p->error,sizeof(p->error),"Frame evaluation or GPU rendering failed");
    return ok;
}

const char *fl_scene_renderer_error(const Fl_SceneRenderer *renderer)
{
    return renderer ? renderer->error : "Renderer is unavailable";
}
