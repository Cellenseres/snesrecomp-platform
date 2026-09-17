/*
 * Native Vulkan presentation backend. SDL owns the window, the event loop
 * and the WSI surface; everything below the surface is Vulkan. The floor is
 * Vulkan 1.0 plus VK_KHR_swapchain.
 *
 * Every mutable GPU resource is owned per frame in flight, so a frame fence
 * alone proves its resources are free and no frame waits on a queue.
 */

#include "snesrecomp_platform/presenter_backend.h"
#include "snesrecomp_platform/snes_ppu_semantic_gpu.h"

#include "vulkan_policy.h"
#include "vulkan_shaders.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    /* Deep enough that a compositor holding a presented image cannot stall
     * the next submit. */
    VULKAN_FRAMES_IN_FLIGHT = 3,
    VULKAN_MODE7_INPUTS = 8,
    VULKAN_DESCRIPTOR_SETS_PER_FRAME = 24,
    VULKAN_SAMPLERS_PER_FRAME = 48,
};

typedef struct VulkanBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *mapped;
} VulkanBuffer;

typedef struct VulkanImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer framebuffer; /* only for offscreen targets */
    VkFormat format;
    uint32_t width;
    uint32_t height;
    VkImageLayout layout;
} VulkanImage;

typedef struct VulkanFrame {
    VkCommandBuffer command_buffer;
    VkSemaphore image_available;
    VkFence in_flight;
    VkDescriptorPool descriptor_pool;

    VulkanBuffer staging;
    VkDeviceSize staging_used;

    VulkanImage source;
    VulkanImage overlays[SNESRECOMP_OVERLAY_MAX_LAYERS];
    VulkanImage mode7_inputs[VULKAN_MODE7_INPUTS];
    VulkanImage mode7_target;
    VulkanImage scale_target;
} VulkanFrame;

typedef struct VulkanPresenterContext {
    SDL_Window *window;
    bool vulkan_library_loaded;

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkPhysicalDeviceProperties device_properties;
    VkDevice device;
    uint32_t graphics_family;
    uint32_t present_family;
    VkQueue graphics_queue;
    VkQueue present_queue;

    VkSwapchainKHR swapchain;
    VkSurfaceFormatKHR surface_format;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *swapchain_images;
    VkImageView *swapchain_views;
    VkFramebuffer *swapchain_framebuffers;
    /* Owned per swapchain image, not per frame in flight: vkQueuePresentKHR
     * waits on this and no fence covers when the presentation engine is done
     * with it, so reuse is only safe once its image is acquired again. */
    VkSemaphore *render_finished;
    bool swapchain_stale;

    VkRenderPass present_pass;
    VkRenderPass offscreen_pass;

    VkCommandPool command_pool;
    VkSampler nearest_sampler;
    VkSampler linear_sampler;

    VkDescriptorSetLayout sampler_layout;
    VkDescriptorSetLayout mode7_input_layout;
    VkPipelineLayout blit_pipeline_layout;
    VkPipelineLayout mode7_pipeline_layout;
    VkPipelineLayout sharp_bilinear_pipeline_layout;

    VkPipeline blit_pipeline;
    VkPipeline overlay_pipeline;
    VkPipeline mode7_pipeline;
    VkPipeline prescale_pipeline;
    VkPipeline sharp_bilinear_pipeline;

    VulkanBuffer vertex_buffer;

    VulkanFrame frames[VULKAN_FRAMES_IN_FLIGHT];
    uint32_t frame_index;

    bool preserve_aspect;
    bool linear_filtering;
    bool vsync_requested;
    SnesRecompPresentScaling scaling;

    /* Compared instead of the extent, so a surface whose currentExtent is
     * mandatory and differs from the window does not rebuild every frame. */
    int swapchain_drawable_width;
    int swapchain_drawable_height;

    uint8_t *mode7_obj_pixels;
    size_t mode7_obj_capacity;
    uint8_t *mode7_window_pixels;
    size_t mode7_window_capacity;

    unsigned swapchain_recreations;

} VulkanPresenterContext;

static bool set_sdl_error(
    SnesRecompPresenter *presenter,
    const char *operation) {
    const char *detail = SDL_GetError();
    snesrecomp_presenter_set_error(
        presenter,
        "%s failed: %s",
        operation,
        detail && detail[0] ? detail : "unknown SDL error");
    return false;
}

static bool set_vk_error(
    SnesRecompPresenter *presenter,
    const char *operation,
    VkResult result) {
    snesrecomp_presenter_set_error(
        presenter,
        "%s failed: %s",
        operation,
        snesrecomp_vulkan_result_name(result));
    return false;
}


static bool allocate_memory(
    SnesRecompPresenter *presenter,
    VkMemoryRequirements requirements,
    VkMemoryPropertyFlags properties,
    VkDeviceMemory *out_memory) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkMemoryAllocateInfo info;
    uint32_t type_index = 0;
    VkResult result;

    if (!snesrecomp_vulkan_find_memory_type(
            &context->memory_properties,
            requirements.memoryTypeBits,
            properties,
            &type_index)) {
        snesrecomp_presenter_set_error(
            presenter,
            "no Vulkan memory type satisfies 0x%x with mask 0x%x",
            (unsigned)properties,
            (unsigned)requirements.memoryTypeBits);
        return false;
    }

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = type_index;
    result = vkAllocateMemory(context->device, &info, NULL, out_memory);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkAllocateMemory", result);
    return true;
}

static void destroy_buffer(VkDevice device, VulkanBuffer *buffer) {
    if (buffer->mapped) {
        vkUnmapMemory(device, buffer->memory);
        buffer->mapped = NULL;
    }
    if (buffer->buffer) {
        vkDestroyBuffer(device, buffer->buffer, NULL);
        buffer->buffer = VK_NULL_HANDLE;
    }
    if (buffer->memory) {
        vkFreeMemory(device, buffer->memory, NULL);
        buffer->memory = VK_NULL_HANDLE;
    }
    buffer->size = 0;
}

static bool create_buffer(
    SnesRecompPresenter *presenter,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    bool keep_mapped,
    VulkanBuffer *out_buffer) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkBufferCreateInfo info;
    VkMemoryRequirements requirements;
    VkResult result;

    memset(out_buffer, 0, sizeof *out_buffer);
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(context->device, &info, NULL, &out_buffer->buffer);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateBuffer", result);

    vkGetBufferMemoryRequirements(
        context->device, out_buffer->buffer, &requirements);
    if (!allocate_memory(
            presenter, requirements, properties, &out_buffer->memory)) {
        destroy_buffer(context->device, out_buffer);
        return false;
    }
    result = vkBindBufferMemory(
        context->device, out_buffer->buffer, out_buffer->memory, 0);
    if (result != VK_SUCCESS) {
        destroy_buffer(context->device, out_buffer);
        return set_vk_error(presenter, "vkBindBufferMemory", result);
    }
    if (keep_mapped) {
        result = vkMapMemory(
            context->device, out_buffer->memory, 0, size, 0,
            &out_buffer->mapped);
        if (result != VK_SUCCESS) {
            destroy_buffer(context->device, out_buffer);
            return set_vk_error(presenter, "vkMapMemory", result);
        }
    }
    out_buffer->size = size;
    return true;
}


static void destroy_image(VkDevice device, VulkanImage *image) {
    if (image->framebuffer) {
        vkDestroyFramebuffer(device, image->framebuffer, NULL);
        image->framebuffer = VK_NULL_HANDLE;
    }
    if (image->view) {
        vkDestroyImageView(device, image->view, NULL);
        image->view = VK_NULL_HANDLE;
    }
    if (image->image) {
        vkDestroyImage(device, image->image, NULL);
        image->image = VK_NULL_HANDLE;
    }
    if (image->memory) {
        vkFreeMemory(device, image->memory, NULL);
        image->memory = VK_NULL_HANDLE;
    }
    image->width = 0;
    image->height = 0;
    image->format = VK_FORMAT_UNDEFINED;
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

static bool ensure_image(
    SnesRecompPresenter *presenter,
    VulkanImage *image,
    VkFormat format,
    uint32_t width,
    uint32_t height,
    VkImageUsageFlags usage,
    VkRenderPass render_pass) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkImageCreateInfo info;
    VkImageViewCreateInfo view_info;
    VkMemoryRequirements requirements;
    VkResult result;

    if (width == 0 || height == 0) {
        snesrecomp_presenter_set_error(
            presenter, "Vulkan image dimensions must be positive");
        return false;
    }
    if (image->image && image->format == format &&
        image->width == width && image->height == height) {
        return true;
    }
    destroy_image(context->device, image);

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(context->device, &info, NULL, &image->image);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateImage", result);

    vkGetImageMemoryRequirements(context->device, image->image, &requirements);
    if (!allocate_memory(
            presenter, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            &image->memory)) {
        destroy_image(context->device, image);
        return false;
    }
    result = vkBindImageMemory(
        context->device, image->image, image->memory, 0);
    if (result != VK_SUCCESS) {
        destroy_image(context->device, image);
        return set_vk_error(presenter, "vkBindImageMemory", result);
    }

    memset(&view_info, 0, sizeof view_info);
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(
        context->device, &view_info, NULL, &image->view);
    if (result != VK_SUCCESS) {
        destroy_image(context->device, image);
        return set_vk_error(presenter, "vkCreateImageView", result);
    }

    if (render_pass) {
        VkFramebufferCreateInfo fb;
        memset(&fb, 0, sizeof fb);
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &image->view;
        fb.width = width;
        fb.height = height;
        fb.layers = 1;
        result = vkCreateFramebuffer(
            context->device, &fb, NULL, &image->framebuffer);
        if (result != VK_SUCCESS) {
            destroy_image(context->device, image);
            return set_vk_error(presenter, "vkCreateFramebuffer", result);
        }
    }

    image->format = format;
    image->width = width;
    image->height = height;
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

static void image_barrier(
    VkCommandBuffer command_buffer,
    VulkanImage *image,
    VkImageLayout new_layout,
    VkAccessFlags src_access,
    VkAccessFlags dst_access,
    VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier;
    if (image->layout == new_layout)
        return;

    memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = image->layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        command_buffer, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
        &barrier);
    image->layout = new_layout;
}


static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

/* Always before recording starts: growing between two copies would leave
 * the first pointing at freed memory. */
static bool reserve_staging(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    VkDeviceSize bytes) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkDeviceSize target;

    if (bytes <= frame->staging.size)
        return true;

    target = align_up(bytes + bytes / 2u, 65536u);
    destroy_buffer(context->device, &frame->staging);
    frame->staging_used = 0;
    return create_buffer(
        presenter, target, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, &frame->staging);
}

/* bufferOffset must be a multiple of four and of the texel block size;
 * only the RGBA32F affine line texture needs more than four. */
