#include "fl_scene_preview.h"
#include "style.h"
#include <ca_gpu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_Visual { double affine[6]; Fl_Transform local; bool visible, resolved; } Fl_Visual;
struct Fl_ScenePreview {
    Fl_Project *project;
    Fl_Id scene, visual_scene;
    uint64_t visual_revision;
    Fl_Visual *visuals;
    VkDevice device;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkFormat format;
};

/* Build a compact solid-colour GPU pipeline on the viewport's native format. */
static bool create_pipeline(Fl_ScenePreview *p, Ca_Viewport *viewport)
{
    if (p->pipeline) return p->format == ca_viewport_format(viewport);
    Ca_Instance *instance = ca_viewport_instance(viewport); p->device = ca_gpu_device(instance);
    const char *vs = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 color; } p;"
        "vec2 v[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));"
        "void main(){vec3 q=vec3(v[gl_VertexIndex],1);gl_Position=vec4(dot(p.a.xyz,q),dot(p.b.xyz,q),0,1);}";
    const char *fs = "#version 450\nlayout(push_constant) uniform P { vec4 a; vec4 b; vec4 color; } p;"
        "layout(location=0) out vec4 c;void main(){c=p.color;}";
    VkShaderModule vert = ca_shader_compile(instance, vs, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(instance, fs, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
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

/* Compose a parent's affine transform with a local transform, in scene coordinates. */
static void compose(double a[6], const Fl_Transform *t)
{
    double c = cos(t->rotation), s = sin(t->rotation);
    double b[6] = {c*t->scale_x, -s*t->scale_y, t->x, s*t->scale_x, c*t->scale_y, t->y};
    double r[6] = {a[0]*b[0]+a[1]*b[3], a[0]*b[1]+a[1]*b[4], a[0]*b[2]+a[1]*b[5]+a[2],
                   a[3]*b[0]+a[4]*b[3], a[3]*b[1]+a[4]*b[4], a[3]*b[2]+a[4]*b[5]+a[5]};
    memcpy(a, r, sizeof(r));
}

/* Decode sRGB component bytes before writing into an sRGB colour attachment. */
static float linear_channel(unsigned char value)
{
    float channel = value / 255.0f;
    return channel <= 0.04045f ? channel / 12.92f : powf((channel + 0.055f) / 1.055f, 2.4f);
}

/* Render rectangles into a letterboxed scene with inherited transforms and visibility. */
void fl_scene_preview_render(Ca_Viewport *viewport, void *data)
{
    Fl_ScenePreview *p = data;
    uint32_t w = ca_viewport_width(viewport), h = ca_viewport_height(viewport);
    VkCommandBuffer cmd = ca_viewport_cmd(viewport);
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ca_viewport_image_view(viewport), .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color = {.float32 = FL_STYLE_VIEW_CLEAR}};
    VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea.extent = {w,h},
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
    vkCmdBeginRendering(cmd, &info);
    const Fl_Scene *s = fl_project_scene(p->project, p->scene);
    if (s && w && h) {
        double scale = fmin((double)w / s->width, (double)h / s->height);
        uint32_t aw = (uint32_t)(s->width * scale), ah = (uint32_t)(s->height * scale);
        VkRect2D area = {{(int32_t)((w-aw)/2), (int32_t)((h-ah)/2)}, {aw,ah}};
        VkClearAttachment clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
            .clearValue.color = {.float32 = FL_STYLE_ARTBOARD_CLEAR}};
        VkClearRect rect = {.rect = area, .layerCount = 1};
        if (aw && ah) vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
        if (aw && ah && create_pipeline(p, viewport)) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
            VkViewport view = {(float)area.offset.x, (float)area.offset.y, (float)aw, (float)ah, 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &view); vkCmdSetScissor(cmd, 0, 1, &area);
            for (size_t i = 0; p->visuals && p->visual_scene == s->id && p->visual_revision == s->revision && i < s->entity_count; ++i) {
                const Fl_Entity *e = &s->entities[i];
                const Fl_Component *shape = fl_entity_component(e, FL_COMPONENT_RECTANGLE);
                const Fl_Visual *v = &p->visuals[i];
                if (!shape || !v->visible) continue;
                const double *a = v->affine;
                float push[12] = {(float)(a[0]*v->local.width*2/s->width), (float)(a[1]*v->local.height*2/s->width), (float)(a[2]*2/s->width-1), 0,
                    (float)(a[3]*v->local.width*2/s->height), (float)(a[4]*v->local.height*2/s->height), (float)(a[5]*2/s->height-1), 0,
                    linear_channel(shape->data[0]), linear_channel(shape->data[1]), linear_channel(shape->data[2]), shape->data[3]/255.f};
                bool finite = true; for (size_t j = 0; j < 12; ++j) finite &= isfinite(push[j]);
                if (!finite) continue;
                vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
                vkCmdDraw(cmd, 6, 1, 0, 0);
            }
        }
    }
    vkCmdEndRendering(cmd);
}

