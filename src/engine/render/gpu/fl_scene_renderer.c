#include "time/fl_composition.h"
#include "fl_scene_renderer.h"
#include "fl_gpu_image.h"
#include "core/fl_log.h"
#include "components/fl_style.h"
#include "components/fl_video_clip.h"
#include "media/fl_media.h"
#include "media/fl_video_decode.h"
#include "platform/fl_paths.h"
#include "text/fl_text_raster.h"
#include "components/fl_path.h"
#include "components/fl_effect.h"
#include "components/fl_path.h"
#include "components/fl_effect.h"
#include "components/fl_path.h"
#include "components/fl_effect.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "renderer"
#define KIND_IMAGE 6
#define IMAGE_MAX_SIDE 8192u
#define PIPELINE_FORMATS 48u
#define PUSH_FLOATS 32u
#define UBO_ALIGN 256u
#define UBO_SLOTS 8192u
#define PIPE_FLAT 0
#define PIPE_DEPTH 1
#define PIPE_PARTICLE 2
#define PIPE_ADD 3
#define PIPE_MESH 4
#define PIPE_MUL 5
#define PIPE_SCREEN 6
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
    double tracking;
    int glyph_count;
    int glyph_box[FL_RASTER_GLYPHS][4];
    Fl_GpuImage *image, *glyphs[FL_RASTER_GLYPHS];
} Fl_CachedText;
typedef struct Fl_CachedPath { Fl_Id entity; uint64_t hash; Fl_GpuImage *image; } Fl_CachedPath;
typedef struct Fl_EffectPipe { uint64_t hash; VkFormat format; int depth; VkPipeline pipeline; } Fl_EffectPipe;
/* Workers own copied source paths and one bounded decoded frame; no project pointers cross threads. */
typedef struct Fl_VideoDecode {
    char path[FL_PATH_MAX];
    double seconds;
    unsigned char *pixels;
    uint32_t width,height;
    atomic_bool cancel;
} Fl_VideoDecode;
typedef struct Fl_VideoFrame {
    Fl_Id entity;
    uint64_t project;
    unsigned char hash[32];
    double seconds;
    Fl_GpuImage *image;
    Fl_VideoDecode *decode;
    Fl_JobHandle job;
    bool used,valid,failed;
} Fl_VideoFrame;
#define VIDEO_SLOTS 16u
#define VIDEO_MAX_SIDE 1920u
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
typedef struct Fl_Pipeline { VkFormat format; int kind; VkPipeline pipeline; } Fl_Pipeline;

struct Fl_SceneRenderer {
    Fl_GpuContext gpu;
    Fl_Fonts *fonts;
    bool owns_fonts;
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
    bool video_error;
    Fl_Retired *graveyard;
    size_t graveyard_count, graveyard_capacity;
    Fl_CachedImage *pictures;
    size_t picture_count, picture_capacity;
    Fl_Id picture_scene;
    uint64_t picture_revision;
    VkDescriptorSetLayout ubo_layout;
    VkDescriptorPool ubo_pool;
    VkDescriptorSet ubo_set;
    VkBuffer ubo;
    VmaAllocation ubo_memory;
    unsigned char *ubo_map;
    uint32_t ubo_cursor;
    VkImage depth_image;
    VmaAllocation depth_memory;
    VkImageView depth_view;
    uint32_t depth_width, depth_height;
    bool depth_ready;
    VkDescriptorPool sample_pool;
    VkDescriptorSet sample_set[LAYER_SLOTS];
    Fl_GpuImage *bloom_image[LAYER_SLOTS][2];
    uint32_t bloom_width, bloom_height;
    VkBuffer mesh_buffer;
    VmaAllocation mesh_memory;
    uint32_t mesh_plane, mesh_cube, mesh_sphere, mesh_plane_count, mesh_cube_count, mesh_sphere_count;
    Fl_Mat4 view_proj;
    Fl_Vec3 cam_right, cam_up;
    float crop_x, crop_y, crop_w, crop_h;
    double bloom, vignette, grain, lift, gamma, gain;
    Fl_CachedPath *paths;
    size_t path_count;
    Fl_EffectPipe effects[8];
    size_t effect_count;
    float draw_mvp[16];
    VkFormat pass_format;
    bool pass_depth;
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
        if (!n) continue;
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

static bool ensure_ubo(Fl_SceneRenderer *p)
{
    if (p->ubo) return p->ubo_map != NULL;
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)UBO_ALIGN * UBO_SLOTS, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo host = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo mapped = {0};
    bool ok = vkCreateDescriptorSetLayout(p->device, &layout, NULL, &p->ubo_layout) == VK_SUCCESS &&
        vmaCreateBuffer(p->gpu.allocator, &buffer, &host, &p->ubo, &p->ubo_memory, &mapped) == VK_SUCCESS;
    p->ubo_map = mapped.pMappedData;
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    if (ok) ok = vkCreateDescriptorPool(p->device, &pool, NULL, &p->ubo_pool) == VK_SUCCESS;
    VkDescriptorSetAllocateInfo sets = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p->ubo_pool, .descriptorSetCount = 1, .pSetLayouts = &p->ubo_layout};
    if (ok) ok = vkAllocateDescriptorSets(p->device, &sets, &p->ubo_set) == VK_SUCCESS;
    if (ok) {
        VkDescriptorBufferInfo info = {p->ubo, 0, UBO_ALIGN};
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->ubo_set, .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &info};
        vkUpdateDescriptorSets(p->device, 1, &write, 0, NULL);
    }
    return ok && p->ubo_map;
}

/* Compile the primitive shaders and build one pipeline for a colour format and depth/blend variant. */
static bool create_pipeline(Fl_SceneRenderer *p, VkFormat format, int kind)
{
    const char *vs = "#version 450\nlayout(set=1,binding=0) uniform M { mat4 mvp; } u;"
        "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; vec4 fill2; vec4 extra; vec4 mid; } p;"
        "layout(location=0) out vec2 uv;vec2 v[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));"
        "void main(){uv=v[gl_VertexIndex];float kind=p.params.w;"
        "if(kind==15.&&p.extra.w>0.5) gl_Position=u.mvp*vec4(uv*vec2(p.a.w,p.b.w),0.,1.);"
        "else if(kind==15.||kind==17.||kind==18.||kind==19.||kind==20.||kind==23.||kind==24.) gl_Position=vec4(uv*2.-1.,0.,1.);"
        "else gl_Position=u.mvp*vec4(uv*p.params.xy,0.,1.);}";
    static const char fs_head[] = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; vec4 fill2; vec4 extra; vec4 mid; } p;"
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
        "if(kind==19.){vec4 c=texture(mask,gl_FragCoord.xy/size);color=vec4(c.rgb*p.extra.x,0.);return;}"
        "if(kind==20.){vec4 c=texture(mask,gl_FragCoord.xy/size);float luma=dot(c.rgb,vec3(.2126,.7152,.0722));"
        "float k=smoothstep(p.extra.x,p.extra.x+.15,luma);color=vec4(c.rgb*k,k);return;}"
        "if(kind==22.){float d=length((uv-.5)*2.);color=p.fill;color.a*=1.-smoothstep(.55,1.,d);return;}";
    static const char fs_look[] =
        "if(kind==24.){color=texture(mask,gl_FragCoord.xy/size);return;}"
        "if(kind==23.){vec2 suv=gl_FragCoord.xy/size;vec4 c=texture(mask,suv);vec3 rgb=c.a>1e-5?c.rgb/max(c.a,.0001):c.rgb;"
        "vec2 q=suv*2.-1.;rgb*=1.-p.extra.x*dot(q,q);float n=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453)-.5;rgb+=n*p.extra.y;"
        "rgb=pow(max(rgb,vec3(0.)),vec3(1./max(p.fill.x,.2)))*p.fill.y+p.extra.z;color=vec4(rgb,c.a);return;}"
        "if(kind==21.){float time=p.mid.x;float ek=p.mid.y;vec2 q=uv;vec4 A=p.fill;vec4 B=p.stroke;"
        "if(ek<1.5){q+=.12*p.fill2.x*vec2(sin(q.y*8.+time),cos(q.x*6.-time));float t=fract(q.x+q.y*.35+time*.05);color=mix(A,B,t);}"
        "else if(ek<2.5){float d=length(q-.5)-p.fill2.x;float w=sin(atan(q.y-.5,q.x-.5)*max(p.fill2.y,1.)+time);color=mix(B,A,smoothstep(.04,-.04,d+.08*w));}"
        "else if(ek<3.5){float n=fract(sin(dot(q*size+time*vec2(17.,9.),vec2(12.9898,78.233)))*43758.5453);color=vec4(mix(A.rgb,B.rgb,n),A.a*(.35+n*p.fill2.x));}"
        "else if(ek<4.5){float t=q.x;color=vec4(mix(A.r,B.r,clamp(t+p.fill2.x,0.,1.)),mix(A.g,B.g,t),mix(A.b,B.b,clamp(t-p.fill2.x,0.,1.)),A.a);}"
        "else{vec2 d=q+p.fill2.x*vec2(sin(q.y*10.+time),cos(q.x*10.-time));color=mix(A,B,clamp(d.x,0.,1.));}"
        "return;}"
        "if(kind==16.){float m=p.extra.w;float d=rbox((uv-.5)*size,size*.5-m,r);color=p.fill;color.a*=1-smoothstep(-sw,sw,d);return;}"
        "if(kind==5.){vec2 m=texture(mask,uv).rg;float fa=p.fill.a*m.r;float sa=p.stroke.a*m.g;float cover=fa+(1.-fa)*sa;"
        "vec3 rgb=cover>1e-4?(p.fill.rgb*fa*(1.-sa)+p.stroke.rgb*sa)/cover:vec3(0.);color=vec4(rgb,cover);return;}"
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
        "if(p.extra.z>.5){float t;if(p.extra.z>1.5)t=length((uv-.5)*2.);else{vec2 dir=vec2(cos(p.extra.y),sin(p.extra.y));t=dot((uv-.5)*size,dir)/max(abs(size.x*dir.x)+abs(size.y*dir.y),.0001)+.5;}"
        "t=clamp(t,0.,1.);float m=clamp(p.extra.w,.02,.98);base=p.mid.a>.01?(t<m?mix(p.fill,p.mid,t/m):mix(p.mid,p.fill2,(t-m)/max(1.-m,.0001))):mix(p.fill,p.fill2,t);"
        "float n=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453)-.5;base.rgb+=n*.004;}"
        "color=mix(base,p.stroke,border);color.a*=cover;}";
    char outlines[2048], fs[sizeof(outlines) + sizeof(fs_head) + sizeof(fs_tail) + sizeof(fs_look)];
    polygon_glsl(outlines, sizeof(outlines));
    snprintf(fs, sizeof(fs), "%s%s%s%s", fs_head, outlines, fs_tail, fs_look);
    VkShaderModule vert = p->gpu.compile_shader(p->gpu.host,vs,VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = p->gpu.compile_shader(p->gpu.host,fs,VK_SHADER_STAGE_FRAGMENT_BIT);
    bool ok = vert && frag && ensure_ubo(p);
    if (ok && !p->layout) {
        VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        VkDescriptorSetLayout sets[] = {fl_gpu_images_layout(p->images), p->ubo_layout};
        VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = sets,
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
        VkPipelineColorBlendAttachmentState blend = {.blendEnable = VK_TRUE,
            .srcColorBlendFactor = kind == PIPE_ADD || kind == PIPE_SCREEN ? VK_BLEND_FACTOR_ONE : kind == PIPE_MUL ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = kind == PIPE_ADD ? VK_BLEND_FACTOR_ONE : kind == PIPE_SCREEN ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = kind == PIPE_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 15};
        VkPipelineColorBlendStateCreateInfo blends = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
        VkPipelineDepthStencilStateCreateInfo depth_state = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = kind == PIPE_PARTICLE, .depthWriteEnable = VK_FALSE, .depthCompareOp = VK_COMPARE_OP_LESS};
        VkFormat depth_format = kind == PIPE_FLAT ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = states};
        VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format,
            .depthAttachmentFormat = depth_format};
        VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
            .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
            .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples, .pDepthStencilState = kind == PIPE_FLAT ? NULL : &depth_state,
            .pColorBlendState = &blends, .pDynamicState = &dynamic, .layout = p->layout};
        ok = vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) == VK_SUCCESS;
    }
    if (vert) vkDestroyShaderModule(p->device, vert, NULL);
    if (frag) vkDestroyShaderModule(p->device, frag, NULL);
    if (ok) p->pipelines[p->pipeline_count++] = (Fl_Pipeline){format, kind, pipeline};
    return ok;
}

