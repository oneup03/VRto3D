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
#include "presenter/wayland_presenter.h"

#include <cstring>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

// The canonical wlr-layer-shell XML names get_layer_surface's last argument
// "namespace", so wayland-scanner emits a C prototype with a parameter named
// after a C++ keyword. All headers the protocol header pulls in are already
// included above (their guards make re-inclusion a no-op), so the macro only
// rewrites the offending parameter name.
#include <cstddef>
#include <cstdint>
#define namespace namespace_  // C++ keyword used as a C parameter name
#include "wlr-layer-shell-client-protocol.h"
#undef namespace

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

namespace vrto3d {

namespace {

// ---------------------------------------------------------------------------
// C listener trampolines. `data` is the owning WaylandPresenter; per-output
// state is looked up by proxy pointer (the outputs_ vector may reallocate
// while globals are still arriving, so no direct OutputInfo* is stored as
// listener data).
// ---------------------------------------------------------------------------

WaylandPresenter::OutputInfo* FindOutput(WaylandPresenter* self, wl_output* output)
{
    for (auto& o : self->Outputs()) {
        if (o.output == output) return &o;
    }
    return nullptr;
}

void RegistryGlobal(void* data, wl_registry* registry, uint32_t name,
                    const char* interface, uint32_t version)
{
    static_cast<WaylandPresenter*>(data)->OnGlobal(registry, name, interface, version);
}

void RegistryGlobalRemove(void* data, wl_registry*, uint32_t name)
{
    static_cast<WaylandPresenter*>(data)->OnGlobalRemove(name);
}

const wl_registry_listener kRegistryListener = {
    RegistryGlobal,
    RegistryGlobalRemove,
};

void OutputGeometry(void* data, wl_output* output, int32_t x, int32_t y,
                    int32_t /*phys_w*/, int32_t /*phys_h*/, int32_t /*subpixel*/,
                    const char* make, const char* model, int32_t /*transform*/)
{
    auto* o = FindOutput(static_cast<WaylandPresenter*>(data), output);
    if (!o) return;
    o->x = x;
    o->y = y;
    o->make = make ? make : "";
    o->model = model ? model : "";
}

void OutputMode(void* data, wl_output* output, uint32_t flags,
                int32_t width, int32_t height, int32_t refresh)
{
    auto* o = FindOutput(static_cast<WaylandPresenter*>(data), output);
    if (!o) return;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->width = width;
        o->height = height;
        o->refresh_mhz = refresh;
    }
}

void OutputDone(void* data, wl_output* output)
{
    auto* o = FindOutput(static_cast<WaylandPresenter*>(data), output);
    if (o) o->done = true;
}

void OutputScale(void* /*data*/, wl_output* /*output*/, int32_t /*factor*/)
{
}

void OutputName(void* data, wl_output* output, const char* name)
{
    auto* o = FindOutput(static_cast<WaylandPresenter*>(data), output);
    if (o && name) o->name = name;
}

void OutputDescription(void* /*data*/, wl_output* /*output*/, const char* /*description*/)
{
}

const wl_output_listener kOutputListener = {
    OutputGeometry,
    OutputMode,
    OutputDone,
    OutputScale,
    OutputName,
    OutputDescription,
};

void WmBasePing(void* /*data*/, xdg_wm_base* wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

const xdg_wm_base_listener kWmBaseListener = {
    WmBasePing,
};

void LayerSurfaceConfigure(void* data, zwlr_layer_surface_v1* /*surface*/,
                           uint32_t serial, uint32_t width, uint32_t height)
{
    static_cast<WaylandPresenter*>(data)->OnLayerConfigure(serial, width, height);
}

void LayerSurfaceClosed(void* data, zwlr_layer_surface_v1* /*surface*/)
{
    static_cast<WaylandPresenter*>(data)->OnLayerClosed();
}

const zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
    LayerSurfaceConfigure,
    LayerSurfaceClosed,
};

void XdgSurfaceConfigure(void* data, xdg_surface* /*surface*/, uint32_t serial)
{
    static_cast<WaylandPresenter*>(data)->OnXdgSurfaceConfigure(serial);
}

const xdg_surface_listener kXdgSurfaceListener = {
    XdgSurfaceConfigure,
};

void XdgToplevelConfigure(void* data, xdg_toplevel* /*toplevel*/,
                          int32_t width, int32_t height, wl_array* /*states*/)
{
    static_cast<WaylandPresenter*>(data)->OnXdgToplevelConfigure(width, height);
}

void XdgToplevelClose(void* data, xdg_toplevel* /*toplevel*/)
{
    static_cast<WaylandPresenter*>(data)->OnXdgToplevelClose();
}

void XdgToplevelConfigureBounds(void* /*data*/, xdg_toplevel* /*toplevel*/,
                                int32_t /*width*/, int32_t /*height*/)
{
}

void XdgToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/,
                               wl_array* /*capabilities*/)
{
}