static VkDeviceSize staging_alignment(uint32_t bytes_per_pixel) {
    return bytes_per_pixel > 4u ? (VkDeviceSize)bytes_per_pixel : 4u;
}

static uint8_t *stage_alloc(
    VulkanFrame *frame,
    VkDeviceSize bytes,
    VkDeviceSize alignment,
    VkDeviceSize *out_offset) {
    const VkDeviceSize offset = align_up(frame->staging_used, alignment);
    if (!frame->staging.mapped || offset + bytes > frame->staging.size)
        return NULL;
    frame->staging_used = offset + bytes;
    *out_offset = offset;
    return (uint8_t *)frame->staging.mapped + offset;
}

static bool upload_image(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    VulkanImage *image,
    const void *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t bytes_per_pixel,
    size_t pitch) {
    const VkDeviceSize row_bytes = (VkDeviceSize)width * bytes_per_pixel;
    const VkDeviceSize total = row_bytes * height;
    VkDeviceSize offset = 0;
    uint8_t *destination = stage_alloc(
        frame, total, staging_alignment(bytes_per_pixel), &offset);
    VkBufferImageCopy copy;
    uint32_t y;

    if (!destination) {
        snesrecomp_presenter_set_error(
            presenter, "Vulkan staging buffer is too small for an upload");
        return false;
    }
    if (pitch == row_bytes) {
        memcpy(destination, pixels, (size_t)total);
    } else {
        for (y = 0; y < height; y++) {
            memcpy(
                destination + (size_t)y * (size_t)row_bytes,
                (const uint8_t *)pixels + (size_t)y * pitch,
                (size_t)row_bytes);
        }
    }

    image_barrier(
        frame->command_buffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    memset(&copy, 0, sizeof copy);
    copy.bufferOffset = offset;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = width;
    copy.imageExtent.height = height;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(
        frame->command_buffer, frame->staging.buffer, image->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    image_barrier(
        frame->command_buffer, image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    return true;
}


static bool allocate_sampled_set(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    VkDescriptorSetLayout layout,
    VkDescriptorSet *out_set) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkDescriptorSetAllocateInfo info;
    VkResult result;

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = frame->descriptor_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    result = vkAllocateDescriptorSets(context->device, &info, out_set);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkAllocateDescriptorSets", result);
    return true;
}

static bool bind_sampled_image(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    const VulkanImage *image,
    bool linear,
    VkDescriptorSet *out_set) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkDescriptorImageInfo image_info;
    VkWriteDescriptorSet write;

    if (!allocate_sampled_set(
            presenter, frame, context->sampler_layout, out_set)) {
        return false;
    }

    memset(&image_info, 0, sizeof image_info);
    image_info.sampler =
        linear ? context->linear_sampler : context->nearest_sampler;
    image_info.imageView = image->view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    memset(&write, 0, sizeof write);
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = *out_set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(context->device, 1, &write, 0, NULL);
    return true;
}

static bool bind_mode7_inputs(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    VkDescriptorSet *out_set) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkDescriptorImageInfo image_info[VULKAN_MODE7_INPUTS];
    VkWriteDescriptorSet writes[VULKAN_MODE7_INPUTS];
    unsigned i;

    if (!allocate_sampled_set(
            presenter, frame, context->mode7_input_layout, out_set)) {
        return false;
    }

    memset(image_info, 0, sizeof image_info);
    memset(writes, 0, sizeof writes);
    for (i = 0; i < VULKAN_MODE7_INPUTS; i++) {
        /* Every Mode 7 input is fetched with texelFetch, so the sampler's
         * filter never applies; nearest keeps that explicit. */
        image_info[i].sampler = context->nearest_sampler;
        image_info[i].imageView = frame->mode7_inputs[i].view;
        image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = *out_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_info[i];
    }
    vkUpdateDescriptorSets(
        context->device, VULKAN_MODE7_INPUTS, writes, 0, NULL);
    return true;
}


static void destroy_swapchain(VulkanPresenterContext *context) {
    uint32_t i;
    if (context->render_finished) {
        for (i = 0; i < context->image_count; i++) {
            if (context->render_finished[i]) {
                vkDestroySemaphore(
                    context->device, context->render_finished[i], NULL);
            }
        }
        free(context->render_finished);
        context->render_finished = NULL;
    }
    if (context->swapchain_framebuffers) {
        for (i = 0; i < context->image_count; i++) {
            if (context->swapchain_framebuffers[i]) {
                vkDestroyFramebuffer(
                    context->device, context->swapchain_framebuffers[i],
                    NULL);
            }
        }
        free(context->swapchain_framebuffers);
        context->swapchain_framebuffers = NULL;
    }
    if (context->swapchain_views) {
        for (i = 0; i < context->image_count; i++) {
            if (context->swapchain_views[i]) {
                vkDestroyImageView(
                    context->device, context->swapchain_views[i], NULL);
            }
        }
        free(context->swapchain_views);
        context->swapchain_views = NULL;
    }
    free(context->swapchain_images);
    context->swapchain_images = NULL;
    context->image_count = 0;
    if (context->swapchain) {
        vkDestroySwapchainKHR(context->device, context->swapchain, NULL);
        context->swapchain = VK_NULL_HANDLE;
    }
}

/* A zero-sized drawable is a minimised window, not a failure: it leaves
 * *out_skipped true. The device idle wait here and the one in destroy are
 * the only two in the backend. */
static bool create_swapchain(
    SnesRecompPresenter *presenter,
    bool *out_skipped) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR *formats = NULL;
    VkPresentModeKHR *modes = NULL;
    uint32_t format_count = 0, mode_count = 0;
    VkSwapchainCreateInfoKHR info;
    uint32_t queue_families[2];
    int drawable_width = 0, drawable_height = 0;
    VkResult result;
    uint32_t i;
    bool ok = false;

    *out_skipped = false;
    if (!SDL_GetWindowSizeInPixels(
            context->window, &drawable_width, &drawable_height)) {
        return set_sdl_error(presenter, "SDL_GetWindowSizeInPixels");
    }
    if (drawable_width <= 0 || drawable_height <= 0) {
        *out_skipped = true;
        return true;
    }

    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        context->physical_device, context->surface, &capabilities);
    if (result != VK_SUCCESS) {
        return set_vk_error(
            presenter, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
    }

    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        context->physical_device, context->surface, &format_count, NULL);
    if (result != VK_SUCCESS || format_count == 0) {
        return set_vk_error(
            presenter, "vkGetPhysicalDeviceSurfaceFormatsKHR", result);
    }
    formats = (VkSurfaceFormatKHR *)calloc(format_count, sizeof *formats);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        context->physical_device, context->surface, &mode_count, NULL);
    if (!formats || result != VK_SUCCESS || mode_count == 0) {
        free(formats);
        snesrecomp_presenter_set_error(
            presenter, "could not enumerate Vulkan surface capabilities");
        return false;
    }
    modes = (VkPresentModeKHR *)calloc(mode_count, sizeof *modes);
    if (!modes) {
        free(formats);
        snesrecomp_presenter_set_error(
            presenter, "out of memory enumerating Vulkan present modes");
        return false;
    }
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(
            context->physical_device, context->surface, &format_count,
            formats) != VK_SUCCESS ||
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            context->physical_device, context->surface, &mode_count,
            modes) != VK_SUCCESS) {
        snesrecomp_presenter_set_error(
            presenter, "could not read Vulkan surface formats/present modes");
        goto done;
    }

    if (context->surface_format.format == VK_FORMAT_UNDEFINED &&
        !snesrecomp_vulkan_choose_surface_format(
            formats, format_count, &context->surface_format)) {
        snesrecomp_presenter_set_error(
            presenter, "no usable Vulkan surface format");
        goto done;
    }
    if (!snesrecomp_vulkan_choose_present_mode(
            modes, mode_count, context->vsync_requested,
            &context->present_mode)) {
        snesrecomp_presenter_set_error(
            presenter, "no usable Vulkan present mode");
        goto done;
    }
    snesrecomp_vulkan_choose_extent(
        &capabilities, (uint32_t)drawable_width, (uint32_t)drawable_height,
        &context->extent);
    if (context->extent.width == 0 || context->extent.height == 0) {
        *out_skipped = true;
        ok = true;
        goto done;
    }

    vkDeviceWaitIdle(context->device);
    {
        VkSwapchainKHR previous = context->swapchain;
        context->swapchain = VK_NULL_HANDLE;
        destroy_swapchain(context);
        context->swapchain = previous;
    }

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = context->surface;
    info.minImageCount = snesrecomp_vulkan_choose_image_count(&capabilities);
    info.imageFormat = context->surface_format.format;
    info.imageColorSpace = context->surface_format.colorSpace;
    info.imageExtent = context->extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    queue_families[0] = context->graphics_family;
    queue_families[1] = context->present_family;
    if (context->graphics_family != context->present_family) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = queue_families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = capabilities.currentTransform;
    if (capabilities.supportedCompositeAlpha &
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha &
               VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha &
               VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
        info.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    } else {
        info.compositeAlpha =
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }
    info.presentMode = context->present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = context->swapchain;

    {
        VkSwapchainKHR created = VK_NULL_HANDLE;
        result = vkCreateSwapchainKHR(
            context->device, &info, NULL, &created);
        if (context->swapchain) {
            vkDestroySwapchainKHR(context->device, context->swapchain, NULL);
            context->swapchain = VK_NULL_HANDLE;
        }
        if (result != VK_SUCCESS) {
            set_vk_error(presenter, "vkCreateSwapchainKHR", result);
            goto done;
        }
        context->swapchain = created;
    }

    result = vkGetSwapchainImagesKHR(
        context->device, context->swapchain, &context->image_count, NULL);
    if (result != VK_SUCCESS || context->image_count == 0) {
        set_vk_error(presenter, "vkGetSwapchainImagesKHR", result);
        goto done;
    }
    context->swapchain_images =
        (VkImage *)calloc(context->image_count, sizeof(VkImage));
    context->swapchain_views =
        (VkImageView *)calloc(context->image_count, sizeof(VkImageView));
    context->swapchain_framebuffers =
        (VkFramebuffer *)calloc(context->image_count, sizeof(VkFramebuffer));
    context->render_finished =
        (VkSemaphore *)calloc(context->image_count, sizeof(VkSemaphore));
    if (!context->swapchain_images || !context->swapchain_views ||
        !context->swapchain_framebuffers || !context->render_finished) {
        snesrecomp_presenter_set_error(
            presenter, "out of memory creating Vulkan swapchain views");
        goto done;
    }
    result = vkGetSwapchainImagesKHR(
        context->device, context->swapchain, &context->image_count,
        context->swapchain_images);
    if (result != VK_SUCCESS) {
        set_vk_error(presenter, "vkGetSwapchainImagesKHR", result);
        goto done;
    }

    for (i = 0; i < context->image_count; i++) {
        VkImageViewCreateInfo view_info;
        VkFramebufferCreateInfo fb;
        VkSemaphoreCreateInfo semaphore_info;

        memset(&semaphore_info, 0, sizeof semaphore_info);
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        result = vkCreateSemaphore(
            context->device, &semaphore_info, NULL,
            &context->render_finished[i]);
        if (result != VK_SUCCESS) {
            set_vk_error(presenter, "vkCreateSemaphore(present)", result);
            goto done;
        }

        memset(&view_info, 0, sizeof view_info);
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = context->swapchain_images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = context->surface_format.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(
            context->device, &view_info, NULL, &context->swapchain_views[i]);
        if (result != VK_SUCCESS) {
            set_vk_error(presenter, "vkCreateImageView(swapchain)", result);
            goto done;
        }

        memset(&fb, 0, sizeof fb);
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = context->present_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &context->swapchain_views[i];
        fb.width = context->extent.width;
        fb.height = context->extent.height;
        fb.layers = 1;
        result = vkCreateFramebuffer(
            context->device, &fb, NULL,
            &context->swapchain_framebuffers[i]);
        if (result != VK_SUCCESS) {
            set_vk_error(presenter, "vkCreateFramebuffer(swapchain)", result);
            goto done;
        }
    }

    presenter->vsync_state =
        snesrecomp_vulkan_present_mode_vsync_state(context->present_mode);
    context->swapchain_drawable_width = drawable_width;
    context->swapchain_drawable_height = drawable_height;
    context->swapchain_stale = false;
    context->swapchain_recreations++;
    if (context->swapchain_recreations == 120u) {
        fprintf(stderr,
            "[vulkan] swapchain rebuilt %u times; the surface keeps "
            "reporting out-of-date or suboptimal\n",
            context->swapchain_recreations);
    }
    ok = true;

