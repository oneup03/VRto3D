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

#include "presenter/swapchain_presenter_base.h"
#include "presenter/vk_swapchain_util.h"

// Raw libwayland-client types (from wayland-client-protocol.h and the
// generated protocol headers) — forward-declared here so the header stays
// light; the .cpp includes the real headers.
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_output;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

namespace vrto3d {

// Wayland implementation of IVkPresenter.
//
// Preferred surface role: zwlr_layer_surface_v1 on the OVERLAY layer of the
// chosen output (anchored to all four edges, exclusive_zone -1, no keyboard
// interactivity) — inherently fullscreen and topmost, no WM fights. When the
// compositor lacks wlr-layer-shell (e.g. GNOME Mutter), falls back to an
// xdg_toplevel with set_fullscreen(output).
//
// Event handling: PumpEvents() only does wl_display_dispatch_pending +
// wl_display_flush — never a blocking dispatch. The Vulkan WSI runs its own
// wl_event_queue and reads the socket during FIFO presents, which also
// queues our default-queue events for the next dispatch_pending. Blocking
// roundtrips are confined to Init().
class WaylandPresenter final : public SwapchainPresenterBase {
public:
    WaylandPresenter() = default;
    ~WaylandPresenter() override;

    bool Init(vrto3d::vk::DeviceCtx* ctx, const StereoDisplayDriverConfiguration& cfg) override;
    void Shutdown() override;

    bool PumpEvents() override;
    // AcquireNext/Present/RenderPass/Extent/Format inherited from
    // SwapchainPresenterBase (delegate to swapchain_).
    const char*  Name() const override { return "WaylandPresenter"; }

    // Layer-shell overlay surfaces are inherently topmost; xdg fullscreen
    // z-order is compositor policy. Both are no-ops (SetAlwaysOnTop replaces
    // them for the peek-at-game toggle).
    void BringToTop() override {}
    void ReleaseTopmost() override {}
    void SetAlwaysOnTop(bool on_top) override;
    void SetInputCapture(bool capture) override;

    // --- listener plumbing (public for the C callback trampolines) ---
    struct OutputInfo {
        wl_output*  output = nullptr;
        uint32_t    global_name = 0;   // registry name (for remove tracking)
        int32_t     x = 0;
        int32_t     y = 0;
        int32_t     width = 0;         // current mode
        int32_t     height = 0;
        int32_t     refresh_mhz = 0;
        std::string name;              // wl_output.name (v4) or make/model
        std::string make;
        std::string model;
        bool        done = false;
    };

    void OnGlobal(wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    void OnGlobalRemove(uint32_t name);
    void OnLayerConfigure(uint32_t serial, uint32_t width, uint32_t height);
    void OnLayerClosed();
    void OnXdgSurfaceConfigure(uint32_t serial);
    void OnXdgToplevelConfigure(int32_t width, int32_t height);
    void OnXdgToplevelClose();

    std::vector<OutputInfo>& Outputs() { return outputs_; }

private:
    bool ConnectAndBind();
    bool CreateLayerSurface(wl_output* output);
    bool CreateXdgFullscreenSurface(wl_output* output);
    bool WaitForConfigure();
    void DestroyNative();
    // Empty input region on surface_ = click-through; null = capture whole
    // surface. Shared by surface init and SetInputCapture.
    void SetSurfaceInputRegion(bool capture);

    // ctx_ + swapchain_ live in SwapchainPresenterBase.

    wl_display*    display_ = nullptr;
    wl_registry*   registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base*   wm_base_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    std::vector<OutputInfo> outputs_;

    wl_surface*            surface_ = nullptr;
    zwlr_layer_surface_v1* layer_surface_ = nullptr;
    xdg_surface*           xdg_surface_ = nullptr;
    xdg_toplevel*          xdg_toplevel_ = nullptr;

    VkSurfaceKHR    vk_surface_ = VK_NULL_HANDLE;

    // Configure-driven state.
    bool     configured_ = false;
    bool     closed_ = false;
    uint32_t surf_width_ = 0;      // latest configure size (0 = pick our own)
    uint32_t surf_height_ = 0;
    int32_t  pending_xdg_w_ = 0;   // xdg_toplevel configure is buffered until
    int32_t  pending_xdg_h_ = 0;   // the matching xdg_surface configure
};

}  // namespace vrto3d