const xdg_toplevel_listener kXdgToplevelListener = {
    XdgToplevelConfigure,
    XdgToplevelClose,
    XdgToplevelConfigureBounds,
    XdgToplevelWmCapabilities,
};

}  // namespace


WaylandPresenter::~WaylandPresenter()
{
    Shutdown();
}


void WaylandPresenter::OnGlobal(wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t version)
{
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, v));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        uint32_t v = version < 2 ? version : 2;
        wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, v));
        xdg_wm_base_add_listener(wm_base_, &kWmBaseListener, this);
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        layer_shell_ = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, v));
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        OutputInfo info;
        info.global_name = name;
        info.output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, v));
        outputs_.push_back(info);
        wl_output_add_listener(outputs_.back().output, &kOutputListener, this);
    }
}


void WaylandPresenter::OnGlobalRemove(uint32_t name)
{
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if (it->global_name == name) {
            wl_output_destroy(it->output);
            outputs_.erase(it);
            return;
        }
    }
}


void WaylandPresenter::OnLayerConfigure(uint32_t serial, uint32_t width, uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(layer_surface_, serial);
    if (width != 0 && height != 0) {
        if (configured_ && (width != surf_width_ || height != surf_height_)) {
            swapchain_.SetDesiredExtent(width, height);
        }
        surf_width_ = width;
        surf_height_ = height;
    }
    configured_ = true;
}


void WaylandPresenter::OnLayerClosed()
{
    closed_ = true;
}


void WaylandPresenter::OnXdgToplevelConfigure(int32_t width, int32_t height)
{
    // Buffered until the matching xdg_surface.configure arrives.
    pending_xdg_w_ = width;
    pending_xdg_h_ = height;
}


void WaylandPresenter::OnXdgSurfaceConfigure(uint32_t serial)
{
    xdg_surface_ack_configure(xdg_surface_, serial);
    if (pending_xdg_w_ > 0 && pending_xdg_h_ > 0) {
        const uint32_t w = static_cast<uint32_t>(pending_xdg_w_);
        const uint32_t h = static_cast<uint32_t>(pending_xdg_h_);
        if (configured_ && (w != surf_width_ || h != surf_height_)) {
            swapchain_.SetDesiredExtent(w, h);
        }
        surf_width_ = w;
        surf_height_ = h;
    }
    configured_ = true;
}


void WaylandPresenter::OnXdgToplevelClose()
{
    closed_ = true;
}


bool WaylandPresenter::ConnectAndBind()
{
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        PresenterLog("WaylandPresenter: wl_display_connect failed (WAYLAND_DISPLAY=%s)",
                     getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "<unset>");
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);

    // First roundtrip: registry globals arrive and outputs get bound.
    // Second roundtrip: the bound outputs' geometry/mode/name/done events.
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);

    if (!compositor_) {
        PresenterLog("WaylandPresenter: compositor lacks wl_compositor (?)");
        return false;
    }
    if (!wm_base_ && !layer_shell_) {
        PresenterLog("WaylandPresenter: neither zwlr_layer_shell_v1 nor xdg_wm_base available");
        return false;
    }
    if (outputs_.empty()) {
        PresenterLog("WaylandPresenter: no wl_output globals");
        return false;
    }
    return true;
}


bool WaylandPresenter::CreateLayerSurface(wl_output* output)
{
    layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell_, surface_, output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "vrto3d");
    if (!layer_surface_) return false;
    zwlr_layer_surface_v1_add_listener(layer_surface_, &kLayerSurfaceListener, this);

    // Anchor to all four edges with no explicit size — the compositor sizes
    // the surface to the full output. exclusive_zone -1 = extend over panels.
    zwlr_layer_surface_v1_set_anchor(layer_surface_,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                                     | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                                     | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                                     | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer_surface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    SetSurfaceInputRegion(false);  // click-through by default (menu closed)
    wl_surface_commit(surface_);
    return WaitForConfigure();
}