done:
    free(formats);
    free(modes);
    return ok;
}

/* The clear is what makes the letterbox deterministic without a separate
 * draw. */
static bool create_render_passes(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkAttachmentDescription attachment;
    VkAttachmentReference reference;
    VkSubpassDescription subpass;
    VkSubpassDependency dependencies[2];
    VkRenderPassCreateInfo info;
    VkResult result;

    memset(&reference, 0, sizeof reference);
    reference.attachment = 0;
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&subpass, 0, sizeof subpass);
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;

    memset(&attachment, 0, sizeof attachment);
    attachment.format = context->surface_format.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    memset(dependencies, 0, sizeof dependencies);
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = dependencies;
    result = vkCreateRenderPass(
        context->device, &info, NULL, &context->present_pass);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateRenderPass(present)", result);

    /* Ends in SHADER_READ_ONLY_OPTIMAL and declares the dependency, so the
     * caller needs no barrier between rendering and sampling. */
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    info.dependencyCount = 2;
    result = vkCreateRenderPass(
        context->device, &info, NULL, &context->offscreen_pass);
    if (result != VK_SUCCESS) {
        return set_vk_error(
            presenter, "vkCreateRenderPass(offscreen)", result);
    }
    return true;
}


static bool create_shader_module(
    SnesRecompPresenter *presenter,
    const uint32_t *code,
    size_t bytes,
    VkShaderModule *out_module) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkShaderModuleCreateInfo info;
    VkResult result;

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    result = vkCreateShaderModule(context->device, &info, NULL, out_module);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateShaderModule", result);
    return true;
}

static bool create_pipeline(
    SnesRecompPresenter *presenter,
    const uint32_t *vertex_code,
    size_t vertex_bytes,
    const uint32_t *fragment_code,
    size_t fragment_bytes,
    VkPipelineLayout layout,
    VkRenderPass render_pass,
    bool premultiplied_blend,
    VkPipeline *out_pipeline) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attributes[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo rasterization;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo blend;
    VkDynamicState dynamic_states[2];
    VkPipelineDynamicStateCreateInfo dynamic;
    VkGraphicsPipelineCreateInfo info;
    VkResult result;
    bool ok = false;

    if (!create_shader_module(
            presenter, vertex_code, vertex_bytes, &vertex_module)) {
        return false;
    }
    if (!create_shader_module(
            presenter, fragment_code, fragment_bytes, &fragment_module)) {
        vkDestroyShaderModule(context->device, vertex_module, NULL);
        return false;
    }

    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    memset(&binding, 0, sizeof binding);
    binding.binding = 0;
    binding.stride = 4u * (uint32_t)sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    memset(attributes, 0, sizeof attributes);
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = 2u * (uint32_t)sizeof(float);

    memset(&vertex_input, 0, sizeof vertex_input);
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    memset(&input_assembly, 0, sizeof input_assembly);
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    memset(&viewport_state, 0, sizeof viewport_state);
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    memset(&rasterization, 0, sizeof rasterization);
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    memset(&multisample, 0, sizeof multisample);
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&blend_attachment, 0, sizeof blend_attachment);
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (premultiplied_blend) {
        /* The overlay contract: colour is already multiplied by alpha, so the
         * source factor is ONE and only the destination is attenuated. */
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstColorBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    memset(&blend, 0, sizeof blend);
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
    memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = render_pass;
    info.subpass = 0;

    result = vkCreateGraphicsPipelines(
        context->device, VK_NULL_HANDLE, 1, &info, NULL, out_pipeline);
    if (result != VK_SUCCESS)
        set_vk_error(presenter, "vkCreateGraphicsPipelines", result);
    else
        ok = true;

    vkDestroyShaderModule(context->device, fragment_module, NULL);
    vkDestroyShaderModule(context->device, vertex_module, NULL);
    return ok;
}

typedef struct Mode7PushConstants {
    float map_fixed_wrap[2];
    float hd_scale;
    float canvas_extra;
    float native_height;
    float bg_filter;
    float line_lerp;
    float reserved;
} Mode7PushConstants;

typedef struct SharpBilinearPushConstants {
    float input_size[2];
    float texture_size[2];
    float output_size[2];
} SharpBilinearPushConstants;

static bool create_samplers(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkSamplerCreateInfo info;
    VkResult result;

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    info.maxLod = VK_LOD_CLAMP_NONE;
    result = vkCreateSampler(
        context->device, &info, NULL, &context->nearest_sampler);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateSampler(nearest)", result);

    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    result = vkCreateSampler(
        context->device, &info, NULL, &context->linear_sampler);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateSampler(linear)", result);
    return true;
}

static bool create_layouts(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkDescriptorSetLayoutBinding bindings[VULKAN_MODE7_INPUTS];
    VkDescriptorSetLayoutCreateInfo layout_info;
    VkPipelineLayoutCreateInfo pipeline_info;
    VkPushConstantRange range;
    VkResult result;
    unsigned i;

    memset(bindings, 0, sizeof bindings);
    for (i = 0; i < VULKAN_MODE7_INPUTS; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    memset(&layout_info, 0, sizeof layout_info);
    layout_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(
        context->device, &layout_info, NULL, &context->sampler_layout);
    if (result != VK_SUCCESS) {
        return set_vk_error(
            presenter, "vkCreateDescriptorSetLayout(sampler)", result);
    }

    layout_info.bindingCount = VULKAN_MODE7_INPUTS;
    result = vkCreateDescriptorSetLayout(
        context->device, &layout_info, NULL, &context->mode7_input_layout);
    if (result != VK_SUCCESS) {
        return set_vk_error(
            presenter, "vkCreateDescriptorSetLayout(mode7)", result);
    }

    memset(&pipeline_info, 0, sizeof pipeline_info);
    pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_info.setLayoutCount = 1;
    pipeline_info.pSetLayouts = &context->sampler_layout;
    result = vkCreatePipelineLayout(
        context->device, &pipeline_info, NULL,
        &context->blit_pipeline_layout);
    if (result != VK_SUCCESS) {
        return set_vk_error(
            presenter, "vkCreatePipelineLayout(blit)", result);
    }

    memset(&range, 0, sizeof range);
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = (uint32_t)sizeof(SharpBilinearPushConstants);
    pipeline_info.pushConstantRangeCount = 1;
    pipeline_info.pPushConstantRanges = &range;
    result = vkCreatePipelineLayout(
        context->device, &pipeline_info, NULL,
        &context->sharp_bilinear_pipeline_layout);
    if (result != VK_SUCCESS) {
        return set_vk_error(
            presenter, "vkCreatePipelineLayout(clean-hd)", result);
    }

    range.size = (uint32_t)sizeof(Mode7PushConstants);
    pipeline_info.pSetLayouts = &context->mode7_input_layout;
    result = vkCreatePipelineLayout(
        context->device, &pipeline_info, NULL,
        &context->mode7_pipeline_layout);
    if (result != VK_SUCCESS) {
        return set_vk_error(
            presenter, "vkCreatePipelineLayout(mode7)", result);
    }
    return true;
}

static bool create_pipelines(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;

    return create_pipeline(
               presenter,
               snesrecomp_vulkan_blit_vert_spv,
               SNESRECOMP_VULKAN_blit_vert_SPV_BYTES,
               snesrecomp_vulkan_blit_frag_spv,
               SNESRECOMP_VULKAN_blit_frag_SPV_BYTES,
               context->blit_pipeline_layout, context->present_pass, false,
               &context->blit_pipeline) &&
           create_pipeline(
               presenter,
               snesrecomp_vulkan_overlay_vert_spv,
               SNESRECOMP_VULKAN_overlay_vert_SPV_BYTES,
               snesrecomp_vulkan_overlay_frag_spv,
               SNESRECOMP_VULKAN_overlay_frag_SPV_BYTES,
               context->blit_pipeline_layout, context->present_pass, true,
               &context->overlay_pipeline) &&
           create_pipeline(
               presenter,
               snesrecomp_vulkan_mode7_vert_spv,
               SNESRECOMP_VULKAN_mode7_vert_SPV_BYTES,
               snesrecomp_vulkan_mode7_frag_spv,
               SNESRECOMP_VULKAN_mode7_frag_SPV_BYTES,
               context->mode7_pipeline_layout, context->offscreen_pass,
               false, &context->mode7_pipeline) &&
           create_pipeline(
               presenter,
               snesrecomp_vulkan_blit_vert_spv,
               SNESRECOMP_VULKAN_blit_vert_SPV_BYTES,
               snesrecomp_vulkan_sharp_bilinear_prescale_frag_spv,
               SNESRECOMP_VULKAN_sharp_bilinear_prescale_frag_SPV_BYTES,
               context->blit_pipeline_layout, context->offscreen_pass, false,
               &context->prescale_pipeline) &&
           create_pipeline(
               presenter,
               snesrecomp_vulkan_blit_vert_spv,
               SNESRECOMP_VULKAN_blit_vert_SPV_BYTES,
               snesrecomp_vulkan_sharp_bilinear_resolve_frag_spv,
               SNESRECOMP_VULKAN_sharp_bilinear_resolve_frag_SPV_BYTES,
               context->sharp_bilinear_pipeline_layout, context->present_pass,
               false, &context->sharp_bilinear_pipeline);
}

static bool create_vertex_buffer(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    /* Vulkan's +Y is down, so uv(0,0) pairs with NDC(-1,-1) and row zero of a
     * texture lands at the top, as in the OpenGL backend. */
    static const float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };

    if (!create_buffer(
            presenter, sizeof vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, &context->vertex_buffer)) {
        return false;
    }
    memcpy(context->vertex_buffer.mapped, vertices, sizeof vertices);
    return true;
}