/* Rasterize one text entity into a coverage texture using the resolved face. */
static Fl_GpuImage *rasterize_text(Fl_SceneRenderer *p, const Fl_CachedText *entry, const unsigned char *text, double scale)
{
    Fl_FontMatch match = fl_fonts_match(p->fonts, &entry->typography);
    Fl_TextStyle style = {match.face, entry->font_size * scale, match.synthetic_bold, match.synthetic_italic, entry->align, entry->tracking * scale};
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
        Fl_CachedText *entry = &next[at++]; entry->entity = e->id;
        Fl_Bounds box = state ? state->entities[i].bounds : fl_entity_bounds(e);
        entry->length = text->size; entry->width = box.width; entry->height = box.height;
        entry->font_size = state ? state->entities[i].appearance.font_size : fl_widget_appearance(e).font_size;
        entry->typography = fl_widget_typography(e);
        entry->align = (int)(state ? state->entities[i].style.align : fl_entity_style(e).align);
        entry->tracking = state ? state->entities[i].motion.tracking : 0;
        bool kinetic = state && state->entities[i].has_motion && (state->entities[i].motion.stagger != 0 || state->entities[i].motion.rise != 0 || state->entities[i].motion.fade != 0);
        entry->text = text->size ? malloc(text->size) : NULL;
        if (text->size && !entry->text) continue;
        if (text->size) memcpy(entry->text, text->data, text->size);
        for (size_t j = 0; !rebuild && j < p->text_count; ++j) {
            Fl_CachedText *old = &p->texts[j];
            if (old->entity == e->id && p->text_scene == scene->id && old->width == entry->width && old->height == entry->height &&
                old->font_size == entry->font_size && old->align == entry->align && old->tracking == entry->tracking && same_typography(&old->typography, &entry->typography) && old->length == entry->length &&
                (!text->size || (old->text && !memcmp(old->text, text->data, text->size)))) {
                entry->image = old->image; old->image = NULL;
                entry->glyph_count = old->glyph_count; memcpy(entry->glyph_box, old->glyph_box, sizeof(entry->glyph_box));
                memcpy(entry->glyphs, old->glyphs, sizeof(entry->glyphs)); old->glyph_count = 0; memset(old->glyphs, 0, sizeof(old->glyphs));
                break;
            }
        }
        if (!entry->image && entry->width > 0 && entry->height > 0) {
            entry->image = rasterize_text(p, entry, text->data, scale);
            if (kinetic && entry->text) {
                Fl_FontMatch match = fl_fonts_match(p->fonts, &entry->typography);
                Fl_TextStyle face = {match.face, entry->font_size * scale, match.synthetic_bold, match.synthetic_italic, entry->align, entry->tracking * scale};
                Fl_RasterGlyph glyphs[FL_RASTER_GLYPHS];
                size_t n = fl_text_rasterize_glyphs(p->rasterizer, &face, entry->text, entry->length, entry->tracking * scale, glyphs, FL_RASTER_GLYPHS);
                entry->glyph_count = (int)n;
                for (size_t g = 0; g < n; ++g) {
                    entry->glyph_box[g][0] = glyphs[g].x; entry->glyph_box[g][1] = glyphs[g].y;
                    entry->glyph_box[g][2] = glyphs[g].w; entry->glyph_box[g][3] = glyphs[g].h;
                    entry->glyphs[g] = fl_gpu_image_create(p->images, FL_GPU_IMAGE_COVERAGE, glyphs[g].coverage, (uint32_t)glyphs[g].w, (uint32_t)glyphs[g].h);
                    free(glyphs[g].coverage);
                }
            }
            if (!entry->image) FL_LOG_ERROR(LOG_TAG, "could not rasterize text entity %llu", (unsigned long long)e->id);
        }
    }
    bool retired = false;
    for (size_t i = 0; i < p->text_count; ++i) retired |= p->texts[i].image != NULL;
    if (retired) vkDeviceWaitIdle(p->device);
    for (size_t i = 0; i < p->text_count; ++i) {fl_gpu_image_destroy(p->texts[i].image); for (int g = 0; g < p->texts[i].glyph_count; ++g) fl_gpu_image_destroy(p->texts[i].glyphs[g]); free(p->texts[i].text);}
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
    for (size_t i=0;i<p->video_count;++i) if (p->videos[i].decode && p->videos[i].job.index==done->job.index && p->videos[i].job.generation==done->job.generation) {
        if (p->invalidate) p->invalidate(p->invalidate_data); break;
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

/* Decode one requested timestamp; the request remains owned until its job fence. */
static void decode_video(void *data)
{
    Fl_VideoDecode *decode=data;
    decode->pixels=fl_video_decode(decode->path,decode->seconds,VIDEO_MAX_SIDE,&decode->width,&decode->height,&decode->cancel);
}

/* Release worker storage only after completion. GPU storage is retired separately. */
static void release_video_decode(Fl_VideoFrame *slot)
{
    if (!slot->decode) return;
    free(slot->decode->pixels); free(slot->decode); slot->decode=NULL; slot->job=(Fl_JobHandle){0};
}

/* Preview keeps the last uploaded frame while a worker decodes; exact rendering fences only requested timestamps. */
static void prepare_videos(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene, const Fl_SceneState *state, const Fl_SceneGeometry *geometry, Fl_Tick tick, bool wait)
{
    p->video_error=false;
    size_t retained=0;
    for (size_t i=0;i<p->video_count;++i) {
        Fl_VideoFrame *slot=&p->videos[i]; const Fl_Entity *entity=fl_scene_entity(scene,slot->entity);
        size_t index=entity ? (size_t)(entity-scene->entities) : SIZE_MAX;
        bool used=entity && geometry->entities[index].visible && geometry->entities[index].opacity>0 &&
            geometry->entities[index].invertible && fl_entity_component(entity,FL_COMPONENT_VIDEO);
        if (!used && slot->decode) {
            atomic_store_explicit(&slot->decode->cancel,true,memory_order_relaxed);
            if (wait) fl_job_wait(p->jobs,slot->job);
        }
        if (used || (slot->decode && !fl_job_is_done(p->jobs,slot->job))) {slot->used=false; p->videos[retained++]=*slot;}
        else {release_video_decode(slot); retire_image(p,slot->image);}
    }
    p->video_count=retained;
    for (size_t i=0;i<scene->entity_count;++i) {
        const Fl_Entity *entity=&scene->entities[i]; Fl_VideoClip clip;
        if (!geometry->entities[i].visible || geometry->entities[i].opacity<=0 || !geometry->entities[i].invertible || !fl_video_clip_decode(fl_entity_component(entity,FL_COMPONENT_VIDEO),&clip)) continue;
        const Fl_Asset *asset=fl_project_asset(project,clip.asset); if (!asset) {p->video_error=true; continue;}
        double seconds=state && state->entities[i].media_seconds>=0 ? state->entities[i].media_seconds : fl_video_clip_source_seconds(&clip,state ? state->entities[i].local_tick : tick,asset->duration);
        Fl_VideoFrame *slot=NULL;
        for (size_t j=0;j<p->video_count;++j) if (p->videos[j].entity==entity->id) {slot=&p->videos[j]; break;}
        if (!slot) {
            if (p->video_count==VIDEO_SLOTS) {p->video_error=true; continue;}
            if (p->video_count==p->video_capacity) {
                size_t capacity=p->video_capacity ? p->video_capacity*2 : 4;
                Fl_VideoFrame *grown=realloc(p->videos,capacity*sizeof(*grown));
                if (!grown) {p->video_error=true; continue;}
                p->videos=grown; p->video_capacity=capacity;
            }
            slot=&p->videos[p->video_count++]; *slot=(Fl_VideoFrame){.entity=entity->id};
        }
        slot->used=true;
        bool same=slot->project==fl_project_identity(project) && !memcmp(slot->hash,asset->hash,32) && slot->seconds==seconds;
        if (slot->decode && !same) atomic_store_explicit(&slot->decode->cancel,true,memory_order_relaxed);
        if (slot->decode && wait) fl_job_wait(p->jobs,slot->job);
        if (slot->decode && fl_job_is_done(p->jobs,slot->job)) {
            if (same && !atomic_load_explicit(&slot->decode->cancel,memory_order_relaxed)) {
                Fl_VideoDecode *decode=slot->decode;
                Fl_GpuImage *image=decode->pixels ? fl_gpu_image_create(p->images,FL_GPU_IMAGE_SRGBA,decode->pixels,decode->width,decode->height) : NULL;
                retire_image(p,slot->image); slot->image=image; slot->valid=true; slot->failed=image==NULL;
            }
            release_video_decode(slot);
        }
        if (slot->decode) continue;
        if (!same || !slot->valid) {
            if (slot->project!=fl_project_identity(project) || memcmp(slot->hash,asset->hash,32)) {retire_image(p,slot->image); slot->image=NULL;}
            slot->project=fl_project_identity(project); memcpy(slot->hash,asset->hash,32); slot->seconds=seconds; slot->valid=false; slot->failed=false;
            slot->decode=calloc(1,sizeof(*slot->decode));
            if (slot->decode) {
                atomic_init(&slot->decode->cancel,false); slot->decode->seconds=seconds;
                if (fl_project_asset_path(project,asset,slot->decode->path,sizeof(slot->decode->path)))
                    slot->job=fl_job_submit(p->jobs,&(Fl_JobDesc){.fn=decode_video,.data=slot->decode,.priority=FL_JOB_PRIORITY_HIGH,.notify=p->bus!=NULL});
            }
            if (!slot->decode || !slot->job.generation) {release_video_decode(slot); slot->failed=true; slot->valid=true;}
            else if (wait) {
                fl_job_wait(p->jobs,slot->job); Fl_VideoDecode *decode=slot->decode;
                Fl_GpuImage *image=decode->pixels ? fl_gpu_image_create(p->images,FL_GPU_IMAGE_SRGBA,decode->pixels,decode->width,decode->height) : NULL;
                retire_image(p,slot->image); slot->image=image; slot->valid=true; slot->failed=image==NULL; release_video_decode(slot);
            }
        }
        if (slot->failed) p->video_error=true;
    }
    size_t kept=0;
    for (size_t i=0;i<p->video_count;++i) {
        Fl_VideoFrame *slot=&p->videos[i];
        if (!slot->used && slot->decode) atomic_store_explicit(&slot->decode->cancel,true,memory_order_relaxed);
        if (slot->used || (slot->decode && !fl_job_is_done(p->jobs,slot->job))) p->videos[kept++]=*slot;
        else {release_video_decode(slot); retire_image(p,slot->image);}
    }
    p->video_count=kept;
    if (wait && p->video_error) snprintf(p->error,sizeof(p->error),"Cannot decode requested video timestamp or video budget exceeded (maximum %u visible video entities)",VIDEO_SLOTS);
}

static void prepare_paths(Fl_SceneRenderer *p, const Fl_Scene *scene, const Fl_SceneState *state)
{
    size_t count = 0;
    for (size_t i = 0; i < scene->entity_count; ++i) if (fl_entity_component(&scene->entities[i], FL_COMPONENT_PATH)) ++count;
    Fl_CachedPath *next = count ? calloc(count, sizeof(*next)) : NULL;
    if (count && !next) return;
    size_t at = 0;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        const Fl_Component *component = fl_entity_component(&scene->entities[i], FL_COMPONENT_PATH);
        if (!component) continue;
        Fl_Path path;
        if (!fl_path_decode(component, &path)) {++at; continue;}
        if (state && i < state->count && state->entities[i].has_path) {path.trim_start = state->entities[i].trim_start; path.trim_end = state->entities[i].trim_end;}
        Fl_Bounds box = state && i < state->count ? state->entities[i].bounds : fl_entity_bounds(&scene->entities[i]);
        double stroke = state && i < state->count ? state->entities[i].appearance.stroke_width : fl_widget_appearance(&scene->entities[i]).stroke_width;
        uint64_t hash = 14695981039346656037ull;
        const unsigned char *bytes = component->data;
        for (size_t b = 0; b < component->size; ++b) { hash ^= bytes[b]; hash *= 1099511628211ull; }
        hash ^= (uint64_t)(path.trim_start * 1000) ^ ((uint64_t)(path.trim_end * 1000) << 20) ^ ((uint64_t)box.width << 40);
        hash ^= (uint64_t)llround(box.height * 1000); hash *= 1099511628211ull;
        hash ^= (uint64_t)llround(stroke * 1000); hash *= 1099511628211ull;
        Fl_CachedPath *entry = &next[at++]; entry->entity = scene->entities[i].id; entry->hash = hash;
        for (size_t j = 0; j < p->path_count; ++j) if (p->paths[j].entity == entry->entity && p->paths[j].hash == hash) {entry->image = p->paths[j].image; p->paths[j].image = NULL; break;}
        if (!entry->image && box.width > 0 && box.height > 0) {
            uint32_t w, h; unsigned char *mask = fl_path_raster(&path, box.width, box.height, stroke, &w, &h);
            if (mask) {entry->image = fl_gpu_image_create(p->images, FL_GPU_IMAGE_VECTOR, mask, w, h); free(mask);}
        }
    }
    for (size_t i = 0; i < p->path_count; ++i) fl_gpu_image_destroy(p->paths[i].image);
    free(p->paths); p->paths = next; p->path_count = at;
}

