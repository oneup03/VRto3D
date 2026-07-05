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
#include <string>
#include <vector>

#include "presenter/vk_presenter.h"
#include "presenter/vk_swapchain_util.h"
#include "presenter/x11_modeline.h"

// Xlib types — kept out of the header so it doesn't leak X11 macros
// (None/Bool/Status...) into every includer. XID-derived handles are
// unsigned long by definition.
struct _XDisplay;

namespace vrto3d {

// Xlib implementation of IVkPresenter: a borderless, topmost,
// _NET_WM_STATE_FULLSCREEN window positioned on the XRandR output selected
// by cfg.display_index (1-based connected-output order; 0 = the primary
// output). DualDisplay/DualDisplayFlip span the chosen output plus its
// contiguous right neighbor when one with identical geometry exists.
class X11Presenter final : public IVkPresenter {
public:
    X11Presenter() = default;
    ~X11Presenter() override;

    bool Init(vrto3d::vk::DeviceCtx* ctx, const StereoDisplayDriverConfiguration& cfg) override;
    void Shutdown() override;

    bool PumpEvents() override;
    bool AcquireNext(FrameTarget* out, VkSemaphore signal_sem) override;
    bool Present(uint32_t image_index, VkSemaphore wait_sem) override;

    VkRenderPass RenderPass() const override { return swapchain_.render_pass; }
    VkExtent2D   Extent() const override { return swapchain_.extent; }
    VkFormat     Format() const override { return swapchain_.format; }
    const char*  Name() const override { return "X11Presenter"; }

    void BringToTop() override;
    void ReleaseTopmost() override;
    void SetAlwaysOnTop(bool on_top) override;
    void SetInputCapture(bool capture) override;

private:
    struct OutputGeom {
        int32_t     x = 0;
        int32_t     y = 0;
        uint32_t    width = 0;
        uint32_t    height = 0;
        float       refresh_hz = 60.0f;
        std::string name;
        bool        is_primary = false;
        int32_t     xinerama_index = 0;   // connected-output order, 0-based
    };

    // Connected XRandR outputs in enumeration order.
    std::vector<OutputGeom> EnumerateOutputs() const;
    void SendNetWmState(long action, unsigned long property, unsigned long property2) const;
    void SendFullscreenMonitors(const OutputGeom& left, const OutputGeom& right) const;

    vk::DeviceCtx* ctx_ = nullptr;

    _XDisplay*     dpy_ = nullptr;
    X11ModelineState modeline_state_;
    unsigned long  window_ = 0;           // Window (XID)
    unsigned long  wm_delete_window_ = 0; // Atom
    bool           have_xshape_ = false;
    uint32_t       width_ = 0;
    uint32_t       height_ = 0;
    bool           closed_ = false;

    VkSurfaceKHR    vk_surface_ = VK_NULL_HANDLE;
    SwapchainBundle swapchain_;
};

}  // namespace vrto3d
