#include "fl_render_host.h"
#include <ca_gpu.h>

/* Compile media shaders using the shared device's driver compiler. */
static VkShaderModule compile_shader(void *host, const char *source, VkShaderStageFlagBits stage)
{
    return ca_shader_compile(host,source,stage);
}
/* Acquire and submit serialized transfers on the editor's shared GPU queue. */
static VkCommandBuffer begin_transfer(void *host) {return ca_gpu_begin_transfer(host);}
static void end_transfer(void *host, VkCommandBuffer command) {ca_gpu_end_transfer(host,command);}

Fl_GpuContext fl_render_host_context(Ca_Instance *instance)
{
    if (!instance) return (Fl_GpuContext){0};
    return (Fl_GpuContext){.device=ca_gpu_device(instance),.allocator=ca_gpu_allocator(instance),.host=instance,
        .compile_shader=compile_shader,.begin_transfer=begin_transfer,.end_transfer=end_transfer};
}