static VkDescriptorSet video_descriptor(const Fl_SceneRenderer *p, Fl_Id entity)
{
    for (size_t i = 0; i < p->video_count; ++i) if (p->videos[i].entity == entity) return fl_gpu_image_descriptor(p->videos[i].image);
    return VK_NULL_HANDLE;
}

void fl_scene_renderer_prepare(Fl_SceneRenderer *p, const Fl_Project *project, const Fl_Scene *scene, const Fl_SceneState *state, const Fl_SceneGeometry *geometry, double text_scale, bool wait)
{
    if (!p || !project || !scene) return;
    scene=fl_scene_state_scene(state,scene);
    if (!geometry || geometry->count!=scene->entity_count || geometry->scene!=scene->id || geometry->revision!=scene->revision) {
        p->video_error=true; snprintf(p->error,sizeof(p->error),"Cannot prepare media without matching resolved geometry"); return;
    }
    uint64_t identity=fl_project_identity(project);
    if (p->project_identity!=identity) {p->text_scene=0; p->picture_scene=0;}
    p->project_identity=identity;
    prepare_texts(p, scene, state, text_scale > 0 ? text_scale : 1);
    prepare_paths(p, scene, state);
    prepare_pictures(p, project, scene);
    stage_pictures(p, project, scene, wait);
    age_graveyard(p);
    prepare_videos(p, project, scene, state,geometry,state ? state->tick : 0,wait);
}

/* Pipeline for format, built on first use; VK_NULL_HANDLE when it cannot be created. */
static VkPipeline pipeline_for(Fl_SceneRenderer *p, VkFormat format, int kind)
{
    for (size_t i = 0; i < p->pipeline_count; ++i) if (p->pipelines[i].format == format && p->pipelines[i].kind == kind) return p->pipelines[i].pipeline;
    if (p->pipeline_count == PIPELINE_FORMATS || !create_pipeline(p, format, kind)) return VK_NULL_HANDLE;
    return p->pipelines[p->pipeline_count - 1].pipeline;
}

static uint32_t upload_uniform(Fl_SceneRenderer *p, const void *data, size_t bytes)
{
    if (!p->ubo_map || bytes > UBO_ALIGN) return UINT32_MAX;
    if (p->ubo_cursor >= UBO_SLOTS) {vkDeviceWaitIdle(p->device); p->ubo_cursor = 0;}
    uint32_t offset = p->ubo_cursor++ * UBO_ALIGN;
    memset(p->ubo_map + offset, 0, UBO_ALIGN);
    memcpy(p->ubo_map + offset, data, bytes);
    vmaFlushAllocation(p->gpu.allocator, p->ubo_memory, offset, UBO_ALIGN);
    return offset;
}

void fl_scene_renderer_sync_uniforms(Fl_SceneRenderer *p, VkCommandBuffer cmd)
{
    if (!p || !cmd || !p->ubo_map) return;
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}

bool fl_scene_renderer_begin(Fl_SceneRenderer *p, VkCommandBuffer cmd, VkFormat format, uint32_t width, uint32_t height, bool depth)
{
    if (!p || !cmd) return false;
    p->pass_format = format; p->pass_depth = depth;
    VkPipeline pipeline = pipeline_for(p, format, depth ? PIPE_DEPTH : PIPE_FLAT);
    if (!pipeline) return false;
    VkRect2D area = {{0, 0}, {width, height}};
    VkViewport view = {0, 0, (float)width, (float)height, 0, 1};
    if (format != LAYER_FORMAT && p->crop_w > 1 && p->crop_h > 1) {
        float x = p->crop_x, y = p->crop_y, cw = p->crop_w, ch = p->crop_h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + cw > (float)width) cw = (float)width - x;
        if (y + ch > (float)height) ch = (float)height - y;
        if (cw > 1 && ch > 1) {
            view.x = x; view.y = y; view.width = cw; view.height = ch;
            area.offset.x = (int32_t)x; area.offset.y = (int32_t)y;
            area.extent.width = (uint32_t)cw; area.extent.height = (uint32_t)ch;
        }
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); vkCmdSetViewport(cmd, 0, 1, &view); vkCmdSetScissor(cmd, 0, 1, &area);
    return true;
}

void fl_scene_renderer_set_view(Fl_SceneRenderer *p, Fl_Mat4 view_proj, Fl_Vec3 right, Fl_Vec3 up, double bloom)
{
    if (!p) return;
    p->view_proj = view_proj; p->cam_right = right; p->cam_up = up; p->bloom = bloom;
}

void fl_scene_renderer_set_viewport(Fl_SceneRenderer *p, float x, float y, float width, float height)
{
    if (!p) return;
    p->crop_x = x; p->crop_y = y; p->crop_w = width; p->crop_h = height;
}

static void use_model(Fl_SceneRenderer *p, Fl_Mat4 model)
{
    fl_mat4_float(fl_mat4_mul(p->view_proj, model), p->draw_mvp);
}

static void use_affine(Fl_SceneRenderer *p, Fl_Affine matrix)
{
    Fl_Mat4 model = fl_mat4_identity();
    model.m[0] = matrix.v[0]; model.m[1] = matrix.v[3]; model.m[4] = matrix.v[1]; model.m[5] = matrix.v[4]; model.m[12] = matrix.v[2]; model.m[13] = matrix.v[5];
    use_model(p, model);
}

void fl_scene_renderer_push(Fl_SceneRenderer *p, VkCommandBuffer cmd, const float push[PUSH_FLOATS], VkDescriptorSet mask)
{
    uint32_t offset = upload_uniform(p, p->draw_mvp, sizeof(p->draw_mvp));
    if (offset == UINT32_MAX) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &mask, 0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &p->ubo_set, 1, &offset);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

VkDescriptorSet fl_scene_renderer_white(const Fl_SceneRenderer *p) { return p ? fl_gpu_images_white(p->images) : VK_NULL_HANDLE; }

