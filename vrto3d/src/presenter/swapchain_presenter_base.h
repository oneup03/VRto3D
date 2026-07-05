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

#include "presenter/vk_presenter.h"
#include "presenter/vk_swapchain_util.h"

namespace vrto3d {

// Shared IVkPresenter plumbing for presenters backed by a SwapchainBundle
// (WaylandPresenter, X11Presenter). These delegations were identical in both;
// subclasses now implement only the native-windowing surface (Init, Shutdown,
// PumpEvents, SetAlwaysOnTop, SetInputCapture, Name) and populate ctx_ +
// swapchain_ during Init. Presenters that don't own a swapchain (e.g.
// WibbleWobblePresenter, which streams dmabufs) implement IVkPresenter directly.
class SwapchainPresenterBase : public IVkPresenter {
public:
    bool AcquireNext(FrameTarget* out, VkSemaphore signal_sem) override {
        return swapchain_.AcquireNext(out, signal_sem);
    }
    bool Present(uint32_t image_index, VkSemaphore wait_sem) override {
        return swapchain_.Present(image_index, wait_sem);
    }
    VkRenderPass RenderPass() const override { return swapchain_.render_pass; }
    VkExtent2D   Extent() const override { return swapchain_.extent; }
    VkFormat     Format() const override { return swapchain_.format; }

protected:
    vk::DeviceCtx*  ctx_ = nullptr;
    SwapchainBundle swapchain_;
};

}  // namespace vrto3d
