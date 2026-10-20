#pragma once
#include <stdbool.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

/* Borrowed Vulkan host services; media rendering owns its resources independently of a UI/window library. */
typedef struct Fl_GpuContext {
    VkDevice device;
    VmaAllocator allocator;
    void *host;
    VkShaderModule (*compile_shader)(void *host, const char *source, VkShaderStageFlagBits stage);
    VkCommandBuffer (*begin_transfer)(void *host);
    void (*end_transfer)(void *host, VkCommandBuffer command);
} Fl_GpuContext;