static void destroy_frame(VkDevice device, VulkanFrame *frame) {
    unsigned i;
    if (frame->in_flight)
        vkDestroyFence(device, frame->in_flight, NULL);
    if (frame->image_available)
        vkDestroySemaphore(device, frame->image_available, NULL);
    if (frame->descriptor_pool)
        vkDestroyDescriptorPool(device, frame->descriptor_pool, NULL);
    destroy_buffer(device, &frame->staging);
    destroy_image(device, &frame->source);
    for (i = 0; i < SNESRECOMP_OVERLAY_MAX_LAYERS; i++)
        destroy_image(device, &frame->overlays[i]);
    for (i = 0; i < VULKAN_MODE7_INPUTS; i++)
        destroy_image(device, &frame->mode7_inputs[i]);
    destroy_image(device, &frame->mode7_target);
    destroy_image(device, &frame->scale_target);
    memset(frame, 0, sizeof *frame);
}

static bool create_frames(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkCommandBufferAllocateInfo command_info;
    VkSemaphoreCreateInfo semaphore_info;
    VkFenceCreateInfo fence_info;
    VkDescriptorPoolSize pool_size;
    VkDescriptorPoolCreateInfo pool_info;
    VkResult result;
    unsigned i;

    memset(&semaphore_info, 0, sizeof semaphore_info);
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    memset(&fence_info, 0, sizeof fence_info);
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    /* Signalled, so the first frame's wait returns immediately. */
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    memset(&pool_size, 0, sizeof pool_size);
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = VULKAN_SAMPLERS_PER_FRAME;
    memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = VULKAN_DESCRIPTOR_SETS_PER_FRAME;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    for (i = 0; i < VULKAN_FRAMES_IN_FLIGHT; i++) {
        VulkanFrame *frame = &context->frames[i];

        memset(&command_info, 0, sizeof command_info);
        command_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = context->command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(
            context->device, &command_info, &frame->command_buffer);
        if (result != VK_SUCCESS) {
            return set_vk_error(
                presenter, "vkAllocateCommandBuffers", result);
        }
        result = vkCreateSemaphore(
            context->device, &semaphore_info, NULL,
            &frame->image_available);
        if (result != VK_SUCCESS)
            return set_vk_error(presenter, "vkCreateSemaphore", result);
        result = vkCreateFence(
            context->device, &fence_info, NULL, &frame->in_flight);
        if (result != VK_SUCCESS)
            return set_vk_error(presenter, "vkCreateFence", result);
        result = vkCreateDescriptorPool(
            context->device, &pool_info, NULL, &frame->descriptor_pool);
        if (result != VK_SUCCESS)
            return set_vk_error(presenter, "vkCreateDescriptorPool", result);
    }
    return true;
}


/* Nothing here resets the fence. That happens only once the command buffer
 * is certain to be submitted, so a failure in between cannot leave a fence
 * that never signals. */
static bool frame_begin(
    SnesRecompPresenter *presenter,
    VulkanFrame **out_frame,
    bool *out_skipped) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VulkanFrame *frame = &context->frames[context->frame_index];
    int drawable_width = 0, drawable_height = 0;
    VkResult result;

    *out_frame = NULL;
    *out_skipped = false;

    if (!SDL_GetWindowSizeInPixels(
            context->window, &drawable_width, &drawable_height)) {
        return set_sdl_error(presenter, "SDL_GetWindowSizeInPixels");
    }
    if (drawable_width <= 0 || drawable_height <= 0) {
        *out_skipped = true;
        return true;
    }
    if (context->swapchain_stale || !context->swapchain ||
        context->swapchain_drawable_width != drawable_width ||
        context->swapchain_drawable_height != drawable_height) {
        bool skipped = false;
        if (!create_swapchain(presenter, &skipped))
            return false;
        if (skipped || !context->swapchain) {
            *out_skipped = true;
            return true;
        }
    }

    result = vkWaitForFences(
        context->device, 1, &frame->in_flight, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkWaitForFences", result);

    result = vkResetDescriptorPool(
        context->device, frame->descriptor_pool, 0);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkResetDescriptorPool", result);
    frame->staging_used = 0;

    *out_frame = frame;
    return true;
}

/* An out-of-date swapchain is not an error: it is rebuilt and this frame
 * is dropped, which is what a resize needs. */
static bool frame_acquire(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    uint32_t *out_image_index,
    bool *out_skipped) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkCommandBufferBeginInfo begin_info;
    VkResult result;

    *out_skipped = false;
    result = vkAcquireNextImageKHR(
        context->device, context->swapchain, UINT64_MAX,
        frame->image_available, VK_NULL_HANDLE, out_image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        context->swapchain_stale = true;
        *out_skipped = true;
        return true;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return set_vk_error(presenter, "vkAcquireNextImageKHR", result);
    if (result == VK_SUBOPTIMAL_KHR)
        context->swapchain_stale = true;

    result = vkResetFences(context->device, 1, &frame->in_flight);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkResetFences", result);

    result = vkResetCommandBuffer(frame->command_buffer, 0);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkResetCommandBuffer", result);

    memset(&begin_info, 0, sizeof begin_info);
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(frame->command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkBeginCommandBuffer", result);
    return true;
}

static bool frame_submit(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    uint32_t image_index) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    const VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore render_finished = context->render_finished[image_index];
    VkSubmitInfo submit;
    VkPresentInfoKHR present;
    VkResult result;

    result = vkEndCommandBuffer(frame->command_buffer);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkEndCommandBuffer", result);

    memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame->image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame->command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished;
    result = vkQueueSubmit(
        context->graphics_queue, 1, &submit, frame->in_flight);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkQueueSubmit", result);

    memset(&present, 0, sizeof present);
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &context->swapchain;
    present.pImageIndices = &image_index;
    result = vkQueuePresentKHR(context->present_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        context->swapchain_stale = true;
    } else if (result != VK_SUCCESS) {
        return set_vk_error(presenter, "vkQueuePresentKHR", result);
    }


    context->frame_index =
        (context->frame_index + 1u) % VULKAN_FRAMES_IN_FLIGHT;
    return true;
}


static bool validate_overlay(
    SnesRecompPresenter *presenter,
    const SnesRecompOverlayFrame *overlay,
    int logical_width,
    int logical_height) {
    size_t i;
    if (!overlay)
        return true;
    /* The OpenGL backend's acceptance, so a frame either refuses is refused
     * by both. */
    if (!overlay->layers || overlay->layer_count == 0 ||
        overlay->layer_count > SNESRECOMP_OVERLAY_MAX_LAYERS ||
        overlay->canvas_width <= 0 || overlay->canvas_height <= 0 ||
        (overlay->space != SNESRECOMP_OVERLAY_SPACE_FRAME &&
         overlay->space != SNESRECOMP_OVERLAY_SPACE_PRESENTATION) ||
        (overlay->space == SNESRECOMP_OVERLAY_SPACE_FRAME &&
         (overlay->canvas_width != logical_width ||
          overlay->canvas_height != logical_height))) {
        snesrecomp_presenter_set_error(presenter, "invalid overlay frame");
        return false;
    }
    for (i = 0; i < overlay->layer_count; i++) {
        const SnesRecompOverlayLayer *layer = &overlay->layers[i];
        if (!layer->pixels ||
            layer->pixel_format != SNESRECOMP_PIXEL_FORMAT_ARGB8888 ||
            layer->width <= 0 || layer->height <= 0 ||
            layer->pitch < layer->width * 4 || (layer->pitch & 3) != 0 ||
            layer->display_width <= 0 || layer->display_height <= 0) {
            snesrecomp_presenter_set_error(
                presenter, "invalid overlay layer");
            return false;
        }
    }
    return true;
}

static VkDeviceSize overlay_staging_bytes(
    const SnesRecompOverlayFrame *overlay) {
    VkDeviceSize bytes = 0;
    size_t i;
    if (!overlay)
        return 0;
    for (i = 0; i < overlay->layer_count; i++) {
        bytes = align_up(bytes, staging_alignment(4u)) +
            (VkDeviceSize)overlay->layers[i].width *
            overlay->layers[i].height * 4u;
    }
    return bytes;
}

static bool prepare_overlay_images(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    const SnesRecompOverlayFrame *overlay) {
    size_t i;
    if (!overlay)
        return true;
    for (i = 0; i < overlay->layer_count; i++) {
        if (!ensure_image(
                presenter, &frame->overlays[i],
                snesrecomp_vulkan_frame_format(),
                (uint32_t)overlay->layers[i].width,
                (uint32_t)overlay->layers[i].height,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_NULL_HANDLE)) {
            return false;
        }
    }
    return true;
}

static bool upload_overlays(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    const SnesRecompOverlayFrame *overlay) {
    size_t i;
    if (!overlay)
        return true;
    for (i = 0; i < overlay->layer_count; i++) {
        const SnesRecompOverlayLayer *layer = &overlay->layers[i];
        if (!upload_image(
                presenter, frame, &frame->overlays[i], layer->pixels,
                (uint32_t)layer->width, (uint32_t)layer->height, 4u,
                (size_t)layer->pitch)) {
            return false;
        }
    }
    return true;
}

static void set_viewport_and_scissor(
    VkCommandBuffer command_buffer,
    const SnesRecompVulkanRect *rect,
    VkExtent2D bounds) {
    VkViewport viewport;
    VkRect2D scissor;
    int x = rect->x, y = rect->y, width = rect->width, height = rect->height;

    /* Clipped for the scissor but not for the viewport, so a layer outside a
     * shrinking window is cropped rather than squashed. */
    memset(&viewport, 0, sizeof viewport);
    viewport.x = (float)x;
    viewport.y = (float)y;
    viewport.width = (float)(width > 0 ? width : 1);
    viewport.height = (float)(height > 0 ? height : 1);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);

    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x > (int)bounds.width) x = (int)bounds.width;
    if (y > (int)bounds.height) y = (int)bounds.height;
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    if (x + width > (int)bounds.width) width = (int)bounds.width - x;
    if (y + height > (int)bounds.height) height = (int)bounds.height - y;

    memset(&scissor, 0, sizeof scissor);
    scissor.offset.x = x;
    scissor.offset.y = y;
    scissor.extent.width = (uint32_t)width;
    scissor.extent.height = (uint32_t)height;
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}


