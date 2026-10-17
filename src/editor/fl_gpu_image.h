#pragma once
#include <ca_gpu.h>
#include <causality.h>
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

/* Create shared texture resources on instance; caller destroys before the GPU instance. */
Fl_GpuImages *fl_gpu_images_create(Ca_Instance *instance);
/* Wait for GPU use, then release shared resources. NULL is safe. */
void fl_gpu_images_destroy(Fl_GpuImages *images);
/* Return the combined-image-sampler layout and the white texel's descriptor used by untextured primitives. */
VkDescriptorSetLayout fl_gpu_images_layout(const Fl_GpuImages *images);
VkDescriptorSet fl_gpu_images_white(const Fl_GpuImages *images);
/* Upload width x height pixels of format synchronously. NULL on invalid input or failure. */
Fl_GpuImage *fl_gpu_image_create(Fl_GpuImages *images, Fl_GpuImageFormat format, const unsigned char *pixels,
                                 uint32_t width, uint32_t height);
/* Release an image after the caller ensured outstanding GPU reads completed. NULL is safe. */
void fl_gpu_image_destroy(Fl_GpuImage *image);
/* Return image's descriptor; NULL image returns VK_NULL_HANDLE. */
VkDescriptorSet fl_gpu_image_descriptor(const Fl_GpuImage *image);