/* Fill the push block: affine to clip space, linear-light colours, sizes, kind and style parameters. */
static void push_block(float push[PUSH_FLOATS], const Fl_SceneFrame *frame, Fl_Affine matrix, double width, double height, Fl_WidgetKind kind,
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
    push[24] = style ? (float)style->radius : 0; push[25] = style ? (float)style->gradient_angle : 0;
    push[26] = style && style->gradient ? (style->radial ? 2.f : 1.f) : 0.f;
    push[27] = style && style->gradient_mid[3] ? (float)style->gradient_mid_pos : 0.f;
    const unsigned char *mid = style ? style->gradient_mid : (const unsigned char[4]){0};
    push[28] = fl_scene_renderer_linear(mid[0]); push[29] = fl_scene_renderer_linear(mid[1]);
    push[30] = fl_scene_renderer_linear(mid[2]); push[31] = mid[3] / 255.f;
}

void fl_scene_renderer_styled(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Affine matrix,
                              double width, double height, Fl_WidgetKind kind, Fl_Appearance appearance, VkDescriptorSet mask,
                              const Fl_PrimitiveStyle *style)
{
    float push[PUSH_FLOATS];
    push_block(push, frame, matrix, width, height, kind, appearance, style);
    for (size_t i = 0; i < PUSH_FLOATS; ++i) if (!isfinite(push[i])) return;
    use_affine(p, matrix);
    fl_scene_renderer_push(p, cmd, push, mask);
}

void fl_scene_renderer_model(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Mat4 model,
                             double width, double height, Fl_WidgetKind kind, Fl_Appearance appearance, VkDescriptorSet mask,
                             const Fl_PrimitiveStyle *style)
{
    float push[PUSH_FLOATS];
    push_block(push, frame, fl_affine_from_mat4(model), width, height, kind, appearance, style);
    for (size_t i = 0; i < PUSH_FLOATS; ++i) if (!isfinite(push[i])) return;
    use_model(p, model);
    fl_scene_renderer_push(p, cmd, push, mask);
}

void fl_scene_renderer_grid_plane(Fl_SceneRenderer *p, VkCommandBuffer cmd, double origin_x, double origin_y, double extent,
                                  double spacing, const unsigned char minor[4], const unsigned char major[4])
{
    if (!p || !minor || !major || !(extent > 0) || !(spacing > 0)) return;
    float push[PUSH_FLOATS] = {0, 0, 0, (float)extent, 0, 0, 0, (float)extent,
        fl_scene_renderer_linear(minor[0]), fl_scene_renderer_linear(minor[1]), fl_scene_renderer_linear(minor[2]), minor[3] / 255.f,
        fl_scene_renderer_linear(major[0]), fl_scene_renderer_linear(major[1]), fl_scene_renderer_linear(major[2]), major[3] / 255.f,
        (float)origin_x, (float)origin_y, (float)spacing, FL_RENDER_KIND_GRID, 0, 0, 0, 0, 0, 0, 0, 1};
    for (size_t i = 0; i < PUSH_FLOATS; ++i) if (!isfinite(push[i])) return;
    use_model(p, fl_mat4_translation(origin_x, origin_y, 0));
    fl_scene_renderer_push(p, cmd, push, fl_scene_renderer_white(p));
}

void fl_scene_renderer_primitive(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Affine matrix,
                                 double width, double height, Fl_WidgetKind kind, Fl_Appearance appearance, VkDescriptorSet mask)
{
    fl_scene_renderer_styled(p, cmd, frame, matrix, width, height, kind, appearance, mask, NULL);
}

/* Soft drop shadow of a (rounded) box: a larger quad whose alpha falls off with the distance to the box. */
static void draw_shadow(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Mat4 model, double width, double height,
                        double radius, const Fl_Style *style, double opacity)
{
    double margin = style->shadow_blur + 2, shift_x = style->shadow_x - margin, shift_y = style->shadow_y - margin;
    model = fl_mat4_mul(model, fl_mat4_translation(shift_x, shift_y, 0));
    Fl_Affine matrix = fl_affine_from_mat4(model);
    Fl_Appearance color = {.stroke_width = style->shadow_blur};
    memcpy(color.fill, style->shadow_color, 4);
    color.fill[3] = (unsigned char)lround(color.fill[3] * opacity);
    float push[PUSH_FLOATS];
    push_block(push, frame, matrix, width + 2 * margin, height + 2 * margin, (Fl_WidgetKind)16, color, NULL);
    push[24] = (float)fmin(radius, fmin(width, height) * .5); push[27] = (float)margin;
    for (size_t i = 0; i < PUSH_FLOATS; ++i) if (!isfinite(push[i])) return;
    use_model(p, model);
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

static VkPipeline effect_pipeline(Fl_SceneRenderer *p, VkFormat format, bool depth, const char *source, size_t size);

/* Draw one entity's own pixels; children are drawn by the caller. */
static void draw_entity(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_Scene *scene,
                        const Fl_SceneState *state, const Fl_SceneGeometry *g, size_t index, Fl_EntityPart part)
{
    VkDescriptorSet white = fl_scene_renderer_white(p);
    const Fl_Appearance placeholder = {.fill = FL_RENDER_PLACEHOLDER_FILL_RGBA, .stroke = FL_RENDER_PLACEHOLDER_STROKE_RGBA, .stroke_width = FL_RENDER_PLACEHOLDER_STROKE};
    const Fl_ResolvedEntity *v = &g->entities[index]; const Fl_Entity *e = &scene->entities[index];
    if (!v->visible || v->kind == FL_WIDGET_ENTITY || v->opacity <= 0) return;
    Fl_Appearance look = state ? state->entities[index].appearance : fl_widget_appearance(e);
    look.fill[3] = (unsigned char)lround(look.fill[3] * v->opacity);
    look.stroke[3] = (unsigned char)lround(look.stroke[3] * v->opacity);
    Fl_Appearance picture = {.fill = {255, 255, 255, (unsigned char)lround(255 * v->opacity)}};
    Fl_Style style = state ? state->entities[index].style : fl_entity_style(e);
    Fl_PrimitiveStyle shape = {.radius = style.radius, .gradient = style.gradient, .gradient_angle = style.gradient_angle, .radial = style.radial, .gradient_mid_pos = style.gradient_mid_pos};
    memcpy(shape.gradient_to, style.gradient_to, 4);
    shape.gradient_to[3] = (unsigned char)lround(style.gradient_to[3] * v->opacity);
    memcpy(shape.gradient_mid, style.gradient_mid, 4);
    memcpy(shape.crop, state ? state->entities[index].crop : style.crop, sizeof(shape.crop));
    bool boxed = v->kind == FL_WIDGET_RECTANGLE || v->kind == FL_WIDGET_ELLIPSE || v->kind == FL_WIDGET_IMAGE || v->kind == FL_WIDGET_VIDEO || v->kind == FL_WIDGET_EFFECT;
    if (part != PART_BODY && style.glow > 0 && look.fill[3]) {
        Fl_Style halo = style; halo.shadow_blur = style.glow; halo.shadow_x = halo.shadow_y = 0; memcpy(halo.shadow_color, look.fill, 4);
        draw_shadow(p, cmd, frame, v->model, v->bounds.width, v->bounds.height, v->kind == FL_WIDGET_ELLIPSE ? 1e9 : style.radius, &halo, v->opacity);
    }
    if (part != PART_BODY && boxed && style.shadow_blur > 0 && style.shadow_color[3])
        draw_shadow(p, cmd, frame, v->model, v->bounds.width, v->bounds.height, v->kind == FL_WIDGET_ELLIPSE ? 1e9 : style.radius, &style, v->opacity);
    if (part == PART_SHADOW) return;
    if (style.blend) {
        int pipe = style.blend == 1 ? PIPE_ADD : style.blend == 2 ? PIPE_MUL : style.blend == 3 ? PIPE_SCREEN : PIPE_FLAT;
        VkPipeline bound = pipeline_for(p, p->pass_format, pipe);
        if (bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
    }
    VkDescriptorSet mask = white;
    if (v->kind == FL_WIDGET_TEXT) {
        mask = VK_NULL_HANDLE;
        Fl_CachedText *cached = NULL;
        if (p->text_scene == scene->id) for (size_t j = 0; j < p->text_count; ++j) if (p->texts[j].entity == e->id) cached = &p->texts[j];
        if (cached && cached->glyph_count && state && state->entities[index].has_motion) {
            double seconds = (double)state->tick / (double)FL_TICKS_PER_SECOND;
            double scale = p->text_scale > 0 ? p->text_scale : 1;
            const Fl_TextMotion *motion = &state->entities[index].motion;
            for (int g = 0; g < cached->glyph_count; ++g) {
                double u = motion->duration > 0 ? (seconds - motion->delay - g * motion->stagger) / motion->duration : 1;
                u = u < 0 ? 0 : u > 1 ? 1 : u; u = u * u * (3 - 2 * u);
                double dy = motion->rise * (1 - u);
                double fade = motion->fade == 0 ? 1 : u * motion->fade + (1 - motion->fade);
                Fl_Appearance glyph = look; glyph.fill[3] = (unsigned char)lround(glyph.fill[3] * fade);
                Fl_Mat4 placed = fl_mat4_mul(v->model, fl_mat4_translation(cached->glyph_box[g][0] / scale, cached->glyph_box[g][1] / scale + dy, 0));
                VkDescriptorSet glyph_mask = fl_gpu_image_descriptor(cached->glyphs[g]);
                if (glyph_mask) fl_scene_renderer_model(p, cmd, frame, placed, cached->glyph_box[g][2] / scale, cached->glyph_box[g][3] / scale, (Fl_WidgetKind)5, glyph, glyph_mask, NULL);
            }
            if (style.blend) { VkPipeline flat = pipeline_for(p, p->pass_format, p->pass_depth ? PIPE_DEPTH : PIPE_FLAT); if (flat) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flat); }
            return;
        }
        if (cached) mask = fl_gpu_image_descriptor(cached->image);
    } else if (v->kind == FL_WIDGET_PATH) {
        mask = VK_NULL_HANDLE;
        for (size_t j = 0; j < p->path_count; ++j) if (p->paths[j].entity == e->id) mask = fl_gpu_image_descriptor(p->paths[j].image);
        if (mask) fl_scene_renderer_model(p, cmd, frame, v->model, v->bounds.width, v->bounds.height, (Fl_WidgetKind)5, look, mask, NULL);
        if (style.blend) { VkPipeline flat = pipeline_for(p, p->pass_format, p->pass_depth ? PIPE_DEPTH : PIPE_FLAT); if (flat) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flat); }
        return;
    } else if (v->kind == FL_WIDGET_EFFECT && state && state->entities[index].has_effect) {
        float push[PUSH_FLOATS];
        push_block(push, frame, fl_affine_from_mat4(v->model), v->bounds.width, v->bounds.height, (Fl_WidgetKind)21, look, &shape);
        push[19] = 21;
        for (int u = 0; u < 4; ++u) { push[20 + u] = (float)state->entities[index].effect_u[u]; push[24 + u] = (float)state->entities[index].effect_u[u + 4]; }
        push[28] = state ? (float)((double)state->tick / (double)FL_TICKS_PER_SECOND) : 0;
        push[29] = (float)state->entities[index].effect_kind;
        bool finite = true;
        for (size_t c = 0; c < PUSH_FLOATS; ++c) if (!isfinite(push[c])) finite = false;
        if (finite) {
            use_model(p, v->model);
            VkPipeline custom = VK_NULL_HANDLE;
            if (state->entities[index].effect_kind == FL_EFFECT_CUSTOM && state->entities[index].effect_source)
                custom = effect_pipeline(p, p->pass_format, p->pass_depth, state->entities[index].effect_source, state->entities[index].effect_source_size);
            if (custom) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, custom);
            fl_scene_renderer_push(p, cmd, push, white);
            VkPipeline flat = pipeline_for(p, p->pass_format, p->pass_depth ? PIPE_DEPTH : PIPE_FLAT);
            if (flat) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flat);
        }
        return;
    } else if (v->kind == FL_WIDGET_IMAGE || v->kind == FL_WIDGET_VIDEO) {
        mask = v->kind == FL_WIDGET_IMAGE ? picture_descriptor(p, e) : video_descriptor(p, e->id);
        if (mask) fl_scene_renderer_model(p, cmd, frame, v->model, v->bounds.width, v->bounds.height, (Fl_WidgetKind)KIND_IMAGE, picture, mask, &shape);
        else fl_scene_renderer_model(p, cmd, frame, v->model, v->bounds.width, v->bounds.height, FL_WIDGET_RECTANGLE, placeholder, white, &shape);
        return;
    }
    if (mask) fl_scene_renderer_model(p, cmd, frame, v->model, v->bounds.width, v->bounds.height, v->kind, look, mask, boxed || fl_widget_is_polygon(v->kind) ? &shape : NULL);
    if (style.blend) { VkPipeline flat = pipeline_for(p, p->pass_format, p->pass_depth ? PIPE_DEPTH : PIPE_FLAT); if (flat) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flat); }
}