static bool sharp_bilinear_active(const VulkanPresenterContext *context) {
    return context->scaling == SNESRECOMP_PRESENT_SCALING_SHARP_BILINEAR;
}

/* Point-sampled into a target twice the size, establishing the integer
 * texel grid the final pass blends across. Selected by the scaling policy,
 * never by recognising a preset filename. */
static bool record_prescale(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    const VulkanImage *source) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkRenderPassBeginInfo pass;
    VkClearValue clear;
    VkDescriptorSet set = VK_NULL_HANDLE;
    SnesRecompVulkanRect rect;
    VkExtent2D bounds;
    VkDeviceSize offset = 0;

    if (!bind_sampled_image(presenter, frame, source, false, &set))
        return false;

    memset(&clear, 0, sizeof clear);
    clear.color.float32[3] = 1.0f;
    memset(&pass, 0, sizeof pass);
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = context->offscreen_pass;
    pass.framebuffer = frame->scale_target.framebuffer;
    pass.renderArea.extent.width = frame->scale_target.width;
    pass.renderArea.extent.height = frame->scale_target.height;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(
        frame->command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);

    rect.x = 0;
    rect.y = 0;
    rect.width = (int)frame->scale_target.width;
    rect.height = (int)frame->scale_target.height;
    bounds.width = frame->scale_target.width;
    bounds.height = frame->scale_target.height;
    set_viewport_and_scissor(frame->command_buffer, &rect, bounds);

    vkCmdBindPipeline(
        frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        context->prescale_pipeline);
    vkCmdBindDescriptorSets(
        frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        context->blit_pipeline_layout, 0, 1, &set, 0, NULL);
    vkCmdBindVertexBuffers(
        frame->command_buffer, 0, 1, &context->vertex_buffer.buffer,
        &offset);
    vkCmdDraw(frame->command_buffer, 4, 1, 0, 0);
    vkCmdEndRenderPass(frame->command_buffer);

    frame->scale_target.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

