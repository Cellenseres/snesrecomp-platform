#ifndef SNESRECOMP_PLATFORM_VULKAN_POLICY_H
#define SNESRECOMP_PLATFORM_VULKAN_POLICY_H

/* Device-independent Vulkan selection policy. Nothing here calls a Vulkan
 * entry point, which is what makes swapchain and memory choices testable on a
 * machine with no Vulkan device. */

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "snesrecomp_platform/presenter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompVulkanRect {
    int x;
    int y;
    int width;
    int height;
} SnesRecompVulkanRect;

/* SNESRECOMP_PIXEL_FORMAT_ARGB8888 is a host uint32_t 0xAARRGGBB, so on a
 * little-endian host its bytes are B,G,R,A. */
VkFormat snesrecomp_vulkan_frame_format(void);

void snesrecomp_vulkan_frame_pixel_bytes(uint32_t argb, uint8_t out[4]);

bool snesrecomp_vulkan_frame_format_is_native(void);

/* UNORM is preferred over the matching _SRGB: submitted frames are already
 * display-encoded, and an _SRGB swapchain would apply the transfer twice. */
bool snesrecomp_vulkan_choose_surface_format(
    const VkSurfaceFormatKHR *formats,
    uint32_t count,
    VkSurfaceFormatKHR *out_format);

bool snesrecomp_vulkan_choose_present_mode(
    const VkPresentModeKHR *modes,
    uint32_t count,
    bool vsync,
    VkPresentModeKHR *out_mode);

/* What the mode does about tearing, which is not what the configuration
 * asked for. MAILBOX presents whole refreshes, so it reports as enabled. */
SnesRecompVSyncState snesrecomp_vulkan_present_mode_vsync_state(
    VkPresentModeKHR mode);

const char *snesrecomp_vulkan_present_mode_name(VkPresentModeKHR mode);

void snesrecomp_vulkan_choose_extent(
    const VkSurfaceCapabilitiesKHR *caps,
    uint32_t drawable_width,
    uint32_t drawable_height,
    VkExtent2D *out_extent);

uint32_t snesrecomp_vulkan_choose_image_count(
    const VkSurfaceCapabilitiesKHR *caps);

bool snesrecomp_vulkan_find_memory_type(
    const VkPhysicalDeviceMemoryProperties *properties,
    uint32_t type_bits,
    VkMemoryPropertyFlags required,
    uint32_t *out_index);

/* Lower sorts better: discrete, integrated, virtual, CPU, other. */
int snesrecomp_vulkan_device_rank(VkPhysicalDeviceType type);

/* The OpenGL backend's viewport arithmetic, expressed once so the two cannot
 * drift. `display_width` comes from snesrecomp_presenter_display_width(). */
void snesrecomp_vulkan_fit_viewport(
    int drawable_width,
    int drawable_height,
    int display_width,
    int logical_height,
    bool preserve_aspect,
    SnesRecompVulkanRect *out_rect);

const char *snesrecomp_vulkan_result_name(VkResult result);

#ifdef __cplusplus
}
#endif

#endif