void WaylandPresenter::SetSurfaceInputRegion(bool capture)
{
    if (!surface_) return;
    if (capture) {
        // null region = whole surface receives pointer input (game shielded).
        wl_surface_set_input_region(surface_, nullptr);
    } else {
        // empty region = input-transparent; pointer falls through to the game.
        wl_region* region = wl_compositor_create_region(compositor_);
        wl_surface_set_input_region(surface_, region);
        wl_region_destroy(region);
    }
}

void WaylandPresenter::SetAlwaysOnTop(bool on_top)
{
    // Layer-shell (v2+): drop to the BACKGROUND layer to reveal the flat game
    // (a normal xdg_toplevel sits above background), OVERLAY to restore. Surface
    // stays mapped/presentable — we keep rendering behind it.
    if (layer_surface_) {
        zwlr_layer_surface_v1_set_layer(
            layer_surface_,
            on_top ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);
        wl_surface_commit(surface_);
    }
    // xdg_toplevel fallback (GNOME): z-order is compositor policy; the peek
    // toggle is a no-op there (documented).
}

void WaylandPresenter::SetInputCapture(bool capture)
{
    SetSurfaceInputRegion(capture);
    if (layer_surface_) {
        // Take keyboard while the OSD is open so text entry doesn't leak into
        // the game; release (NONE) otherwise so the game keeps keys.
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_surface_,
            capture ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                    : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    }
    if (surface_)
        wl_surface_commit(surface_);
}


bool WaylandPresenter::CreateXdgFullscreenSurface(wl_output* output)
{
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    if (!xdg_surface_) return false;
    xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);

    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    if (!xdg_toplevel_) return false;
    xdg_toplevel_add_listener(xdg_toplevel_, &kXdgToplevelListener, this);
    xdg_toplevel_set_title(xdg_toplevel_, "VRto3D");
    xdg_toplevel_set_app_id(xdg_toplevel_, "vrto3d");
    xdg_toplevel_set_fullscreen(xdg_toplevel_, output);

    wl_surface_commit(surface_);
    return WaitForConfigure();
}


bool WaylandPresenter::WaitForConfigure()
{
    // Blocking dispatch is allowed here — Init() runs before the Vulkan WSI
    // has its own event queue on this display.
    for (int i = 0; i < 100 && !configured_ && !closed_; ++i) {
        if (wl_display_roundtrip(display_) < 0) {
            PresenterLog("WaylandPresenter: display error while waiting for configure");
            return false;
        }
    }
    if (!configured_ || closed_) {
        PresenterLog("WaylandPresenter: surface never configured (closed=%d)", closed_ ? 1 : 0);
        return false;
    }
    return true;
}