static bool record_presentation(
    SnesRecompPresenter *presenter,
    VulkanFrame *frame,
    uint32_t image_index,
    const VulkanImage *source,
    int logical_width,
    int logical_height,
    const SnesRecompOverlayFrame *overlay) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    const VulkanImage *resolved = source;
    VkRenderPassBeginInfo pass;
    VkClearValue clear;
    VkDescriptorSet set = VK_NULL_HANDLE;
    SnesRecompVulkanRect rect;
    VkDeviceSize offset = 0;
    size_t i;

    if (sharp_bilinear_active(context)) {
        if (!record_prescale(presenter, frame, source))
            return false;
        resolved = &frame->scale_target;
    }
    if (!bind_sampled_image(
            presenter, frame, resolved,
            sharp_bilinear_active(context) || context->linear_filtering, &set)) {
        return false;
    }

    snesrecomp_vulkan_fit_viewport(
        (int)context->extent.width, (int)context->extent.height,
        snesrecomp_presenter_display_width(presenter, logical_width),
        logical_height, context->preserve_aspect, &rect);

    memset(&clear, 0, sizeof clear);
    clear.color.float32[3] = 1.0f;
    memset(&pass, 0, sizeof pass);
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = context->present_pass;
    pass.framebuffer = context->swapchain_framebuffers[image_index];
    pass.renderArea.extent = context->extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(
        frame->command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);

    set_viewport_and_scissor(frame->command_buffer, &rect, context->extent);
    vkCmdBindVertexBuffers(
        frame->command_buffer, 0, 1, &context->vertex_buffer.buffer,
        &offset);

    if (sharp_bilinear_active(context)) {
        SharpBilinearPushConstants push;
        push.input_size[0] = (float)resolved->width;
        push.input_size[1] = (float)resolved->height;
        push.texture_size[0] = (float)resolved->width;
        push.texture_size[1] = (float)resolved->height;
        push.output_size[0] = (float)rect.width;
        push.output_size[1] = (float)rect.height;
        vkCmdBindPipeline(
            frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            context->sharp_bilinear_pipeline);
        vkCmdBindDescriptorSets(
            frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            context->sharp_bilinear_pipeline_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(
            frame->command_buffer, context->sharp_bilinear_pipeline_layout,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    } else {
        vkCmdBindPipeline(
            frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            context->blit_pipeline);
        vkCmdBindDescriptorSets(
            frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            context->blit_pipeline_layout, 0, 1, &set, 0, NULL);
    }
    vkCmdDraw(frame->command_buffer, 4, 1, 0, 0);

    if (overlay) {
        /* Vulkan counts Y from the top, so the top-origin destination is used
         * directly rather than mirrored. */
        const int base_x =
            overlay->space == SNESRECOMP_OVERLAY_SPACE_FRAME ? rect.x : 0;
        const int base_y =
            overlay->space == SNESRECOMP_OVERLAY_SPACE_FRAME ? rect.y : 0;
        const int base_width =
            overlay->space == SNESRECOMP_OVERLAY_SPACE_FRAME
                ? rect.width : (int)context->extent.width;
        const int base_height =
            overlay->space == SNESRECOMP_OVERLAY_SPACE_FRAME
                ? rect.height : (int)context->extent.height;

        vkCmdBindPipeline(
            frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            context->overlay_pipeline);
        for (i = 0; i < overlay->layer_count; i++) {
            const SnesRecompOverlayLayer *layer = &overlay->layers[i];
            SnesRecompVulkanRect layer_rect;
            VkDescriptorSet layer_set = VK_NULL_HANDLE;

            if (!bind_sampled_image(
                    presenter, frame, &frame->overlays[i],
                    layer->linear_filtering, &layer_set)) {
                vkCmdEndRenderPass(frame->command_buffer);
                return false;
            }
            layer_rect.x = base_x +
                (int)((int64_t)layer->x * base_width /
                      overlay->canvas_width);
            layer_rect.y = base_y +
                (int)((int64_t)layer->y * base_height /
                      overlay->canvas_height);
            layer_rect.width =
                (int)((int64_t)layer->display_width * base_width /
                      overlay->canvas_width);
            layer_rect.height =
                (int)((int64_t)layer->display_height * base_height /
                      overlay->canvas_height);
            set_viewport_and_scissor(
                frame->command_buffer, &layer_rect, context->extent);
            vkCmdBindDescriptorSets(
                frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                context->blit_pipeline_layout, 0, 1, &layer_set, 0, NULL);
            vkCmdDraw(frame->command_buffer, 4, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(frame->command_buffer);
    return true;
}


static bool vulkan_present(
    SnesRecompPresenter *presenter,
    const SnesRecompVideoFrame *frame_data) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VulkanFrame *frame = NULL;
    uint32_t image_index = 0;
    VkDeviceSize staging_bytes;
    bool skipped = false;

    if (!frame_data || !frame_data->pixels || frame_data->width <= 0 ||
        frame_data->height <= 0 ||
        frame_data->pixel_format != SNESRECOMP_PIXEL_FORMAT_ARGB8888 ||
        frame_data->pitch < frame_data->width * 4 ||
        (frame_data->pitch & 3) != 0) {
        snesrecomp_presenter_set_error(presenter, "invalid video frame");
        return false;
    }
    if (!validate_overlay(
            presenter, frame_data->overlay, frame_data->width,
            frame_data->height)) {
        return false;
    }

    if (!frame_begin(presenter, &frame, &skipped))
        return false;
    if (skipped)
        return true;

    /* Everything fallible happens here, before the command buffer opens: the
     * fence is still signalled, so bailing out now costs nothing. */
    if (!ensure_image(
            presenter, &frame->source, snesrecomp_vulkan_frame_format(),
            (uint32_t)frame_data->width, (uint32_t)frame_data->height,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_NULL_HANDLE) ||
        !prepare_overlay_images(presenter, frame, frame_data->overlay)) {
        return false;
    }
    if (sharp_bilinear_active(context) &&
        !ensure_image(
            presenter, &frame->scale_target, VK_FORMAT_R8G8B8A8_UNORM,
            (uint32_t)frame_data->width * 2u,
            (uint32_t)frame_data->height * 2u,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            context->offscreen_pass)) {
        return false;
    }
    staging_bytes =
        (VkDeviceSize)frame_data->width * frame_data->height * 4u +
        overlay_staging_bytes(frame_data->overlay);
    if (!reserve_staging(presenter, frame, staging_bytes))
        return false;

    if (!frame_acquire(presenter, frame, &image_index, &skipped))
        return false;
    if (skipped)
        return true;

    if (!upload_image(
            presenter, frame, &frame->source, frame_data->pixels,
            (uint32_t)frame_data->width, (uint32_t)frame_data->height, 4u,
            (size_t)frame_data->pitch) ||
        !upload_overlays(presenter, frame, frame_data->overlay) ||
        !record_presentation(
            presenter, frame, image_index, &frame->source,
            frame_data->width, frame_data->height, frame_data->overlay)) {
        /* The command buffer is open and the fence is reset, so it still has
         * to be submitted; the frame is simply incomplete. */
        (void)frame_submit(presenter, frame, image_index);
        return false;
    }
    return frame_submit(presenter, frame, image_index);
}


static uint8_t mode7_obj_texel(
    const uint16_t *vram,
    const SnesRecompObjSliver *sliver,
    unsigned x) {
    const unsigned word = (unsigned)sliver->tile * 16u + sliver->row;
    const uint16_t lo = vram[word & 0x7fffu];
    const uint16_t hi = vram[(word + 8u) & 0x7fffu];
    const unsigned bit = sliver->flip_h ? x : 7u - x;
    return (uint8_t)(((lo >> bit) & 1u) |
                     (((lo >> (bit + 8u)) & 1u) << 1u) |
                     (((hi >> bit) & 1u) << 2u) |
                     (((hi >> (bit + 8u)) & 1u) << 3u));
}

static bool build_mode7_obj_plane(
    SnesRecompPresenter *presenter,
    const SnesRecompMode7HdFrame *frame) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    const SnesPpuFrameCapture *cap = frame->capture;
    const size_t bytes =
        (size_t)cap->canvas_width * cap->visible_height * 2u;

    if (context->mode7_obj_capacity < bytes) {
        uint8_t *grown = (uint8_t *)realloc(context->mode7_obj_pixels, bytes);
        if (!grown) {
            snesrecomp_presenter_set_error(
                presenter, "out of memory for HD Mode 7 OBJ plane");
            return false;
        }
        context->mode7_obj_pixels = grown;
        context->mode7_obj_capacity = bytes;
    }
    memset(context->mode7_obj_pixels, 0, bytes);
    if (!frame->obj)
        return true;
    if (!frame->obj->slivers || frame->obj->count > frame->obj->capacity) {
        snesrecomp_presenter_set_error(
            presenter, "invalid HD Mode 7 OBJ frame");
        return false;
    }

    /* The evaluator emits back-to-front slivers. Plain overwrite here first
     * resolves OAM order into one native OBJ plane; priority is consulted only
     * afterwards by the Mode 7 compositor. */
    for (unsigned i = 0; i < frame->obj->count; i++) {
        const SnesRecompObjSliver *s = &frame->obj->slivers[i];
        if (s->line >= cap->visible_height || s->priority > 3u) {
            snesrecomp_presenter_set_error(
                presenter, "invalid HD Mode 7 OBJ sliver");
            return false;
        }
        for (unsigned px = 0; px < 8u; px++) {
            const int canvas_x =
                (int)cap->canvas_extra + (int)s->screen_x + (int)px;
            uint8_t index;
            uint8_t *dst;
            if (canvas_x < 0 || canvas_x >= (int)cap->canvas_width)
                continue;
            index = mode7_obj_texel(cap->vram, s, px);
            if (!index)
                continue;
            dst = context->mode7_obj_pixels +
                ((size_t)s->line * cap->canvas_width +
                 (unsigned)canvas_x) * 2u;
            dst[0] = (uint8_t)(s->palette_base + index);
            dst[1] = s->priority;
        }
    }
    return true;
}

static bool build_mode7_window_plane(
    SnesRecompPresenter *presenter,
    const SnesRecompMode7HdFrame *frame,
    SnesRecompSemanticLineState *lines) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    const SnesPpuFrameCapture *cap = frame->capture;
    const size_t pixels = (size_t)cap->canvas_width * cap->visible_height;
    const size_t bytes = pixels * 2u;
    size_t i;

    if (!lines) {
        snesrecomp_presenter_set_error(
            presenter, "missing HD Mode 7 semantic line output");
        return false;
    }
    if (context->mode7_window_capacity < bytes) {
        uint8_t *grown =
            (uint8_t *)realloc(context->mode7_window_pixels, bytes);
        if (!grown) {
            snesrecomp_presenter_set_error(
                presenter, "out of memory for HD Mode 7 window plane");
            return false;
        }
        context->mode7_window_pixels = grown;
        context->mode7_window_capacity = bytes;
    }
    if (!snesrecomp_ppu_compile_semantic_input(
            cap, context->mode7_window_pixels,
            context->mode7_window_pixels + pixels, cap->canvas_width,
            lines, SNES_PPU_MAX_BANDS)) {
        snesrecomp_presenter_set_error(
            presenter, "invalid HD Mode 7 BG1 window state");
        return false;
    }
    /* Bits 1/2 are unavailable to BG2/BG3 in the accepted Mode 7 subset, so
     * they carry OBJ main/sub permissions in the same compact R8 texture.
     * BG1-sub remains in its native bit 3 for a future larger subset. */
    for (i = 0; i < pixels; i++) {
        const uint8_t obj_bits = context->mode7_window_pixels[pixels + i];
        if (obj_bits & SNESRECOMP_SEMANTIC_OBJ_MAIN)
            context->mode7_window_pixels[i] |= 2u;
        else
            context->mode7_window_pixels[i] &= (uint8_t)(~2u & 0xFFu);
        if (obj_bits & SNESRECOMP_SEMANTIC_OBJ_SUB)
            context->mode7_window_pixels[i] |= 4u;
        else
            context->mode7_window_pixels[i] &= (uint8_t)(~4u & 0xFFu);
    }
    return true;
}

typedef struct Mode7Upload {
    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    const void *pixels;
} Mode7Upload;

/* The acceptance test, the host-side planes and the shader are the OpenGL
 * backend's. A frame this backend cannot render returns false so the caller
 * falls back to its authentic CPU frame. */
static bool vulkan_present_mode7_hd(
    SnesRecompPresenter *presenter,
    const SnesRecompMode7HdFrame *hd) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    uint8_t map_tex[SNESRECOMP_MODE7_TEXTURE_TEXELS];
    uint8_t char_tex[SNESRECOMP_MODE7_TEXTURE_TEXELS];
    uint8_t palette[SNES_PPU_CGRAM_ENTRIES * 4u];
    float affine[SNES_PPU_MAX_BANDS * 4u];
    uint8_t flags[SNES_PPU_MAX_BANDS * 4u];
    SnesRecompSemanticLineState semantic_lines[SNES_PPU_MAX_BANDS];
    Mode7Upload uploads[VULKAN_MODE7_INPUTS];
    Mode7PushConstants push;
    VulkanFrame *frame = NULL;
    VkDescriptorSet mode7_set = VK_NULL_HANDLE;
    VkRenderPassBeginInfo pass;
    VkClearValue clear;
    SnesRecompVulkanRect rect;
    VkExtent2D bounds;
    VkDeviceSize staging_bytes = 0;
    VkDeviceSize offset = 0;
    const SnesPpuFrameCapture *cap;
    const uint8_t *map_pixels;
    int map_width, map_height;
    uint32_t target_width, target_height;
    bool wants_obj = false;
    bool skipped = false;
    uint32_t image_index = 0;
    unsigned i;

    if (!hd || !(cap = hd->capture) || hd->scale != 2u || !hd->lines ||
        hd->line_count < cap->visible_height ||
        (hd->map_source &&
         !snesrecomp_ppu_mode7_map_source_valid(hd->map_source)) ||
        snesrecomp_ppu_mode7_supports(cap) != SNES_PPU_SUPPORTED ||
        cap->visible_height > SNES_PPU_MAX_BANDS ||
        cap->canvas_width > INT_MAX / 2 ||
        cap->visible_height > INT_MAX / 2) {
        snesrecomp_presenter_set_error(
            presenter, "unsupported HD Mode 7 frame");
        return false;
    }
    for (i = 0; i < cap->band_count; i++) {
        if (!cap->bands[i].forced_blank &&
            ((cap->bands[i].main_enable |
              cap->bands[i].sub_enable) & 0x10u)) {
            wants_obj = true;
        }
    }
    if (wants_obj && !hd->obj) {
        snesrecomp_presenter_set_error(
            presenter, "HD Mode 7 frame is missing qualified OBJ state");
        return false;
    }
    if (!validate_overlay(
            presenter, hd->overlay, (int)cap->canvas_width,
            (int)cap->visible_height)) {
        return false;
    }
    if (!build_mode7_obj_plane(presenter, hd) ||
        !build_mode7_window_plane(presenter, hd, semantic_lines) ||
        !snesrecomp_ppu_mode7_unpack_vram(
            cap->vram, hd->map_source ? NULL : map_tex, char_tex)) {
        return false;
    }

    if (hd->map_source) {
        map_pixels = hd->map_source->tiles;
        map_width = (int)hd->map_source->width_tiles;
        map_height = (int)hd->map_source->height_tiles;
    } else {
        map_pixels = map_tex;
        map_width = 128;
        map_height = 128;
    }

    memset(flags, 0, sizeof flags);
    for (i = 0; i < cap->visible_height; i++) {
        affine[i * 4u + 0u] = (float)hd->lines[i].start_x;
        affine[i * 4u + 1u] = (float)hd->lines[i].start_y;
        affine[i * 4u + 2u] = (float)hd->lines[i].step_x;
        affine[i * 4u + 3u] = (float)hd->lines[i].step_y;
    }
    for (i = 0; i < cap->band_count; i++) {
        const SnesPpuRasterBand *band = &cap->bands[i];
        unsigned y;
        for (y = band->y_begin; y < band->y_end; y++) {
            flags[y * 4u + 0u] = band->forced_blank ? 0u : band->brightness;
            flags[y * 4u + 1u] = band->bg[0].margin_left;
            flags[y * 4u + 2u] = band->main_enable;
            flags[y * 4u + 3u] = band->bg[0].margin_right;
        }
    }
    for (i = 0; i < SNES_PPU_CGRAM_ENTRIES; i++) {
        const uint16_t c = cap->cgram[i];
        palette[i * 4u + 0u] = (uint8_t)(c & 31u);
        palette[i * 4u + 1u] = (uint8_t)((c >> 5u) & 31u);
        palette[i * 4u + 2u] = (uint8_t)((c >> 10u) & 31u);
        palette[i * 4u + 3u] = 255u;
    }

    /* The eight semantic inputs, in the binding order the shader declares. */
    memset(uploads, 0, sizeof uploads);
    uploads[0].format = VK_FORMAT_R8_UNORM;
    uploads[0].width = (uint32_t)map_width;
    uploads[0].height = (uint32_t)map_height;
    uploads[0].bytes_per_pixel = 1u;
    uploads[0].pixels = map_pixels;
    uploads[1].format = VK_FORMAT_R8_UNORM;
    uploads[1].width = SNESRECOMP_MODE7_TEXTURE_DIM;
    uploads[1].height = SNESRECOMP_MODE7_TEXTURE_DIM;
    uploads[1].bytes_per_pixel = 1u;
    uploads[1].pixels = char_tex;
    uploads[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    uploads[2].width = SNES_PPU_CGRAM_ENTRIES;
    uploads[2].height = 1u;
    uploads[2].bytes_per_pixel = 4u;
    uploads[2].pixels = palette;
    uploads[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    uploads[3].width = 1u;
    uploads[3].height = cap->visible_height;
    uploads[3].bytes_per_pixel = 16u;
    uploads[3].pixels = affine;
    uploads[4].format = VK_FORMAT_R8G8B8A8_UNORM;
    uploads[4].width = 1u;
    uploads[4].height = cap->visible_height;
    uploads[4].bytes_per_pixel = 4u;
    uploads[4].pixels = flags;
    uploads[5].format = VK_FORMAT_R8G8_UNORM;
    uploads[5].width = cap->canvas_width;
    uploads[5].height = cap->visible_height;
    uploads[5].bytes_per_pixel = 2u;
    uploads[5].pixels = context->mode7_obj_pixels;
    uploads[6].format = VK_FORMAT_R8_UNORM;
    uploads[6].width = cap->canvas_width;
    uploads[6].height = cap->visible_height;
    uploads[6].bytes_per_pixel = 1u;
    uploads[6].pixels = context->mode7_window_pixels;
    uploads[7].format = VK_FORMAT_R8G8B8A8_UNORM;
    uploads[7].width = 2u;
    uploads[7].height = cap->visible_height;
    uploads[7].bytes_per_pixel = 4u;
    uploads[7].pixels = semantic_lines;

    target_width = (uint32_t)cap->canvas_width * 2u;
    target_height = (uint32_t)cap->visible_height * 2u;

    if (!frame_begin(presenter, &frame, &skipped))
        return false;
    if (skipped)
        return true;

    for (i = 0; i < VULKAN_MODE7_INPUTS; i++) {
        if (!ensure_image(
                presenter, &frame->mode7_inputs[i], uploads[i].format,
                uploads[i].width, uploads[i].height,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_NULL_HANDLE)) {
            return false;
        }
        staging_bytes =
            align_up(staging_bytes,
                     staging_alignment(uploads[i].bytes_per_pixel)) +
            (VkDeviceSize)uploads[i].width * uploads[i].height *
            uploads[i].bytes_per_pixel;
    }
    if (!ensure_image(
            presenter, &frame->mode7_target, VK_FORMAT_R8G8B8A8_UNORM,
            target_width, target_height,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            context->offscreen_pass) ||
        !prepare_overlay_images(presenter, frame, hd->overlay)) {
        return false;
    }
    if (sharp_bilinear_active(context) &&
        !ensure_image(
            presenter, &frame->scale_target, VK_FORMAT_R8G8B8A8_UNORM,
            target_width * 2u, target_height * 2u,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            context->offscreen_pass)) {
        return false;
    }
    staging_bytes = align_up(staging_bytes, staging_alignment(4u)) +
        overlay_staging_bytes(hd->overlay);
    if (!reserve_staging(presenter, frame, staging_bytes))
        return false;
    if (!bind_mode7_inputs(presenter, frame, &mode7_set))
        return false;

    if (!frame_acquire(presenter, frame, &image_index, &skipped))
        return false;
    if (skipped)
        return true;

    for (i = 0; i < VULKAN_MODE7_INPUTS; i++) {
        if (!upload_image(
                presenter, frame, &frame->mode7_inputs[i], uploads[i].pixels,
                uploads[i].width, uploads[i].height,
                uploads[i].bytes_per_pixel,
                (size_t)uploads[i].width * uploads[i].bytes_per_pixel)) {
            (void)frame_submit(presenter, frame, image_index);
            return false;
        }
    }
    if (!upload_overlays(presenter, frame, hd->overlay)) {
        (void)frame_submit(presenter, frame, image_index);
        return false;
    }

    memset(&clear, 0, sizeof clear);
    clear.color.float32[3] = 1.0f;
    memset(&pass, 0, sizeof pass);
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = context->offscreen_pass;
    pass.framebuffer = frame->mode7_target.framebuffer;
    pass.renderArea.extent.width = target_width;
    pass.renderArea.extent.height = target_height;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(
        frame->command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);

    rect.x = 0;
    rect.y = 0;
    rect.width = (int)target_width;
    rect.height = (int)target_height;
    bounds.width = target_width;
    bounds.height = target_height;
    set_viewport_and_scissor(frame->command_buffer, &rect, bounds);

    push.map_fixed_wrap[0] = (float)map_width * 8.0f * 256.0f;
    push.map_fixed_wrap[1] = (float)map_height * 8.0f * 256.0f;
    push.hd_scale = 2.0f;
    push.canvas_extra = (float)cap->canvas_extra;
    push.native_height = (float)cap->visible_height;
    push.bg_filter = hd->filter_bg ? 1.0f : 0.0f;
    push.line_lerp = hd->interpolate_lines ? 1.0f : 0.0f;
    push.reserved = 0.0f;

    vkCmdBindPipeline(
        frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        context->mode7_pipeline);
    vkCmdBindDescriptorSets(
        frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        context->mode7_pipeline_layout, 0, 1, &mode7_set, 0, NULL);
    vkCmdPushConstants(
        frame->command_buffer, context->mode7_pipeline_layout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    vkCmdBindVertexBuffers(
        frame->command_buffer, 0, 1, &context->vertex_buffer.buffer,
        &offset);
    vkCmdDraw(frame->command_buffer, 4, 1, 0, 0);
    vkCmdEndRenderPass(frame->command_buffer);
    frame->mode7_target.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (!record_presentation(
            presenter, frame, image_index, &frame->mode7_target,
            (int)cap->canvas_width, (int)cap->visible_height,
            hd->overlay)) {
        (void)frame_submit(presenter, frame, image_index);
        return false;
    }
    return frame_submit(presenter, frame, image_index);
}


static bool vulkan_set_fullscreen(
    SnesRecompPresenter *presenter,
    bool fullscreen) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    if (!SDL_SetWindowFullscreen(context->window, fullscreen))
        return set_sdl_error(presenter, "SDL_SetWindowFullscreen");
    context->swapchain_stale = true;
    return true;
}

static bool vulkan_set_window_scale(
    SnesRecompPresenter *presenter,
    int scale) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    if (scale <= 0) {
        snesrecomp_presenter_set_error(
            presenter, "window scale must be positive");
        return false;
    }
    if (!SDL_SetWindowSize(
            context->window,
            snesrecomp_presenter_display_width(
                presenter, presenter->frame_width) * scale,
            presenter->frame_height * scale)) {
        return set_sdl_error(presenter, "SDL_SetWindowSize");
    }
    context->swapchain_stale = true;
    return true;
}

static bool vulkan_set_window_title(
    SnesRecompPresenter *presenter,
    const char *title) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    if (!title || !title[0]) {
        snesrecomp_presenter_set_error(presenter, "window title is empty");
        return false;
    }
    if (!SDL_SetWindowTitle(context->window, title))
        return set_sdl_error(presenter, "SDL_SetWindowTitle");
    return true;
}