static uint64_t fnv_bytes(const void *data, size_t size)
{
    uint64_t h = 14695981039346656037ull;
    const unsigned char *p = data;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static VkPipeline effect_pipeline(Fl_SceneRenderer *p, VkFormat format, bool depth, const char *source, size_t size)
{
    uint64_t hash = fnv_bytes(source, size) ^ (uint64_t)format ^ (depth ? 1 : 0);
    for (size_t i = 0; i < p->effect_count; ++i) if (p->effects[i].hash == hash) return p->effects[i].pipeline;
    if (p->effect_count == 8 || size > 65536 || !p->layout) return VK_NULL_HANDLE;
    char *fs = malloc(size + 512);
    if (!fs) return VK_NULL_HANDLE;
    int wrote = snprintf(fs, size + 512,
        "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; vec4 fill2; vec4 extra; vec4 mid; } p;"
        "layout(set=0,binding=0) uniform sampler2D mask;layout(location=0) in vec2 uv;layout(location=0) out vec4 color;\n%.*s\n"
        "void main(){color=effect(uv,p.mid.x,p.fill2,p.extra);}", (int)size, source);
    const char *vs = "#version 450\nlayout(set=1,binding=0) uniform M { mat4 mvp; } u;"
        "layout(push_constant) uniform P { vec4 a; vec4 b; vec4 fill; vec4 stroke; vec4 params; vec4 fill2; vec4 extra; vec4 mid; } p;"
        "layout(location=0) out vec2 uv;vec2 v[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));"
        "void main(){uv=v[gl_VertexIndex];gl_Position=u.mvp*vec4(uv*p.params.xy,0.,1.);}";
    VkShaderModule vert = wrote > 0 ? p->gpu.compile_shader(p->gpu.host, vs, VK_SHADER_STAGE_VERTEX_BIT) : VK_NULL_HANDLE;
    VkShaderModule frag = vert ? p->gpu.compile_shader(p->gpu.host, fs, VK_SHADER_STAGE_FRAGMENT_BIT) : VK_NULL_HANDLE;
    free(fs);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vert && frag) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo depth_state = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = depth, .depthWriteEnable = depth, .depthCompareOp = VK_COMPARE_OP_LESS};
        VkPipelineColorBlendAttachmentState blend = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 15};
        VkPipelineColorBlendStateCreateInfo blends = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = states};
        VkFormat depth_format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
        VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format, .depthAttachmentFormat = depth_format};
        VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
            .pInputAssemblyState = &assembly, .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples, .pDepthStencilState = depth ? &depth_state : NULL,
            .pColorBlendState = &blends, .pDynamicState = &dynamic, .layout = p->layout};
        if (vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
    }
    if (vert) vkDestroyShaderModule(p->device, vert, NULL);
    if (frag) vkDestroyShaderModule(p->device, frag, NULL);
    if (!pipeline) return VK_NULL_HANDLE;
    p->effects[p->effect_count++] = (Fl_EffectPipe){hash, format, depth, pipeline};
    return pipeline;
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

static void begin_pass(Fl_SceneRenderer *p, VkCommandBuffer cmd, VkImageView view, VkImageView depth, uint32_t width, uint32_t height, bool clear, bool clear_depth)
{
    fl_scene_renderer_sync_uniforms(p, cmd);
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color = {.float32 = {0, 0, 0, 0}}};
    VkRenderingAttachmentInfo depth_info = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.depthStencil.depth = 1};
    VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {width, height}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment,
        .pDepthAttachment = depth ? &depth_info : NULL};
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
static void layer_push(float push[PUSH_FLOATS], const Fl_RenderTarget *t, int kind)
{
    memset(push, 0, PUSH_FLOATS * sizeof(float));
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
        begin_pass(p, cmd, fl_gpu_image_view(to), VK_NULL_HANDLE, t->width, t->height, true, false);
        float push[PUSH_FLOATS];
        layer_push(push, t, KIND_BLUR);
        push[24] = (float)fmin(sigma, MAX_BLUR_TAPS / 3.0); push[25] = pass ? 0.f : 1.f; push[26] = pass ? 1.f : 0.f;
        bool ok = fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, t->width, t->height, false);
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
        begin_pass(p, cmd, t->view, t->depth, t->width, t->height, false, false);
        return fl_scene_renderer_begin(p, cmd, t->format, t->width, t->height, t->depth != VK_NULL_HANDLE);
    }
    Fl_GpuImage *below = layer_image(p, t, level - 1);
    begin_pass(p, cmd, fl_gpu_image_view(below), VK_NULL_HANDLE, t->width, t->height, false, false);
    return fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, t->width, t->height, false);
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
    float push[PUSH_FLOATS];
    layer_push(push, t, KIND_COMPOSITE);
    const double *i = v->inverse.v, kx = frame->width / t->width, ky = frame->height / t->height;
    push[8] = (float)(i[0] * kx); push[9] = (float)(i[1] * ky); push[10] = (float)(i[0] * frame->x + i[1] * frame->y + i[2]); push[11] = (float)v->bounds.width;
    push[12] = (float)(i[3] * kx); push[13] = (float)(i[4] * ky); push[14] = (float)(i[3] * frame->x + i[4] * frame->y + i[5]); push[15] = (float)v->bounds.height;
    push[20] = (float)fmin(style->radius, fmin(v->bounds.width, v->bounds.height) * .5); push[21] = style->clip ? 1.f : 0.f; push[22] = v->kind == FL_WIDGET_ELLIPSE ? 1.f : 0.f;
    for (size_t k = 0; k < PUSH_FLOATS; ++k) if (!isfinite(push[k])) return;
    fl_scene_renderer_push(p, cmd, push, fl_gpu_image_descriptor(layer));
}

static void mesh_vert(float *out, uint32_t *n, float x, float y, float z, float nx, float ny, float nz)
{
    float *d = out + (size_t)(*n) * 6; d[0] = x; d[1] = y; d[2] = z; d[3] = nx; d[4] = ny; d[5] = nz; ++*n;
}
static void mesh_quad(float *out, uint32_t *n, const float a[3], const float b[3], const float c[3], const float d[3], const float normal[3])
{
    mesh_vert(out, n, a[0], a[1], a[2], normal[0], normal[1], normal[2]);
    mesh_vert(out, n, b[0], b[1], b[2], normal[0], normal[1], normal[2]);
    mesh_vert(out, n, c[0], c[1], c[2], normal[0], normal[1], normal[2]);
    mesh_vert(out, n, a[0], a[1], a[2], normal[0], normal[1], normal[2]);
    mesh_vert(out, n, c[0], c[1], c[2], normal[0], normal[1], normal[2]);
    mesh_vert(out, n, d[0], d[1], d[2], normal[0], normal[1], normal[2]);
}

