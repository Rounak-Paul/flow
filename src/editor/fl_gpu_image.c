#include "fl_gpu_image.h"
#include <stdlib.h>
#include <string.h>

struct Fl_GpuImages {
    Ca_Instance *instance;
    VkDevice device;
    VmaAllocator allocator;
    VkDescriptorSetLayout layout;
    VkSampler sampler;
    Fl_GpuImage *white;
};
struct Fl_GpuImage {
    Fl_GpuImages *owner;
    VkImage image;
    VmaAllocation allocation;
    VkImageView view;
    VkDescriptorPool pool;
    VkDescriptorSet descriptor;
};

Fl_GpuImage *fl_gpu_image_create(Fl_GpuImages *device, Fl_GpuImageFormat format, const unsigned char *pixels,
                                 uint32_t w, uint32_t h)
{
    if (!device || !pixels || !w || !h || (format != FL_GPU_IMAGE_COVERAGE && format != FL_GPU_IMAGE_SRGBA)) return NULL;
    const VkDeviceSize texel = format == FL_GPU_IMAGE_SRGBA ? 4 : 1, bytes = (VkDeviceSize)w * h * texel;
    const VkFormat vk_format = format == FL_GPU_IMAGE_SRGBA ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8_UNORM;
    Fl_GpuImage *image = calloc(1,sizeof(*image)); if (!image) return NULL;
    image->owner=device;
    VkBuffer staging=VK_NULL_HANDLE; VmaAllocation allocation=NULL; VmaAllocationInfo mapped={0};
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo host={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VkImageCreateInfo desc={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=vk_format,
        .extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo gpu={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    bool ok=vmaCreateBuffer(device->allocator,&buffer,&host,&staging,&allocation,&mapped)==VK_SUCCESS;
    if (ok) { memcpy(mapped.pMappedData,pixels,(size_t)bytes); ok=vmaFlushAllocation(device->allocator,allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS; }
    if (ok) ok=vmaCreateImage(device->allocator,&desc,&gpu,&image->image,&image->allocation,NULL)==VK_SUCCESS;
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image->image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=vk_format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if (ok) ok=vkCreateImageView(device->device,&view,NULL,&image->view)==VK_SUCCESS;
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};
    if (ok) ok=vkCreateDescriptorPool(device->device,&pool,NULL,&image->pool)==VK_SUCCESS;
    VkDescriptorSetAllocateInfo sets={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=image->pool,
        .descriptorSetCount=1,.pSetLayouts=&device->layout};
    if (ok) ok=vkAllocateDescriptorSets(device->device,&sets,&image->descriptor)==VK_SUCCESS;
    VkCommandBuffer cmd=ok ? ca_gpu_begin_transfer(device->instance) : VK_NULL_HANDLE;
    if (ok && !cmd) ok=false;
    if (ok) {
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image->image,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={w,h,1}};
        vkCmdCopyBufferToImage(cmd,staging,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&barrier);
        ca_gpu_end_transfer(device->instance,cmd);
        VkDescriptorImageInfo sampled={device->sampler,image->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=image->descriptor,.dstBinding=0,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&sampled};
        vkUpdateDescriptorSets(device->device,1,&write,0,NULL);
    }
    if (staging) vmaDestroyBuffer(device->allocator,staging,allocation);
    if (!ok) { fl_gpu_image_destroy(image); return NULL; }
    return image;
}

Fl_GpuImages *fl_gpu_images_create(Ca_Instance *instance)
{
    if (!instance) return NULL;
    Fl_GpuImages *device=calloc(1,sizeof(*device)); if (!device) return NULL;
    device->instance=instance; device->device=ca_gpu_device(instance); device->allocator=ca_gpu_allocator(instance);
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    bool ok=vkCreateDescriptorSetLayout(device->device,&layout,NULL,&device->layout)==VK_SUCCESS &&
        vkCreateSampler(device->device,&sampler,NULL,&device->sampler)==VK_SUCCESS;
    const unsigned char white=255;
    if (ok) device->white=fl_gpu_image_create(device,FL_GPU_IMAGE_COVERAGE,&white,1,1);
    if (!ok || !device->white) { fl_gpu_images_destroy(device); return NULL; }
    return device;
}
VkDescriptorSetLayout fl_gpu_images_layout(const Fl_GpuImages *device) { return device ? device->layout : VK_NULL_HANDLE; }
VkDescriptorSet fl_gpu_images_white(const Fl_GpuImages *device) { return device ? fl_gpu_image_descriptor(device->white) : VK_NULL_HANDLE; }
VkDescriptorSet fl_gpu_image_descriptor(const Fl_GpuImage *image) { return image ? image->descriptor : VK_NULL_HANDLE; }
void fl_gpu_image_destroy(Fl_GpuImage *image)
{
    if (!image) return;
    Fl_GpuImages *device=image->owner;
    if (image->pool) vkDestroyDescriptorPool(device->device,image->pool,NULL);
    if (image->view) vkDestroyImageView(device->device,image->view,NULL);
    if (image->image) vmaDestroyImage(device->allocator,image->image,image->allocation);
    free(image);
}
void fl_gpu_images_destroy(Fl_GpuImages *device)
{
    if (!device) return;
    vkDeviceWaitIdle(device->device);
    fl_gpu_image_destroy(device->white);
    if (device->sampler) vkDestroySampler(device->device,device->sampler,NULL);
    if (device->layout) vkDestroyDescriptorSetLayout(device->device,device->layout,NULL);
    free(device);
}
