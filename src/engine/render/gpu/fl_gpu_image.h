#pragma once
#include "fl_gpu_context.h"
#include <stddef.h>
#include <stdint.h>

/* Shared sampled-texture resources for preview primitives: one descriptor layout, sampler and white texel. */
typedef struct Fl_GpuImages Fl_GpuImages;
/* One immutable sampled texture with its own descriptor set. */
typedef struct Fl_GpuImage Fl_GpuImage;
typedef enum Fl_GpuImageFormat {
    FL_GPU_IMAGE_COVERAGE,      /* 8-bit linear coverage mask */
    FL_GPU_IMAGE_SRGBA          /* straight-alpha sRGB RGBA8 picture */
} Fl_GpuImageFormat;

/* Create shared texture resources on the GPU host; caller destroys them before the host. */
Fl_GpuImages *fl_gpu_images_create(const Fl_GpuContext *context);
/* Wait for GPU use, then release shared resources. NULL is safe. */
void fl_gpu_images_destroy(Fl_GpuImages *images);
/* Return the combined-image-sampler layout and the white texel's descriptor used by untextured primitives. */
VkDescriptorSetLayout fl_gpu_images_layout(const Fl_GpuImages *images);
VkSampler fl_gpu_images_sampler(const Fl_GpuImages *images);
VkDescriptorSet fl_gpu_images_white(const Fl_GpuImages *images);
/* Upload width x height pixels of format synchronously. NULL on invalid input or failure. */
Fl_GpuImage *fl_gpu_image_create(Fl_GpuImages *images, Fl_GpuImageFormat format, const unsigned char *pixels,
                                 uint32_t width, uint32_t height);
/* Create an empty width x height colour target that can be rendered into and then sampled (premultiplied layers use
   VK_FORMAT_R16G16B16A16_SFLOAT). Its layout starts undefined; callers transition it. NULL on failure. */
Fl_GpuImage *fl_gpu_target_create(Fl_GpuImages *images, VkFormat format, uint32_t width, uint32_t height);
/* Vulkan handles of an image, for render passes and barriers. */
VkImage fl_gpu_image_handle(const Fl_GpuImage *image);
VkImageView fl_gpu_image_view(const Fl_GpuImage *image);
/* Release an image after the caller ensured outstanding GPU reads completed. NULL is safe. */
void fl_gpu_image_destroy(Fl_GpuImage *image);
/* Return image's descriptor; NULL image returns VK_NULL_HANDLE. */
VkDescriptorSet fl_gpu_image_descriptor(const Fl_GpuImage *image);