static bool ensure_mesh(Fl_SceneRenderer *p, VkFormat format)
{
    if (!p->mesh_buffer) {
        enum { STACKS = 8, SLICES = 12 };
        float verts[700 * 6]; uint32_t n = 0;
        const float plane[4][3] = {{-.5f, -.5f, 0}, {.5f, -.5f, 0}, {.5f, .5f, 0}, {-.5f, .5f, 0}}, up[3] = {0, 0, 1};
        p->mesh_plane = 0; mesh_quad(verts, &n, plane[0], plane[1], plane[2], plane[3], up); p->mesh_plane_count = n;
        p->mesh_cube = n;
        const float s = .5f;
        const float corners[8][3] = {{-s,-s,-s},{s,-s,-s},{s,s,-s},{-s,s,-s},{-s,-s,s},{s,-s,s},{s,s,s},{-s,s,s}};
        const int faces[6][4] = {{4,5,6,7},{1,0,3,2},{3,7,6,2},{0,4,7,3},{1,2,6,5},{0,1,5,4}};
        const float normals[6][3] = {{0,0,1},{0,0,-1},{0,1,0},{0,-1,0},{1,0,0},{-1,0,0}};
        for (int f = 0; f < 6; ++f) mesh_quad(verts, &n, corners[faces[f][0]], corners[faces[f][1]], corners[faces[f][2]], corners[faces[f][3]], normals[f]);
        p->mesh_cube_count = n - p->mesh_cube; p->mesh_sphere = n;
        for (int stack = 0; stack < STACKS; ++stack) for (int slice = 0; slice < SLICES; ++slice) {
            float p0 = (float)stack / STACKS * 3.14159265f, p1 = (float)(stack + 1) / STACKS * 3.14159265f;
            float t0 = (float)slice / SLICES * 6.2831853f, t1 = (float)(slice + 1) / SLICES * 6.2831853f;
            float q[4][3], normal[3];
            float phi[4] = {p0, p0, p1, p1}, theta[4] = {t0, t1, t1, t0};
            for (int k = 0; k < 4; ++k) {
                float nx = sinf(phi[k]) * cosf(theta[k]), ny = cosf(phi[k]), nz = sinf(phi[k]) * sinf(theta[k]);
                q[k][0] = nx * .5f; q[k][1] = ny * .5f; q[k][2] = nz * .5f;
                if (!k) {normal[0] = nx; normal[1] = ny; normal[2] = nz;}
            }
            mesh_quad(verts, &n, q[0], q[1], q[2], q[3], normal);
        }
        p->mesh_sphere_count = n - p->mesh_sphere;
        VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)n * 6 * sizeof(float), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        VmaAllocationCreateInfo host = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo mapped = {0};
        if (vmaCreateBuffer(p->gpu.allocator, &buffer, &host, &p->mesh_buffer, &p->mesh_memory, &mapped) != VK_SUCCESS) return false;
        memcpy(mapped.pMappedData, verts, (size_t)n * 6 * sizeof(float));
        vmaFlushAllocation(p->gpu.allocator, p->mesh_memory, 0, VK_WHOLE_SIZE);
    }
    for (size_t i = 0; i < p->pipeline_count; ++i) if (p->pipelines[i].format == format && p->pipelines[i].kind == PIPE_MESH) return true;
    if (p->pipeline_count == PIPELINE_FORMATS || !ensure_ubo(p) || !p->layout) return false;
    const char *vs = "#version 450\nlayout(location=0) in vec3 pos;layout(location=1) in vec3 normal;"
        "layout(set=1,binding=0) uniform M{mat4 mvp;mat4 model;vec4 base;vec4 shade;vec4 lp0;vec4 lc0;vec4 lp1;vec4 lc1;vec4 lp2;vec4 lc2;}u;"
        "layout(location=0) out vec3 vN;layout(location=1) out vec3 vW;void main(){vN=mat3(transpose(inverse(u.model)))*normal;vW=(u.model*vec4(pos,1)).xyz;gl_Position=u.mvp*vec4(pos,1);}";
    const char *fs = "#version 450\nlayout(location=0) in vec3 vN;layout(location=1) in vec3 vW;"
        "layout(set=1,binding=0) uniform M{mat4 mvp;mat4 model;vec4 base;vec4 shade;vec4 lp0;vec4 lc0;vec4 lp1;vec4 lc1;vec4 lp2;vec4 lc2;}u;"
        "layout(location=0) out vec4 color;vec3 lit_one(vec4 lp,vec4 lc,vec3 n){vec3 ldir;float att=1.;if(lp.w<.5)ldir=normalize(lp.xyz);else{vec3 to=lp.xyz-vW;float d=length(to);ldir=to/max(d,.0001);att=clamp(1.-d/4000.,0.,1.);att*=att;}"
        "float rough=clamp(u.shade.x,.04,1.);float metal=clamp(u.shade.y,0.,1.);vec3 h=normalize(ldir+vec3(0.,0.,1.));float spec=pow(clamp(dot(n,h),0.,1.),mix(96.,6.,rough));"
        "vec3 F0=mix(vec3(.04),u.base.rgb,metal);return (mix(u.base.rgb,vec3(0.),metal)*(vec3(1.)-F0)*clamp(dot(n,ldir),0.,1.)+F0*spec)*lc.rgb*lc.w*att;}"
        "void main(){if(u.shade.x<0.){color=vec4(0.,0.,0.,.4*u.base.a);return;}vec3 n=normalize(vN);vec3 hemi=mix(vec3(.05,.06,.08),vec3(.62,.64,.68),clamp(n.y*.5+.5,0.,1.));"
        "vec3 lit=u.base.rgb*hemi*.45;int count=int(u.shade.w+.5);"
        "if(count==0)lit+=u.base.rgb*clamp(dot(n,vec3(0.,0.,1.)),0.,1.);else{if(count>0)lit+=lit_one(u.lp0,u.lc0,n);if(count>1)lit+=lit_one(u.lp1,u.lc1,n);if(count>2)lit+=lit_one(u.lp2,u.lc2,n);}lit+=u.base.rgb*u.shade.z;color=vec4(lit,u.base.a);}";
    VkShaderModule vert = p->gpu.compile_shader(p->gpu.host, vs, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = p->gpu.compile_shader(p->gpu.host, fs, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPipeline pipeline = VK_NULL_HANDLE; bool ok = vert && frag;
    if (ok) {
        VkVertexInputBindingDescription binding = {0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
        VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs};
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
        VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS};
        VkPipelineColorBlendAttachmentState blend = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 15};
        VkPipelineColorBlendStateCreateInfo blends = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = states};
        VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
        VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format, .depthAttachmentFormat = depth_format};
        VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
            .pInputAssemblyState = &assembly, .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples, .pDepthStencilState = &depth,
            .pColorBlendState = &blends, .pDynamicState = &dynamic, .layout = p->layout};
        ok = vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) == VK_SUCCESS;
    }
    if (vert) vkDestroyShaderModule(p->device, vert, NULL);
    if (frag) vkDestroyShaderModule(p->device, frag, NULL);
    if (!ok) return false;
    p->pipelines[p->pipeline_count++] = (Fl_Pipeline){format, PIPE_MESH, pipeline};
    return true;
}

static void draw_meshes(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_Scene *scene, const Fl_SceneState *state, const Fl_SceneGeometry *g)
{
    if (!g || !p->pass_depth || !ensure_mesh(p, p->pass_format)) return;
    VkPipeline mesh = VK_NULL_HANDLE;
    for (size_t i = 0; i < p->pipeline_count; ++i) if (p->pipelines[i].format == p->pass_format && p->pipelines[i].kind == PIPE_MESH) mesh = p->pipelines[i].pipeline;
    if (!mesh) return;
    float lights[3][8] = {0}; int light_count = 0;
    for (size_t i = 0; i < g->count && light_count < 3; ++i) {
        bool has = state && i < state->count ? state->entities[i].has_light : fl_entity_component(&scene->entities[i], FL_COMPONENT_LIGHT) != NULL;
        if (!has || !g->entities[i].visible) continue;
        Fl_Light light = state && state->entities[i].has_light ? state->entities[i].light : fl_light_default();
        if (!(state && state->entities[i].has_light)) fl_light_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_LIGHT), &light);
        const Fl_Mat4 *m = &g->entities[i].model;
        if (light.kind == FL_LIGHT_DIRECTIONAL) {lights[light_count][0] = (float)m->m[8]; lights[light_count][1] = (float)m->m[9]; lights[light_count][2] = (float)m->m[10]; lights[light_count][3] = 0;}
        else {lights[light_count][0] = (float)m->m[12]; lights[light_count][1] = (float)m->m[13]; lights[light_count][2] = (float)m->m[14]; lights[light_count][3] = light.kind == FL_LIGHT_SPOT ? 2.f : 1.f;}
        lights[light_count][4] = fl_scene_renderer_linear(light.color[0]); lights[light_count][5] = fl_scene_renderer_linear(light.color[1]);
        lights[light_count][6] = fl_scene_renderer_linear(light.color[2]); lights[light_count][7] = (float)light.intensity;
        ++light_count;
    }
    bool any = false;
    for (size_t i = 0; i < g->count; ++i) if (g->entities[i].mesh && g->entities[i].visible && g->entities[i].opacity > 0) any = true;
    if (!any) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh);
    VkDeviceSize zero = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &p->mesh_buffer, &zero);
    for (size_t i = 0; i < g->count; ++i) {
        const Fl_ResolvedEntity *v = &g->entities[i];
        if (!v->mesh || !v->visible || v->opacity <= 0) continue;
        Fl_Material material = fl_material_default();
        Fl_MeshShape shape = FL_MESH_CUBE;
        if (state && i < state->count && state->entities[i].has_material) material = state->entities[i].material;
        else fl_material_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_MATERIAL), &material);
        if (state && i < state->count && state->entities[i].has_mesh) shape = state->entities[i].mesh;
        else fl_mesh_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_MESH), &shape);
        if (shape != FL_MESH_PLANE) {
            Fl_Mat4 blob = fl_mat4_mul(fl_mat4_translation(v->model.m[12], v->model.m[13], v->model.m[14] - fmax(v->bounds.depth, 1) * .5 - 1), fl_mat4_scale(v->bounds.width, v->bounds.height, 1));
            float shadow[64] = {0};
            fl_mat4_float(fl_mat4_mul(p->view_proj, blob), shadow); fl_mat4_float(blob, shadow + 16);
            shadow[35] = (float)v->opacity; shadow[36] = -1;
            uint32_t shadow_offset = upload_uniform(p, shadow, sizeof(shadow));
            if (shadow_offset != UINT32_MAX) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &p->ubo_set, 1, &shadow_offset);
                vkCmdDraw(cmd, p->mesh_plane_count, 1, p->mesh_plane, 0);
            }
        }
        float block[64] = {0};
        fl_mat4_float(fl_mat4_mul(p->view_proj, v->model), block);
        fl_mat4_float(v->model, block + 16);
        block[32] = fl_scene_renderer_linear(material.base[0]); block[33] = fl_scene_renderer_linear(material.base[1]);
        block[34] = fl_scene_renderer_linear(material.base[2]); block[35] = material.base[3] / 255.f * (float)v->opacity;
        block[36] = (float)material.roughness; block[37] = (float)material.metallic; block[38] = (float)material.emissive; block[39] = (float)light_count;
        memcpy(block + 40, lights, sizeof(lights));
        uint32_t offset = upload_uniform(p, block, sizeof(block));
        if (offset == UINT32_MAX) continue;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &p->ubo_set, 1, &offset);
        uint32_t first = shape == FL_MESH_PLANE ? p->mesh_plane : shape == FL_MESH_SPHERE ? p->mesh_sphere : p->mesh_cube;
        uint32_t count = shape == FL_MESH_PLANE ? p->mesh_plane_count : shape == FL_MESH_SPHERE ? p->mesh_sphere_count : p->mesh_cube_count;
        vkCmdDraw(cmd, count, 1, first, 0);
    }
    pipeline_for(p, p->pass_format, PIPE_DEPTH);
    VkPipeline flat = VK_NULL_HANDLE;
    for (size_t i = 0; i < p->pipeline_count; ++i) if (p->pipelines[i].format == p->pass_format && p->pipelines[i].kind == PIPE_DEPTH) flat = p->pipelines[i].pipeline;
    if (flat) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flat);
}

