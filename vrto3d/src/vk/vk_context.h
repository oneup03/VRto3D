/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>

namespace vrto3d::vk {

// Shared Vulkan device context for the Linux renderer. One per driver.
//
// Thread model: the queue is used from the compositor thread (dmabuf imports)
// and the present thread (frame rendering) — every vkQueueSubmit/vkQueuePresentKHR
// must hold queue_mutex.
struct DeviceCtx {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    uint32_t         queue_family = 0;
    VkQueue          queue = VK_NULL_HANDLE;
    std::mutex       queue_mutex;
    VkPhysicalDeviceMemoryProperties mem_props{};

    // Creates instance + device with the extensions the Linux port needs:
    //   instance: VK_KHR_surface, VK_KHR_wayland_surface, VK_KHR_xcb_surface,
    //             VK_KHR_xlib_surface (each only if supported),
    //             VK_KHR_get_physical_device_properties2
    //   device:   VK_KHR_swapchain, VK_KHR_external_memory_fd,
    //             VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier,
    //             VK_KHR_external_semaphore_fd, VK_KHR_timeline_semaphore
    // Picks the first physical device that has the required device extensions
    // and a graphics queue with present support.
    bool Init();
    void Destroy();

    // Index into mem_props matching type_bits + required property flags, or
    // UINT32_MAX when no type matches.
    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags props) const;
};

// Small helpers shared by the renderer / presenters / OSD backend.
VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* spirv_words, size_t byte_size);

// One-line VkResult logging helper. Returns r for chaining.
VkResult LogIfFailed(VkResult r, const char* what);

}  // namespace vrto3d::vk