static bool vulkan_get_drawable_size(
    SnesRecompPresenter *presenter,
    int *width,
    int *height) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    if (!width || !height) {
        snesrecomp_presenter_set_error(
            presenter, "drawable-size output is null");
        return false;
    }
    if (!SDL_GetWindowSizeInPixels(context->window, width, height))
        return set_sdl_error(presenter, "SDL_GetWindowSizeInPixels");
    return true;
}

static float vulkan_get_display_scale(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    return SDL_GetWindowDisplayScale(context->window);
}

static void vulkan_destroy(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    unsigned i;
    if (!context)
        return;

    if (context->device) {
        /* Everything below may still be referenced by work in flight. */
        vkDeviceWaitIdle(context->device);
        for (i = 0; i < VULKAN_FRAMES_IN_FLIGHT; i++)
            destroy_frame(context->device, &context->frames[i]);
        destroy_buffer(context->device, &context->vertex_buffer);
        destroy_swapchain(context);
        if (context->sharp_bilinear_pipeline)
            vkDestroyPipeline(context->device, context->sharp_bilinear_pipeline, NULL);
        if (context->prescale_pipeline)
            vkDestroyPipeline(context->device, context->prescale_pipeline, NULL);
        if (context->mode7_pipeline)
            vkDestroyPipeline(context->device, context->mode7_pipeline, NULL);
        if (context->overlay_pipeline)
            vkDestroyPipeline(context->device, context->overlay_pipeline, NULL);
        if (context->blit_pipeline)
            vkDestroyPipeline(context->device, context->blit_pipeline, NULL);
        if (context->mode7_pipeline_layout) {
            vkDestroyPipelineLayout(
                context->device, context->mode7_pipeline_layout, NULL);
        }
        if (context->sharp_bilinear_pipeline_layout) {
            vkDestroyPipelineLayout(
                context->device, context->sharp_bilinear_pipeline_layout, NULL);
        }
        if (context->blit_pipeline_layout) {
            vkDestroyPipelineLayout(
                context->device, context->blit_pipeline_layout, NULL);
        }
        if (context->mode7_input_layout) {
            vkDestroyDescriptorSetLayout(
                context->device, context->mode7_input_layout, NULL);
        }
        if (context->sampler_layout) {
            vkDestroyDescriptorSetLayout(
                context->device, context->sampler_layout, NULL);
        }
        if (context->linear_sampler)
            vkDestroySampler(context->device, context->linear_sampler, NULL);
        if (context->nearest_sampler)
            vkDestroySampler(context->device, context->nearest_sampler, NULL);
        if (context->offscreen_pass)
            vkDestroyRenderPass(context->device, context->offscreen_pass, NULL);
        if (context->present_pass)
            vkDestroyRenderPass(context->device, context->present_pass, NULL);
        if (context->command_pool)
            vkDestroyCommandPool(context->device, context->command_pool, NULL);
        vkDestroyDevice(context->device, NULL);
        context->device = VK_NULL_HANDLE;
    }

    free(context->mode7_obj_pixels);
    free(context->mode7_window_pixels);
    if (context->surface && context->instance)
        SDL_Vulkan_DestroySurface(context->instance, context->surface, NULL);
    if (context->instance)
        vkDestroyInstance(context->instance, NULL);
    if (context->window)
        SDL_DestroyWindow(context->window);
    if (context->vulkan_library_loaded)
        SDL_Vulkan_UnloadLibrary();
    free(context);
    presenter->context = NULL;
}

static const SnesRecompPresenterOps kVulkanPresenterOps = {
    vulkan_destroy,
    vulkan_present,
    vulkan_set_fullscreen,
    vulkan_set_window_scale,
    vulkan_set_window_title,
    vulkan_get_drawable_size,
    vulkan_present_mode7_hd,
    vulkan_get_display_scale,
};


static bool create_instance(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkApplicationInfo application;
    VkInstanceCreateInfo info;
    const char *const *sdl_extensions = NULL;
    const char **extensions = NULL;
    uint32_t sdl_extension_count = 0;
    uint32_t extension_count = 0;
    VkResult result;
#if SNESRECOMP_PLATFORM_VULKAN_VALIDATION
    static const char *const kValidationLayers[] = {
        "VK_LAYER_KHRONOS_validation",
    };
#endif

    sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (!sdl_extensions || sdl_extension_count == 0)
        return set_sdl_error(presenter, "SDL_Vulkan_GetInstanceExtensions");

    /* One spare slot for the debug extension in a validation build. */
    extensions = (const char **)calloc(
        sdl_extension_count + 1u, sizeof(const char *));
    if (!extensions) {
        snesrecomp_presenter_set_error(
            presenter, "out of memory listing Vulkan instance extensions");
        return false;
    }
    for (extension_count = 0; extension_count < sdl_extension_count;
         extension_count++) {
        extensions[extension_count] = sdl_extensions[extension_count];
    }

    memset(&application, 0, sizeof application);
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "snesrecomp-platform";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "snesrecomp-platform";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_0;

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
#if SNESRECOMP_PLATFORM_VULKAN_VALIDATION
    extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    info.enabledLayerCount =
        (uint32_t)(sizeof kValidationLayers / sizeof kValidationLayers[0]);
    info.ppEnabledLayerNames = kValidationLayers;
#endif
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;

    result = vkCreateInstance(&info, NULL, &context->instance);
    free(extensions);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateInstance", result);
    return true;
}

