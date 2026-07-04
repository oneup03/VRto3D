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

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "presenter/vk_presenter.h"
#include "vk/vk_context.h"

namespace vrto3d {

// Minimal stderr logger for the Linux presenter files (printf-style). The
// driver's DebugLog is Windows-only; vrserver captures driver stderr.
void PresenterLog(const char* fmt, ...);

// Shared FIFO swapchain + render pass + framebuffers for the Linux
// presenters (Wayland/X11). The presenter owns the VkSurfaceKHR; this bundle
// owns everything derived from it.
//
// Recreate policy (matches IVkPresenter's contract):
//   - Callers set `needs_recreate` on native resize events.
//   - AcquireNext() recreates BEFORE acquiring when flagged or on
//     VK_ERROR_OUT_OF_DATE_KHR (returns false — skip this frame; the
//     signal semaphore is never left pending).
//   - VK_SUBOPTIMAL_KHR at acquire proceeds with the frame (the semaphore
//     is already signaled) and flags a recreate for the next frame.
//   - Present() flags a recreate on VK_SUBOPTIMAL_KHR / OUT_OF_DATE.
//
// Thread model: all methods on the present thread. Present() takes
// ctx->queue_mutex around vkQueuePresentKHR.
struct SwapchainBundle {
    vk::DeviceCtx* ctx = nullptr;
    VkSurfaceKHR   surface = VK_NULL_HANDLE;   // not owned
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass   render_pass = VK_NULL_HANDLE;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D     extent{};
    std::vector<VkImage>       images;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> framebuffers;
    bool           needs_recreate = false;

    // `desired_w/h` are the fallback extent when the surface reports
    // currentExtent = 0xFFFFFFFF (Wayland lets the client pick).
    uint32_t desired_w = 0;
    uint32_t desired_h = 0;

    // Verifies present support on ctx->queue_family, picks
    // VK_FORMAT_B8G8R8A8_SRGB when available (else the first reported
    // format), creates the FIFO swapchain, render pass (loadOp=DONT_CARE,
    // storeOp=STORE, finalLayout=PRESENT_SRC), image views and framebuffers.
    bool Create(vk::DeviceCtx* ctx, VkSurfaceKHR surface, uint32_t desired_w, uint32_t desired_h);

    // Waits for the device to go idle (under queue_mutex) and rebuilds the
    // swapchain with oldSwapchain retirement. Render pass is kept (the
    // surface format does not change across resizes).
    bool Recreate();

    void Destroy();

    bool AcquireNext(IVkPresenter::FrameTarget* out, VkSemaphore signal_sem);
    bool Present(uint32_t image_index, VkSemaphore wait_sem);

    // Update the fallback extent (called from native configure events).
    void SetDesiredExtent(uint32_t w, uint32_t h);

private:
    bool CreateSwapchainObjects(VkSwapchainKHR old_swapchain);
    void DestroySwapchainObjects(bool keep_render_pass);
};

}  // namespace vrto3d
