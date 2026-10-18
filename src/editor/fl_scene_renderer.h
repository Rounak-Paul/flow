#pragma once
#include "core/fl_job.h"
#include "doc/fl_scene_geometry.h"
#include "doc/fl_scene_frame.h"
#include "render/fl_frame.h"
#include "text/fl_fonts.h"
#include <ca_gpu.h>
#include <causality.h>

/*
 * The scene renderer: the single implementation that draws scene content on the GPU. The editor viewport records
 * into the viewport's command buffer; fl_scene_renderer_render records into an offscreen image and reads it back,
 * so exports, agent previews and the viewport run the same shaders, blending and text/image handling.
 *
 * It owns the primitive pipelines, text coverage masks and decoded pictures. Owner (UI) thread only.
 */
typedef struct Fl_SceneRenderer Fl_SceneRenderer;

typedef struct Fl_SceneRendererDesc {
    Ca_Instance *instance;      /* required; supplies the device, queue and allocator */
    Fl_Fonts *fonts;            /* required */
    Fl_JobSystem *jobs;         /* required; images decode on workers */
    Fl_EventBus *bus;           /* optional: decode completion events (live viewport); NULL for synchronous use */
    void (*invalidate)(void *user_data);    /* called when a decode finishes and a redraw would change the picture */
    void *invalidate_data;
} Fl_SceneRendererDesc;

/* Create a renderer borrowing every described service until destroy. NULL on failure. */
Fl_SceneRenderer *fl_scene_renderer_create(const Fl_SceneRendererDesc *desc);
/* Wait for decoding and GPU work, then release every resource. NULL is safe. */
void fl_scene_renderer_destroy(Fl_SceneRenderer *renderer);

/*
 * Make text masks and pictures current for scene at state's time, before recording. Text is rasterized at
 * text_scale output pixels per scene unit. With wait, pending image decodes are finished first so the frame is
 * complete; without it missing pictures show a placeholder until invalidate fires.
 *
 * state  Evaluated state at the drawn time, or NULL for base values.
 */
void fl_scene_renderer_prepare(Fl_SceneRenderer *renderer, const Fl_Project *project, const Fl_Scene *scene,
                               const Fl_SceneState *state, double text_scale, bool wait);
/* Bind the primitive pipeline for a colour format and the whole target as viewport/scissor. False when unavailable. */
bool fl_scene_renderer_begin(Fl_SceneRenderer *renderer, VkCommandBuffer cmd, VkFormat format, uint32_t width, uint32_t height);
/* Fill target (scene units) with the scene backdrop; frame is the visible world rectangle the target maps into. */
void fl_scene_renderer_background(Fl_SceneRenderer *renderer, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_SceneFrame *target);
/* Draw every visible entity in draw order. geometry and state describe the same scene time. */
void fl_scene_renderer_entities(Fl_SceneRenderer *renderer, VkCommandBuffer cmd, const Fl_SceneFrame *frame, const Fl_Scene *scene,
                                const Fl_SceneState *state, const Fl_SceneGeometry *geometry);
/* Draw one affine primitive (editor aids use this); mask is the sampled coverage/picture descriptor. */
void fl_scene_renderer_primitive(Fl_SceneRenderer *renderer, VkCommandBuffer cmd, const Fl_SceneFrame *frame, Fl_Affine matrix,
                                 double width, double height, Fl_WidgetKind kind, Fl_Appearance appearance, VkDescriptorSet mask);
/* Issue a pre-built 20-float push block with mask, for full-target passes such as the editor grid. */
void fl_scene_renderer_push(Fl_SceneRenderer *renderer, VkCommandBuffer cmd, const float push[20], VkDescriptorSet mask);
/* Descriptor of the solid white texel used by untextured primitives. */
VkDescriptorSet fl_scene_renderer_white(const Fl_SceneRenderer *renderer);
/* Decode an sRGB channel to linear light as the shaders expect. */
float fl_scene_renderer_linear(unsigned char value);

/*
 * Render scene at tick into an RGBA8 image width pixels wide (height from the target frame's aspect), entirely
 * offscreen: independent of any window or what the editor shows. Blocks until the GPU finishes.
 */
bool fl_scene_renderer_render(Fl_SceneRenderer *renderer, const Fl_Project *project, const Fl_Scene *scene, Fl_Tick tick,
                              uint32_t width, Fl_Frame *out);