static void draw_particles(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_Scene *scene, const Fl_SceneState *state, const Fl_SceneGeometry *g)
{
    if (!g || !p->pass_depth) return;
    VkPipeline pipe = pipeline_for(p, p->pass_format, PIPE_PARTICLE);
    if (!pipe) return;
    bool any = false;
    for (size_t i = 0; i < g->count; ++i) {
        bool has = state && i < state->count ? state->entities[i].has_particles : fl_entity_component(&scene->entities[i], FL_COMPONENT_PARTICLES) != NULL;
        if (has && g->entities[i].visible && g->entities[i].opacity > 0) any = true;
    }
    if (!any) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    double seconds = state ? (double)state->tick / (double)FL_TICKS_PER_SECOND : 0;
    Fl_Particle samples[1024];
    for (size_t i = 0; i < g->count; ++i) {
        const Fl_ResolvedEntity *v = &g->entities[i];
        Fl_Particles system;
        bool has = state && i < state->count && state->entities[i].has_particles;
        if (has) system = state->entities[i].particles;
        else if (!fl_particles_decode(fl_entity_component(&scene->entities[i], FL_COMPONENT_PARTICLES), &system)) continue;
        if (!v->visible || v->opacity <= 0) continue;
        VkPipeline batch = pipeline_for(p, p->pass_format, system.blend > 0.5 ? PIPE_ADD : PIPE_PARTICLE);
        if (batch) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, batch);
        size_t count = fl_particles_sample(&system, seconds, samples, 1024);
        for (size_t n = 0; n < count; ++n) {
            unsigned char alpha = (unsigned char)lround(samples[n].color[3] * v->opacity);
            Fl_Appearance look = {.fill = {samples[n].color[0], samples[n].color[1], samples[n].color[2], alpha}};
            Fl_Vec3 center = fl_mat4_point(v->model, (Fl_Vec3){samples[n].x, samples[n].y, samples[n].z});
            double s = samples[n].size; Fl_Vec3 right = p->cam_right, up = p->cam_up;
            Fl_Mat4 model = fl_mat4_identity();
            model.m[0] = right.x; model.m[1] = right.y; model.m[2] = right.z;
            model.m[4] = up.x; model.m[5] = up.y; model.m[6] = up.z;
            model.m[12] = center.x - (right.x + up.x) * s * .5; model.m[13] = center.y - (right.y + up.y) * s * .5; model.m[14] = center.z - (right.z + up.z) * s * .5;
            float push[PUSH_FLOATS]; push_block(push, frame, fl_affine_identity(), s, s, (Fl_WidgetKind)22, look, NULL); push[19] = 22;
            bool finite = true;
            for (size_t c = 0; c < PUSH_FLOATS; ++c) if (!isfinite(push[c])) finite = false;
            if (!finite) continue;
            fl_mat4_float(fl_mat4_mul(p->view_proj, model), p->draw_mvp);
            fl_scene_renderer_push(p, cmd, push, fl_scene_renderer_white(p));
        }
    }
    VkPipeline flat = pipeline_for(p, p->pass_format, PIPE_DEPTH);
    if (flat) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flat);
}

static bool ensure_bloom_images(Fl_SceneRenderer *p, uint32_t width, uint32_t height)
{
    if (p->bloom_width == width && p->bloom_height == height && p->bloom_image[0][0]) return true;
    vkDeviceWaitIdle(p->device);
    for (size_t s = 0; s < LAYER_SLOTS; ++s) for (int i = 0; i < 2; ++i) {fl_gpu_image_destroy(p->bloom_image[s][i]); p->bloom_image[s][i] = NULL;}
    p->bloom_width = width; p->bloom_height = height;
    return true;
}

static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

/* One bloom source set per frame slot. A slot is recorded only after its previous submit has finished. */
static bool ensure_sample_sets(Fl_SceneRenderer *p)
{
    if (p->sample_set[0]) return true;
    VkDescriptorSetLayout layout = fl_gpu_images_layout(p->images);
    VkDescriptorSetLayout layouts[LAYER_SLOTS];
    for (size_t i = 0; i < LAYER_SLOTS; ++i) layouts[i] = layout;
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, LAYER_SLOTS};
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = LAYER_SLOTS, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    if (vkCreateDescriptorPool(p->device, &pool, NULL, &p->sample_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo sets = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p->sample_pool,
        .descriptorSetCount = LAYER_SLOTS, .pSetLayouts = layouts};
    if (vkAllocateDescriptorSets(p->device, &sets, p->sample_set) != VK_SUCCESS) {
        vkDestroyDescriptorPool(p->device, p->sample_pool, NULL);
        p->sample_pool = VK_NULL_HANDLE;
        memset(p->sample_set, 0, sizeof(p->sample_set));
        return false;
    }
    return true;
}