bool WaylandPresenter::Init(vrto3d::vk::DeviceCtx* ctx, const StereoDisplayDriverConfiguration& cfg)
{
    ctx_ = ctx;

    if (!ConnectAndBind()) {
        Shutdown();
        return false;
    }

    // Pick output by display_index: 1-based into the bound order; 0 = first.
    size_t pick = 0;
    if (cfg.display_index > 0 && static_cast<size_t>(cfg.display_index) <= outputs_.size()) {
        pick = static_cast<size_t>(cfg.display_index - 1);
    }
    const OutputInfo& out = outputs_[pick];
    PresenterLog("WaylandPresenter: output %zu/%zu '%s' %dx%d@%.2fHz (display_index=%d)",
                 pick + 1, outputs_.size(),
                 !out.name.empty() ? out.name.c_str() : out.model.c_str(),
                 out.width, out.height, out.refresh_mhz / 1000.0,
                 cfg.display_index);

    if (cfg.output_mode == OutputMode::DualDisplay
        || cfg.output_mode == OutputMode::DualDisplayFlip) {
        PresenterLog("WaylandPresenter: DualDisplay spanning is not possible on Wayland "
                     "(one surface per output) — rendering SbS on the chosen output only");
    }

    surface_ = wl_compositor_create_surface(compositor_);
    if (!surface_) {
        PresenterLog("WaylandPresenter: wl_compositor_create_surface failed");
        Shutdown();
        return false;
    }

    // Preferred: layer-shell overlay on the chosen output. Fallback: xdg
    // toplevel fullscreened onto it.
    bool surface_ok = false;
    if (layer_shell_) {
        surface_ok = CreateLayerSurface(out.output);
        if (!surface_ok) {
            PresenterLog("WaylandPresenter: layer-shell surface failed, trying xdg fallback");
        }
    }
    if (!surface_ok && wm_base_) {
        // A wl_surface whose layer-surface role failed cannot be reused.
        if (layer_surface_) {
            zwlr_layer_surface_v1_destroy(layer_surface_);
            layer_surface_ = nullptr;
            wl_surface_destroy(surface_);
            surface_ = wl_compositor_create_surface(compositor_);
            configured_ = false;
        }
        surface_ok = surface_ && CreateXdgFullscreenSurface(out.output);
    }
    if (!surface_ok) {
        Shutdown();
        return false;
    }

    // 0 from configure means "client picks" — use the output's mode size.
    if (surf_width_ == 0 || surf_height_ == 0) {
        surf_width_  = out.width  > 0 ? static_cast<uint32_t>(out.width)  : 1920u;
        surf_height_ = out.height > 0 ? static_cast<uint32_t>(out.height) : 1080u;
    }

    VkWaylandSurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
    sci.display = display_;
    sci.surface = surface_;
    VkResult r = vkCreateWaylandSurfaceKHR(ctx_->instance, &sci, nullptr, &vk_surface_);
    if (r != VK_SUCCESS) {
        PresenterLog("WaylandPresenter: vkCreateWaylandSurfaceKHR failed (r=%d)",
                     static_cast<int>(r));
        Shutdown();
        return false;
    }

    if (!swapchain_.Create(ctx_, vk_surface_, surf_width_, surf_height_)) {
        Shutdown();
        return false;
    }

    wl_display_flush(display_);
    PresenterLog("WaylandPresenter: up via %s, %ux%u",
                 layer_surface_ ? "zwlr_layer_shell_v1 (overlay)" : "xdg_toplevel fullscreen",
                 swapchain_.extent.width, swapchain_.extent.height);
    return true;
}


void WaylandPresenter::DestroyNative()
{
    if (layer_surface_) {
        zwlr_layer_surface_v1_destroy(layer_surface_);
        layer_surface_ = nullptr;
    }
    if (xdg_toplevel_) {
        xdg_toplevel_destroy(xdg_toplevel_);
        xdg_toplevel_ = nullptr;
    }
    if (xdg_surface_) {
        xdg_surface_destroy(xdg_surface_);
        xdg_surface_ = nullptr;
    }
    if (surface_) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }
    for (auto& o : outputs_) {
        if (o.output) wl_output_destroy(o.output);
    }
    outputs_.clear();
    if (layer_shell_) {
        zwlr_layer_shell_v1_destroy(layer_shell_);
        layer_shell_ = nullptr;
    }
    if (wm_base_) {
        xdg_wm_base_destroy(wm_base_);
        wm_base_ = nullptr;
    }
    if (compositor_) {
        wl_compositor_destroy(compositor_);
        compositor_ = nullptr;
    }
    if (registry_) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_) {
        wl_display_flush(display_);
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}


void WaylandPresenter::Shutdown()
{
    if (swapchain_.ctx) {
        swapchain_.Destroy();
    }
    if (vk_surface_ != VK_NULL_HANDLE && ctx_ && ctx_->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx_->instance, vk_surface_, nullptr);
    }
    vk_surface_ = VK_NULL_HANDLE;
    DestroyNative();
    configured_ = false;
    closed_ = false;
    ctx_ = nullptr;
}


bool WaylandPresenter::PumpEvents()
{
    if (!display_ || closed_) return false;

    // Non-blocking only: the Vulkan WSI owns its own wl_event_queue and reads
    // the socket during FIFO presents, which also queues our default-queue
    // events — dispatch whatever is pending and flush our outgoing requests.
    wl_display_dispatch_pending(display_);
    wl_display_flush(display_);

    if (wl_display_get_error(display_) != 0) {
        PresenterLog("WaylandPresenter: display error %d — stopping",
                     wl_display_get_error(display_));
        return false;
    }
    return !closed_;
}


bool WaylandPresenter::AcquireNext(FrameTarget* out, VkSemaphore signal_sem)
{
    return swapchain_.AcquireNext(out, signal_sem);
}


bool WaylandPresenter::Present(uint32_t image_index, VkSemaphore wait_sem)
{
    return swapchain_.Present(image_index, wait_sem);
}

}  // namespace vrto3d