static bool device_supports_swapchain(VkPhysicalDevice device) {
    VkExtensionProperties *properties = NULL;
    uint32_t count = 0;
    uint32_t i;
    bool found = false;

    if (vkEnumerateDeviceExtensionProperties(
            device, NULL, &count, NULL) != VK_SUCCESS || count == 0) {
        return false;
    }
    properties = (VkExtensionProperties *)calloc(count, sizeof *properties);
    if (!properties)
        return false;
    if (vkEnumerateDeviceExtensionProperties(
            device, NULL, &count, properties) == VK_SUCCESS) {
        for (i = 0; i < count && !found; i++) {
            found = strcmp(
                properties[i].extensionName,
                VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        }
    }
    free(properties);
    return found;
}

/* Graphics, presentation to this surface, VK_KHR_swapchain, at least one
 * surface format and one present mode. The families may differ. */
static bool device_queue_families(
    VkPhysicalDevice device,
    VkSurfaceKHR surface,
    uint32_t *out_graphics,
    uint32_t *out_present) {
    VkQueueFamilyProperties *families = NULL;
    uint32_t count = 0;
    uint32_t i;
    bool have_graphics = false, have_present = false;

    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    if (count == 0)
        return false;
    families = (VkQueueFamilyProperties *)calloc(count, sizeof *families);
    if (!families)
        return false;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);

    for (i = 0; i < count; i++) {
        VkBool32 present_supported = VK_FALSE;
        if (families[i].queueCount == 0)
            continue;
        if (!have_graphics &&
            (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            *out_graphics = i;
            have_graphics = true;
        }
        if (vkGetPhysicalDeviceSurfaceSupportKHR(
                device, i, surface, &present_supported) == VK_SUCCESS &&
            present_supported) {
            /* Prefer a family that can do both, which avoids the concurrent
             * sharing mode on the swapchain images. */
            if (!have_present ||
                (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                *out_present = i;
                have_present = true;
            }
        }
    }
    if (have_graphics && have_present &&
        *out_present != *out_graphics &&
        (families[*out_graphics].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        VkBool32 present_supported = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(
                device, *out_graphics, surface,
                &present_supported) == VK_SUCCESS && present_supported) {
            *out_present = *out_graphics;
        }
    }
    free(families);
    return have_graphics && have_present;
}

static bool select_physical_device(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkPhysicalDevice *devices = NULL;
    uint32_t count = 0;
    uint32_t i;
    int best_rank = INT_MAX;
    VkResult result;

    result = vkEnumeratePhysicalDevices(context->instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        snesrecomp_presenter_set_error(
            presenter, "no Vulkan physical device is available");
        return false;
    }
    devices = (VkPhysicalDevice *)calloc(count, sizeof *devices);
    if (!devices) {
        snesrecomp_presenter_set_error(
            presenter, "out of memory enumerating Vulkan devices");
        return false;
    }
    result = vkEnumeratePhysicalDevices(context->instance, &count, devices);
    if (result != VK_SUCCESS) {
        free(devices);
        return set_vk_error(presenter, "vkEnumeratePhysicalDevices", result);
    }

    for (i = 0; i < count; i++) {
        VkPhysicalDeviceProperties properties;
        uint32_t graphics = 0, present = 0;
        uint32_t format_count = 0, mode_count = 0;
        int rank;

        if (!device_supports_swapchain(devices[i]))
            continue;
        if (!device_queue_families(
                devices[i], context->surface, &graphics, &present)) {
            continue;
        }
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(
                devices[i], context->surface, &format_count,
                NULL) != VK_SUCCESS || format_count == 0) {
            continue;
        }
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(
                devices[i], context->surface, &mode_count,
                NULL) != VK_SUCCESS || mode_count == 0) {
            continue;
        }

        vkGetPhysicalDeviceProperties(devices[i], &properties);
        rank = snesrecomp_vulkan_device_rank(properties.deviceType);
        /* Preference, not a requirement: any device that clears the checks
         * above is acceptable, and a strictly better rank replaces it. */
        if (rank < best_rank) {
            best_rank = rank;
            context->physical_device = devices[i];
            context->device_properties = properties;
            context->graphics_family = graphics;
            context->present_family = present;
        }
    }
    free(devices);

    if (best_rank == INT_MAX) {
        snesrecomp_presenter_set_error(
            presenter,
            "no Vulkan device has graphics, presentation and "
            "VK_KHR_swapchain for this window");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(
        context->physical_device, &context->memory_properties);
    return true;
}

static bool create_device(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    static const char *const kDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    VkDeviceQueueCreateInfo queues[2];
    VkPhysicalDeviceFeatures features;
    VkDeviceCreateInfo info;
    const float priority = 1.0f;
    uint32_t queue_count = 1;
    VkResult result;

    memset(queues, 0, sizeof queues);
    queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[0].queueFamilyIndex = context->graphics_family;
    queues[0].queueCount = 1;
    queues[0].pQueuePriorities = &priority;
    if (context->present_family != context->graphics_family) {
        queues[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[1].queueFamilyIndex = context->present_family;
        queues[1].queueCount = 1;
        queues[1].pQueuePriorities = &priority;
        queue_count = 2;
    }

    /* No optional feature is requested: the backend is Vulkan 1.0 core. */
    memset(&features, 0, sizeof features);

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = queue_count;
    info.pQueueCreateInfos = queues;
    info.enabledExtensionCount =
        (uint32_t)(sizeof kDeviceExtensions / sizeof kDeviceExtensions[0]);
    info.ppEnabledExtensionNames = kDeviceExtensions;
    info.pEnabledFeatures = &features;
    result = vkCreateDevice(
        context->physical_device, &info, NULL, &context->device);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateDevice", result);

    vkGetDeviceQueue(
        context->device, context->graphics_family, 0,
        &context->graphics_queue);
    vkGetDeviceQueue(
        context->device, context->present_family, 0,
        &context->present_queue);
    return true;
}

/* Chosen once: a render pass is tied to its attachment format, and the
 * swapchain is rebuilt far more often than the surface changes. */
static bool select_surface_format(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkSurfaceFormatKHR *formats = NULL;
    uint32_t count = 0;
    bool ok;

    if (vkGetPhysicalDeviceSurfaceFormatsKHR(
            context->physical_device, context->surface, &count,
            NULL) != VK_SUCCESS || count == 0) {
        snesrecomp_presenter_set_error(
            presenter, "the Vulkan surface reports no formats");
        return false;
    }
    formats = (VkSurfaceFormatKHR *)calloc(count, sizeof *formats);
    if (!formats) {
        snesrecomp_presenter_set_error(
            presenter, "out of memory reading Vulkan surface formats");
        return false;
    }
    ok = vkGetPhysicalDeviceSurfaceFormatsKHR(
             context->physical_device, context->surface, &count,
             formats) == VK_SUCCESS &&
         snesrecomp_vulkan_choose_surface_format(
             formats, count, &context->surface_format);
    free(formats);
    if (!ok) {
        snesrecomp_presenter_set_error(
            presenter, "no usable Vulkan surface format");
    }
    return ok;
}

static bool create_command_pool(SnesRecompPresenter *presenter) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)presenter->context;
    VkCommandPoolCreateInfo info;
    VkResult result;

    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = context->graphics_family;
    result = vkCreateCommandPool(
        context->device, &info, NULL, &context->command_pool);
    if (result != VK_SUCCESS)
        return set_vk_error(presenter, "vkCreateCommandPool", result);
    return true;
}

bool snesrecomp_presenter_vulkan_create(
    SnesRecompPresenter *presenter,
    const SnesRecompPresentConfig *config) {
    VulkanPresenterContext *context =
        (VulkanPresenterContext *)calloc(1, sizeof(*context));
    bool skipped = false;

    if (!context) {
        snesrecomp_presenter_set_error(
            presenter, "out of memory creating Vulkan presenter");
        return false;
    }

    presenter->ops = &kVulkanPresenterOps;
    presenter->context = context;
    context->preserve_aspect = config->preserve_aspect;
    context->linear_filtering = config->linear_filtering;
    context->scaling = config->scaling;
    context->vsync_requested = config->vsync;

    /* The loader has to be up before instance extensions can be queried. */
    if (!SDL_Vulkan_LoadLibrary(NULL))
        return set_sdl_error(presenter, "SDL_Vulkan_LoadLibrary");
    context->vulkan_library_loaded = true;

    context->window = SDL_CreateWindow(
        config->window_title,
        snesrecomp_presenter_display_width(
            presenter, config->frame_width) * config->window_scale,
        config->frame_height * config->window_scale,
        SDL_WINDOW_VULKAN |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!context->window)
        return set_sdl_error(presenter, "SDL_CreateWindow(Vulkan)");

    if (!create_instance(presenter))
        return false;
    if (!SDL_Vulkan_CreateSurface(
            context->window, context->instance, NULL, &context->surface)) {
        return set_sdl_error(presenter, "SDL_Vulkan_CreateSurface");
    }
    if (!select_physical_device(presenter) ||
        !create_device(presenter) ||
        !select_surface_format(presenter) ||
        !create_render_passes(presenter) ||
        !create_command_pool(presenter) ||
        !create_samplers(presenter) ||
        !create_layouts(presenter) ||
        !create_pipelines(presenter) ||
        !create_vertex_buffer(presenter) ||
        !create_frames(presenter)) {
        return false;
    }
    if (!create_swapchain(presenter, &skipped))
        return false;
    if (skipped) {
        context->swapchain_stale = true;
        presenter->vsync_state = SNESRECOMP_VSYNC_UNKNOWN;
    }

    if (config->fullscreen) {
        if (!SDL_SetWindowFullscreen(context->window, true))
            return set_sdl_error(presenter, "SDL_SetWindowFullscreen");
        context->swapchain_stale = true;
    }

    presenter->backend = SNESRECOMP_PRESENT_BACKEND_VULKAN;
    /* No SHADER or MULTIPASS: the backend has internal shaders but interprets
     * no user preset. */
    presenter->capabilities =
        SNESRECOMP_PRESENT_CAP_BASIC |
        SNESRECOMP_PRESENT_CAP_OVERLAYS |
        SNESRECOMP_PRESENT_CAP_HD_MODE7;

    snprintf(
        presenter->backend_name,
        sizeof(presenter->backend_name),
        "Vulkan 1.0/%s/%s%s",
        context->device_properties.deviceName[0]
            ? context->device_properties.deviceName : "unknown device",
        snesrecomp_vulkan_present_mode_name(context->present_mode),
        sharp_bilinear_active(context) ? "/sharp-bilinear" : "");
    presenter->last_error[0] = '\0';
    return true;
}