static void apply_bloom(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_RenderTarget *t)
{
    if (!p->bloom || p->bloom <= 0 || !t->image || !t->depth) return;
    size_t slot = t->slot % LAYER_SLOTS;
    if (!ensure_bloom_images(p, t->width, t->height)) return;
    if (!p->bloom_image[slot][0]) p->bloom_image[slot][0] = fl_gpu_target_create(p->images, LAYER_FORMAT, t->width, t->height);
    if (!p->bloom_image[slot][1]) p->bloom_image[slot][1] = fl_gpu_target_create(p->images, LAYER_FORMAT, t->width, t->height);
    if (!p->bloom_image[slot][0] || !ensure_sample_sets(p)) return;
    vkCmdEndRendering(cmd);
    transition(cmd, t->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkDescriptorImageInfo sampled = {fl_gpu_images_sampler(p->images), t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->sample_set[slot], .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sampled};
    vkUpdateDescriptorSets(p->device, 1, &write, 0, NULL);
    layer_transition(cmd, fl_gpu_image_handle(p->bloom_image[slot][0]), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_pass(p, cmd, fl_gpu_image_view(p->bloom_image[slot][0]), VK_NULL_HANDLE, t->width, t->height, true, false);
    if (fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, t->width, t->height, false)) {
        float push[PUSH_FLOATS] = {0}; layer_push(push, t, 20); push[24] = .45f;
        fl_mat4_float(fl_mat4_identity(), p->draw_mvp);
        fl_scene_renderer_push(p, cmd, push, p->sample_set[slot]);
    }
    vkCmdEndRendering(cmd);
    layer_transition(cmd, fl_gpu_image_handle(p->bloom_image[slot][0]), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    blur_layer(p, cmd, t, p->bloom_image[slot][0], fmin(12.0, 4.0 + p->bloom * 4.0));
    transition(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    begin_pass(p, cmd, t->view, t->depth, t->width, t->height, false, false);
    VkPipeline add = pipeline_for(p, t->format, PIPE_ADD);
    if (add && fl_scene_renderer_begin(p, cmd, t->format, t->width, t->height, true)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, add);
        float push[PUSH_FLOATS] = {0}; layer_push(push, t, 19); push[24] = (float)fmin(p->bloom, 4.0);
        fl_mat4_float(fl_mat4_identity(), p->draw_mvp);
        fl_scene_renderer_push(p, cmd, push, fl_gpu_image_descriptor(p->bloom_image[slot][0]));
        fl_scene_renderer_begin(p, cmd, t->format, t->width, t->height, true);
    }
}

static void apply_grade(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_RenderTarget *t)
{
    bool active = p->vignette > 0 || p->grain > 0 || p->lift != 0 || fabs(p->gamma - 1) > 1e-3 || fabs(p->gain - 1) > 1e-3;
    if (!active || !t->image || !t->depth) return;
    size_t slot = t->slot % LAYER_SLOTS;
    if (!ensure_bloom_images(p, t->width, t->height) || !ensure_sample_sets(p)) return;
    if (!p->bloom_image[slot][1]) p->bloom_image[slot][1] = fl_gpu_target_create(p->images, LAYER_FORMAT, t->width, t->height);
    if (!p->bloom_image[slot][1]) return;
    vkCmdEndRendering(cmd);
    transition(cmd, t->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkDescriptorImageInfo sampled = {fl_gpu_images_sampler(p->images), t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->sample_set[slot], .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sampled};
    vkUpdateDescriptorSets(p->device, 1, &write, 0, NULL);
    layer_transition(cmd, fl_gpu_image_handle(p->bloom_image[slot][1]), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_pass(p, cmd, fl_gpu_image_view(p->bloom_image[slot][1]), VK_NULL_HANDLE, t->width, t->height, true, false);
    if (fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, t->width, t->height, false)) {
        float push[PUSH_FLOATS] = {0}; layer_push(push, t, 23);
        push[8] = (float)(p->gamma > 0 ? p->gamma : 1); push[9] = (float)(p->gain > 0 ? p->gain : 1);
        push[24] = (float)p->vignette; push[25] = (float)p->grain; push[26] = (float)p->lift;
        fl_mat4_float(fl_mat4_identity(), p->draw_mvp);
        fl_scene_renderer_push(p, cmd, push, p->sample_set[slot]);
    }
    vkCmdEndRendering(cmd);
    layer_transition(cmd, fl_gpu_image_handle(p->bloom_image[slot][1]), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    begin_pass(p, cmd, t->view, t->depth, t->width, t->height, true, false);
    if (fl_scene_renderer_begin(p, cmd, t->format, t->width, t->height, true)) {
        float push[PUSH_FLOATS] = {0}; layer_push(push, t, 24);
        fl_mat4_float(fl_mat4_identity(), p->draw_mvp);
        fl_scene_renderer_push(p, cmd, push, fl_gpu_image_descriptor(p->bloom_image[slot][1]));
    }
}

void fl_scene_renderer_entities(Fl_SceneRenderer *p, VkCommandBuffer cmd, const Fl_RenderTarget *target, const Fl_SceneFrame *frame,
                                const Fl_Scene *scene, const Fl_SceneState *state, const Fl_SceneGeometry *g)
{
    scene=fl_scene_state_scene(state,scene);
    p->vignette = p->grain = p->lift = 0; p->gamma = 1; p->gain = 1;
    if (state && state->valid) for (size_t i = 0; i < state->count && i < scene->entity_count; ++i) if (state->entities[i].has_camera && scene->entities[i].visible) {
        if (state->entities[i].has_post) {
            p->vignette = state->entities[i].post.vignette; p->grain = state->entities[i].post.grain; p->lift = state->entities[i].post.lift;
            p->gamma = state->entities[i].post.gamma > 0 ? state->entities[i].post.gamma : 1; p->gain = state->entities[i].post.gain;
        }
        break;
    }
    size_t *parent = g->count ? malloc(g->count * sizeof(*parent)) : NULL;
    for (size_t i = 0; parent && i < g->count; ++i) {
        const Fl_Entity *above = fl_scene_entity(scene, scene->entities[i].parent);
        parent[i] = above ? (size_t)(above - scene->entities) : SIZE_MAX;
    }
    struct {size_t entity, end; Fl_Style style;} open[LAYER_DEPTH];
    size_t depth = 0;
    ensure_layers(p, target);
    if (target->depth) {draw_meshes(p, cmd, scene, state, g); draw_particles(p, cmd, frame, scene, state, g);}
    for (size_t k = 0; k < g->count; ++k) {
        while (depth && k >= open[depth - 1].end) {
            --depth; close_layer(p, cmd, target, frame, depth, &g->entities[open[depth].entity], &open[depth].style);
        }
        size_t index = g->order[k];
        const Fl_ResolvedEntity *v = &g->entities[index];
        Fl_Style style = state ? state->entities[index].style : fl_entity_style(&scene->entities[index]);
        bool layered = parent && (style.clip || style.blur > 0) && v->visible && v->invertible && v->opacity > 0 && depth < LAYER_DEPTH &&
                       layer_image(p, target, depth);
        if (!layered) {draw_entity(p, cmd, frame, scene, state, g, index, PART_ALL); continue;}
        draw_entity(p, cmd, frame, scene, state, g, index, PART_SHADOW);
        Fl_GpuImage *layer = layer_image(p, target, depth);
        vkCmdEndRendering(cmd);
        layer_transition(cmd, fl_gpu_image_handle(layer), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        begin_pass(p, cmd, fl_gpu_image_view(layer), VK_NULL_HANDLE, target->width, target->height, true, false);
        fl_scene_renderer_begin(p, cmd, LAYER_FORMAT, target->width, target->height, false);
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
    apply_bloom(p, cmd, target);
    apply_grade(p, cmd, target);
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
    p->view_proj = fl_mat4_identity(); p->cam_right = (Fl_Vec3){1, 0, 0}; p->cam_up = (Fl_Vec3){0, 1, 0};
    fl_mat4_float(p->view_proj, p->draw_mvp);
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
    for (size_t i=0;i<p->video_count;++i) if (p->videos[i].decode) atomic_store_explicit(&p->videos[i].decode->cancel,true,memory_order_relaxed);
    for (size_t i=0;i<p->video_count;++i) if (p->videos[i].decode) {fl_job_wait(p->jobs,p->videos[i].job); release_video_decode(&p->videos[i]);}
    vkDeviceWaitIdle(p->device);
    for (size_t i = 0; i < p->picture_count; ++i) release_picture(&p->pictures[i]);
    for (size_t i = 0; i < p->text_count; ++i) {fl_gpu_image_destroy(p->texts[i].image); for (int g = 0; g < p->texts[i].glyph_count; ++g) fl_gpu_image_destroy(p->texts[i].glyphs[g]); free(p->texts[i].text);}
    for (size_t s = 0; s < LAYER_SLOTS; ++s) {
        for (size_t d = 0; d < LAYER_DEPTH; ++d) fl_gpu_image_destroy(p->layers[s][d]);
        fl_gpu_image_destroy(p->scratch[s]);
    }
    for (size_t i = 0; i < p->video_count; ++i) fl_gpu_image_destroy(p->videos[i].image);
    for (size_t i = 0; i < p->graveyard_count; ++i) fl_gpu_image_destroy(p->graveyard[i].image);
    free(p->videos); free(p->graveyard); free(p->pictures); free(p->texts);
    for (size_t s = 0; s < LAYER_SLOTS; ++s) for (int i = 0; i < 2; ++i) fl_gpu_image_destroy(p->bloom_image[s][i]);
    if (p->depth_view) vkDestroyImageView(p->device, p->depth_view, NULL);
    if (p->depth_image) vmaDestroyImage(p->gpu.allocator, p->depth_image, p->depth_memory);
    if (p->mesh_buffer) vmaDestroyBuffer(p->gpu.allocator, p->mesh_buffer, p->mesh_memory);
    if (p->ubo) vmaDestroyBuffer(p->gpu.allocator, p->ubo, p->ubo_memory);
    for (size_t i = 0; i < p->path_count; ++i) fl_gpu_image_destroy(p->paths[i].image);
    free(p->paths);
    for (size_t i = 0; i < p->effect_count; ++i) vkDestroyPipeline(p->device, p->effects[i].pipeline, NULL);
    for (size_t i = 0; i < p->pipeline_count; ++i) vkDestroyPipeline(p->device, p->pipelines[i].pipeline, NULL);
    if (p->layout) vkDestroyPipelineLayout(p->device, p->layout, NULL);
    if (p->sample_pool) vkDestroyDescriptorPool(p->device, p->sample_pool, NULL);
    if (p->ubo_pool) vkDestroyDescriptorPool(p->device, p->ubo_pool, NULL);
    if (p->ubo_layout) vkDestroyDescriptorSetLayout(p->device, p->ubo_layout, NULL);
    fl_gpu_images_destroy(p->images); fl_text_rasterizer_destroy(p->rasterizer);
    if (p->owns_fonts) fl_fonts_destroy(p->fonts);
    free(p);
}

Fl_SceneRenderer *fl_scene_renderer_snapshot(Fl_SceneRenderer *source, const Fl_Project *project,
                                           const Fl_Id *scenes, size_t count, char *error, size_t capacity)
{
    if (error && capacity) error[0]=0;
    if (!source || !project || (count && !scenes)) return NULL;
    Fl_Typography *requests=NULL; size_t request_count=0,allocated=0; bool ok=true;
    for (size_t i=0;ok && i<count;++i) {
        const Fl_Scene *scene=fl_project_scene(project,scenes[i]);
        Fl_Composition *composition=scene && fl_composition_required(scene) ? fl_composition_create(scene,NULL,error,capacity) : NULL;
        if (!scene || (fl_composition_required(scene) && !composition)) {ok=false; break;}
        const Fl_Scene *flat=composition ? fl_composition_scene(composition) : scene;
        for (size_t j=0;j<flat->entity_count;++j) {
            const Fl_Entity *entity=&flat->entities[j];
            if (!fl_entity_component(entity,FL_COMPONENT_TEXT)) continue;
            if (request_count==allocated) {
                size_t next=allocated ? allocated*2 : 16;
                Fl_Typography *grown=next>=allocated && next<=SIZE_MAX/sizeof(*grown) ? realloc(requests,next*sizeof(*grown)) : NULL;
                if (!grown) {ok=false; break;}
                requests=grown; allocated=next;
            }
            requests[request_count++]=fl_widget_typography(entity);
        }
        fl_composition_destroy(composition);
    }
    Fl_Fonts *fonts=ok ? fl_fonts_snapshot(source->fonts,requests,request_count) : NULL; free(requests);
    if (!fonts) {if (error && capacity && !error[0]) snprintf(error,capacity,"Cannot capture immutable export fonts"); return NULL;}
    Fl_SceneRenderer *renderer=fl_scene_renderer_create(&(Fl_SceneRendererDesc){.gpu=source->gpu,.jobs=source->jobs,.fonts=fonts});
    if (!renderer) {fl_fonts_destroy(fonts); if (error && capacity) snprintf(error,capacity,"Cannot allocate export renderer"); return NULL;}
    renderer->owns_fonts=true; return renderer;
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

VkImageView fl_scene_renderer_depth(Fl_SceneRenderer *p, VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if (!p || !cmd || !width || !height) return VK_NULL_HANDLE;
    if (p->depth_width != width || p->depth_height != height) {
        if (p->depth_view || p->depth_image) vkDeviceWaitIdle(p->device);
        if (p->depth_view) vkDestroyImageView(p->device, p->depth_view, NULL);
        if (p->depth_image) vmaDestroyImage(p->gpu.allocator, p->depth_image, p->depth_memory);
        p->depth_view = VK_NULL_HANDLE; p->depth_image = VK_NULL_HANDLE; p->depth_ready = false;
        VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
            .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo gpu = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(p->gpu.allocator, &image, &gpu, &p->depth_image, &p->depth_memory, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = p->depth_image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(p->device, &view, NULL, &p->depth_view) != VK_SUCCESS) return VK_NULL_HANDLE;
        p->depth_width = width; p->depth_height = height;
    }
    if (!p->depth_ready) {
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = p->depth_image,
            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        p->depth_ready = true;
    }
    return p->depth_view;
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
    if (!ok && state->error[0]) snprintf(p->error,sizeof(p->error),"%s",state->error);
    if (ok && state->error[0]) {snprintf(p->error,sizeof(p->error),"%s",state->error); ok=false;}
    scene=fl_scene_state_scene(state,scene);
    for (size_t i=0; ok && i<scene->entity_count; ++i) {
        const char *diagnostic=fl_scene_state_entity_error(state,i);
        if (diagnostic[0]) {snprintf(p->error,sizeof(p->error),"%s",diagnostic); ok=false;}
    }
    ok=ok && fl_scene_geometry_update(&geometry,scene,state);
    VmaAllocator allocator = p->gpu.allocator;
    Offscreen o = {0};
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    if (ok) {
        fl_scene_renderer_prepare(p, project, scene, state,&geometry,(double)w / frame.width, true);
        VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo device_memory = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo host = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        ok = !p->video_error && vmaCreateImage(allocator, &image, &device_memory, &o.image, &o.image_memory, NULL) == VK_SUCCESS &&
             vmaCreateBuffer(allocator, &buffer, &host, &o.buffer, &o.buffer_memory, &o.buffer_info) == VK_SUCCESS;
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        ok = ok && vkCreateImageView(p->device, &view, NULL, &o.view) == VK_SUCCESS;
    }
    ok = ok && pipeline_for(p, format, PIPE_DEPTH) != VK_NULL_HANDLE;
    VkCommandBuffer cmd = ok ? p->gpu.begin_transfer(p->gpu.host) : VK_NULL_HANDLE;
    VkImageView depth = cmd ? fl_scene_renderer_depth(p, cmd, w, h) : VK_NULL_HANDLE;
    if (ok && cmd && depth) {
        transition(cmd, o.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        const unsigned char *backdrop = scene->background;
        VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = o.view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color = {.float32 = {fl_scene_renderer_linear(backdrop[0]), fl_scene_renderer_linear(backdrop[1]), fl_scene_renderer_linear(backdrop[2]), 1}}};
        VkRenderingAttachmentInfo depth_info = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.depthStencil.depth = 1};
        VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {w, h}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment, .pDepthAttachment = &depth_info};
        fl_scene_renderer_sync_uniforms(p, cmd);
        vkCmdBeginRendering(cmd, &info);
        Fl_Mat4 view; Fl_Vec3 right, up; double bloom = 0;
        fl_scene_camera(scene, state, frame.x, frame.y, frame.width, frame.height, &view, &right, &up, &bloom);
        fl_scene_renderer_set_view(p, view, right, up, bloom);
        ok = fl_scene_renderer_begin(p, cmd, format, w, h, true);
        if (ok) {
            fl_scene_renderer_background(p, cmd, &frame, &frame, scene->background);
            Fl_RenderTarget target = {.view = o.view, .format = format, .width = w, .height = h, .image = o.image, .depth = depth};
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

Fl_JobSystem *fl_scene_renderer_jobs(const Fl_SceneRenderer *renderer) {return renderer ? renderer->jobs : NULL;}