/* Resolve each entity's inherited transform once per document rebuild, without recursion. */
static bool resolve_visuals(Fl_ScenePreview *p, const Fl_Scene *s)
{
    if (s && p->visual_scene == s->id && p->visual_revision == s->revision) return true;
    free(p->visuals); p->visuals = NULL; p->visual_scene = 0; p->visual_revision = 0;
    if (!s || !s->entity_count) return true;
    if (s->entity_count > SIZE_MAX / sizeof(Fl_Visual) || s->entity_count > SIZE_MAX / sizeof(size_t)) return false;
    Fl_Visual *visuals = calloc(s->entity_count, sizeof(*visuals));
    size_t *chain = malloc(s->entity_count * sizeof(*chain));
    if (!visuals || !chain) { free(visuals); free(chain); return false; }
    for (size_t i = 0; i < s->entity_count; ++i) {
        size_t depth = 0; const Fl_Entity *e = &s->entities[i];
        while (e && !visuals[e - s->entities].resolved && depth < s->entity_count) {
            chain[depth++] = (size_t)(e - s->entities); e = fl_scene_entity(s, e->parent);
        }
        while (depth) {
            size_t index = chain[--depth]; e = &s->entities[index];
            Fl_Visual *v = &visuals[index]; const Fl_Entity *parent = fl_scene_entity(s, e->parent);
            v->local = (Fl_Transform){.scale_x = 1, .scale_y = 1};
            fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &v->local);
            const double identity[6] = {1,0,0,0,1,0};
            memcpy(v->affine, parent ? visuals[parent - s->entities].affine : identity, sizeof(v->affine));
            compose(v->affine, &v->local);
            v->visible = e->visible && (!parent || visuals[parent - s->entities].visible); v->resolved = true;
        }
    }
    free(chain); p->visuals = visuals; p->visual_scene = s->id; p->visual_revision = s->revision; return true;
}


Fl_ScenePreview *fl_scene_preview_create(Fl_Project *project)
{
    if (!project) return NULL;
    Fl_ScenePreview *p = calloc(1, sizeof(*p));
    if (p) p->project = project;
    return p;
}

bool fl_scene_preview_set_scene(Fl_ScenePreview *p, Fl_Id scene)
{
    if (!p) return false;
    p->scene = scene;
    return resolve_visuals(p, fl_project_scene(p->project, scene));
}

void fl_scene_preview_destroy(Fl_ScenePreview *p)
{
    if (!p) return;
    if (p->device && (p->pipeline || p->layout)) {
        vkDeviceWaitIdle(p->device);
        if (p->pipeline) vkDestroyPipeline(p->device, p->pipeline, NULL);
        if (p->layout) vkDestroyPipelineLayout(p->device, p->layout, NULL);
    }
    free(p->visuals); free(p);
}
