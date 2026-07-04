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

#include <memory>

#include <vulkan/vulkan.h>

#include "vrto3dlib/stereo_config.h"

namespace vrto3d::vk {
struct DeviceCtx;
}

namespace vrto3d {

// Linux presenter seam: owns the native window/surface + Vulkan swapchain on
// the chosen output. Two implementations: WaylandPresenter (layer-shell
// overlay with xdg_toplevel fullscreen fallback) and X11Presenter (positioned
// borderless topmost window, XRandR modelines).
//
// Thread model: ALL methods are called from the renderer's present thread
// (which is also the thread that created the object via MakeVkPresenter).
class IVkPresenter {
public:
    virtual ~IVkPresenter() = default;

    struct FrameTarget {
        VkImage       image = VK_NULL_HANDLE;
        VkImageView   view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t      index = 0;
    };

    // Creates the native window fullscreen-covering the output selected by
    // cfg.display_index (1-based; 0 = primary), the VkSurfaceKHR, a FIFO
    // swapchain, and a render pass targeting the swapchain format
    // (loadOp=DONT_CARE, storeOp=STORE, finalLayout=PRESENT_SRC).
    virtual bool Init(vrto3d::vk::DeviceCtx* ctx, const StereoDisplayDriverConfiguration& cfg) = 0;
    virtual void Shutdown() = 0;

    // Drain native display-server events. Returns false when the surface was
    // closed/destroyed and presenting must stop.
    virtual bool PumpEvents() = 0;

    // Acquire the next swapchain image; `signal_sem` is signaled when the
    // image is ready to be rendered to. Handles OUT_OF_DATE by recreating
    // the swapchain internally (returns false for "skip this frame").
    virtual bool AcquireNext(FrameTarget* out, VkSemaphore signal_sem) = 0;

    // Queue the present for the acquired image, waiting on `wait_sem`.
    // FIFO mode: this is the frame-pacing block.
    virtual bool Present(uint32_t image_index, VkSemaphore wait_sem) = 0;

    virtual VkRenderPass RenderPass() const = 0;
    virtual VkExtent2D   Extent() const = 0;
    virtual VkFormat     Format() const = 0;
    virtual const char*  Name() const = 0;

    // Best-effort topmost control (X11: _NET_WM_STATE_ABOVE; Wayland
    // layer-shell: inherently on the overlay layer — no-ops).
    virtual void BringToTop() {}
    virtual void ReleaseTopmost() {}
};

// Session-based selection: Wayland when WAYLAND_DISPLAY is set and connectable
// (unless cfg overrides via linux_presenter = "x11"/"wayland"), else X11 via
// DISPLAY. Returns nullptr when neither display server is reachable.
std::unique_ptr<IVkPresenter> MakeVkPresenter(const StereoDisplayDriverConfiguration& cfg);

}  // namespace vrto3d
