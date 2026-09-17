#include "vulkan_policy.h"

#include <string.h>

VkFormat snesrecomp_vulkan_frame_format(void) {
    return VK_FORMAT_B8G8R8A8_UNORM;
}

void snesrecomp_vulkan_frame_pixel_bytes(uint32_t argb, uint8_t out[4]) {
    out[0] = (uint8_t)(argb & 0xFFu);
    out[1] = (uint8_t)((argb >> 8) & 0xFFu);
    out[2] = (uint8_t)((argb >> 16) & 0xFFu);
    out[3] = (uint8_t)((argb >> 24) & 0xFFu);
}

bool snesrecomp_vulkan_frame_format_is_native(void) {
    const uint32_t probe = 0x01020304u;
    uint8_t bytes[4];
    memcpy(bytes, &probe, sizeof bytes);
    return bytes[0] == 0x04u && bytes[1] == 0x03u &&
           bytes[2] == 0x02u && bytes[3] == 0x01u;
}

static bool surface_format_is_preferred(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM ||
           format == VK_FORMAT_R8G8B8A8_UNORM;
}

static bool surface_format_is_srgb_encoded(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

bool snesrecomp_vulkan_choose_surface_format(
    const VkSurfaceFormatKHR *formats,
    uint32_t count,
    VkSurfaceFormatKHR *out_format) {
    uint32_t i;
    if (!formats || count == 0 || !out_format)
        return false;

    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        out_format->format = VK_FORMAT_B8G8R8A8_UNORM;
        out_format->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return true;
    }

    for (i = 0; i < count; i++) {
        if (surface_format_is_preferred(formats[i].format) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            *out_format = formats[i];
            return true;
        }
    }
    for (i = 0; i < count; i++) {
        if (!surface_format_is_srgb_encoded(formats[i].format) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            *out_format = formats[i];
            return true;
        }
    }
    *out_format = formats[0];
    return true;
}

bool snesrecomp_vulkan_choose_present_mode(
    const VkPresentModeKHR *modes,
    uint32_t count,
    bool vsync,
    VkPresentModeKHR *out_mode) {
    /*
     * MAILBOX first for VSync on. Both it and FIFO are tear-free; they
     * differ in who waits. Measured on an RTX 4060 at 60 Hz, windowed FIFO
     * paces at exactly 32.00 fps -- neither 60 nor the 30 a missed refresh
     * gives -- at three through six swapchain images, so it is a throttle
     * rather than a pipelining fault. MAILBOX never blocks, so pacing falls
     * to the caller, where it belongs: an emulator runs at the guest rate
     * and owns a frame limiter anyway. A caller without one pins FIFO
     * through SNESRECOMP_VULKAN_PRESENT_MODE.
     */
    static const VkPresentModeKHR kVsyncOn[] = {
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    static const VkPresentModeKHR kVsyncOff[] = {
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_FIFO_KHR,
    };
    const VkPresentModeKHR *order = vsync ? kVsyncOn : kVsyncOff;
    const size_t order_count =
        vsync ? sizeof kVsyncOn / sizeof kVsyncOn[0]
              : sizeof kVsyncOff / sizeof kVsyncOff[0];
    size_t want;
    uint32_t i;

    if (!modes || count == 0 || !out_mode)
        return false;

    for (want = 0; want < order_count; want++) {
        for (i = 0; i < count; i++) {
            if (modes[i] == order[want]) {
                *out_mode = order[want];
                return true;
            }
        }
    }
    *out_mode = modes[0];
    return true;
}

SnesRecompVSyncState snesrecomp_vulkan_present_mode_vsync_state(
    VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return SNESRECOMP_VSYNC_DISABLED;
    case VK_PRESENT_MODE_FIFO_KHR:
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return SNESRECOMP_VSYNC_ENABLED;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return SNESRECOMP_VSYNC_ENABLED;
    default:
        return SNESRECOMP_VSYNC_UNKNOWN;
    }
}

const char *snesrecomp_vulkan_present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO_RELAXED";
    default:
        return "other";
    }
}

void snesrecomp_vulkan_choose_extent(
    const VkSurfaceCapabilitiesKHR *caps,
    uint32_t drawable_width,
    uint32_t drawable_height,
    VkExtent2D *out_extent) {
    uint32_t width, height;
    if (!caps || !out_extent)
        return;

    if (caps->currentExtent.width != 0xFFFFFFFFu) {
        *out_extent = caps->currentExtent;
        return;
    }

    width = drawable_width;
    height = drawable_height;
    if (width < caps->minImageExtent.width)
        width = caps->minImageExtent.width;
    if (width > caps->maxImageExtent.width)
        width = caps->maxImageExtent.width;
    if (height < caps->minImageExtent.height)
        height = caps->minImageExtent.height;
    if (height > caps->maxImageExtent.height)
        height = caps->maxImageExtent.height;
    out_extent->width = width;
    out_extent->height = height;
}

uint32_t snesrecomp_vulkan_choose_image_count(
    const VkSurfaceCapabilitiesKHR *caps) {
    uint32_t count;
    if (!caps)
        return 2u;
    count = caps->minImageCount + 2u;
    if (caps->maxImageCount > 0u && count > caps->maxImageCount)
        count = caps->maxImageCount;
    return count;
}

bool snesrecomp_vulkan_find_memory_type(
    const VkPhysicalDeviceMemoryProperties *properties,
    uint32_t type_bits,
    VkMemoryPropertyFlags required,
    uint32_t *out_index) {
    uint32_t i;
    if (!properties || !out_index)
        return false;
    for (i = 0; i < properties->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) == 0u)
            continue;
        if ((properties->memoryTypes[i].propertyFlags & required) != required)
            continue;
        *out_index = i;
        return true;
    }
    return false;
}

int snesrecomp_vulkan_device_rank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 3;
    default:
        return 4;
    }
}

void snesrecomp_vulkan_fit_viewport(
    int drawable_width,
    int drawable_height,
    int display_width,
    int logical_height,
    bool preserve_aspect,
    SnesRecompVulkanRect *out_rect) {
    int width, height;
    if (!out_rect)
        return;

    width = drawable_width;
    height = drawable_height;
    if (preserve_aspect && display_width > 0 && logical_height > 0) {
        if ((int64_t)width * logical_height <
            (int64_t)height * display_width) {
            height = width * logical_height / display_width;
        } else {
            width = height * display_width / logical_height;
        }
    }
    out_rect->width = width;
    out_rect->height = height;
    out_rect->x = (drawable_width - width) / 2;
    out_rect->y = (drawable_height - height) / 2;
}

const char *snesrecomp_vulkan_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
        return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT:
        return "VK_ERROR_VALIDATION_FAILED_EXT";
    default: return "VK_ERROR_UNKNOWN";
    }
}
