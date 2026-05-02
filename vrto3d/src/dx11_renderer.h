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

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "openvr_driver.h"
#include "platform/platform.h"
#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"

class StereoDisplayComponent;
namespace vrto3d {
class AutoDepthAnalyzer;
namespace osd {
class OsdRenderer;
struct MenuCallbacks;
} // namespace osd
} // namespace vrto3d


// Owns the DX11 device used to import the SteamVR-composited texture, maintains
// a persistent copy (out_sbs_), and forwards it to the selected presenter.
class Dx11Renderer {
public:
    Dx11Renderer();
    ~Dx11Renderer();

    // Picks the adapter matching `adapter_luid`, creates the D3D11 device, and
    // constructs+initializes the presenter selected by cfg.output_mode.
    bool Init(LUID adapter_luid,
              const StereoDisplayDriverConfiguration& cfg,
              const vrto3d::FocusContext& focus);

    // Called from IVRVirtualDisplay::Present (compositor thread). Stashes the
    // incoming handle and signals the render thread; performs no context work.
    void OnPresent(const vr::PresentInfo_t& info);

    // Called from the presenter's window thread. Blocks up to timeout_ms for a
    // pending frame, then opens the shared texture, copies to out_sbs_, and
    // invokes presenter->PresentFrame. Returns true if a frame was drained.
    // All immediate-context work happens here so the window thread (which owns
    // the swapchain via CreateSwapChainForHwnd) is the one issuing Present —
    // DXGI/DWM drops cross-thread Presents silently with DISCARD swap effect.
    bool WaitAndDrawPending(int timeout_ms);

    void Shutdown();

    // Stash OSD configuration. The OsdRenderer is lazy-initialized on the
    // window thread the first time WaitAndDrawPending sees a frame (so that
    // per-eye dimensions are known). May be called before or after Init.
    void ConfigureOsd(StereoDisplayComponent* component,
                      vrto3d::osd::MenuCallbacks callbacks,
                      void* headset_hwnd);

    // Request that the next composited stereo frame be saved to disk as two
    // PNG images under <Steam>\steamapps\common\SteamVR\screenshots:
    // one as-is (parallel-view) and one with eyes swapped (cross-view). The
    // capture happens on the window thread inside WaitAndDrawPending, after
    // the SBS copy completes but before the OSD is composited in.
    void RequestScreenshot(std::string app_name);

    // Returns the OSD renderer pointer (may be null until first frame).
    // Used by hmd_device_driver to push toast text and toggle the menu.
    vrto3d::osd::OsdRenderer* Osd() { return osd_renderer_.get(); }

    // Live access to the active StereoDisplayComponent. Used by presenters
    // to poll mid-session-tunable fields (e.g. eye_swap) each frame instead
    // of caching the value at Init.
    StereoDisplayComponent* Component() { return osd_component_; }

    // Live access to the active presenter — used by OSD callbacks that need
    // to poke presenter-specific behavior (e.g. LeiaSR head-pose calibrate).
    vrto3d::IOutputPresenter* Presenter() { return presenter_.get(); }

    // Accessors for the presenter.
    ID3D11Device*           Device()   const { return device_.Get();  }
    ID3D11DeviceContext*    Context()  const { return context_.Get(); }
    ID3D11Texture2D*        CurrentSbsTexture() const { return out_sbs_.Get(); }
    uint32_t                SbsWidth()  const { return sbs_width_;  }
    uint32_t                SbsHeight() const { return sbs_height_; }

    // Telemetry consumed by IVRVirtualDisplay::GetTimeSinceLastVsync.
    uint64_t FrameCounter()    const { return frame_counter_.load(std::memory_order_relaxed); }
    double   LastVsyncQpcSec() const { return last_vsync_qpc_sec_.load(std::memory_order_relaxed); }

private:
    void EnsureOutputTexture(const D3D11_TEXTURE2D_DESC& incoming);

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>       adapter_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>     out_sbs_;
    uint32_t                                    sbs_width_  = 0;
    uint32_t                                    sbs_height_ = 0;
    DXGI_FORMAT                                 sbs_format_ = DXGI_FORMAT_UNKNOWN;

    std::unique_ptr<vrto3d::IOutputPresenter>   presenter_;

    std::atomic<uint64_t>  frame_counter_       {0};
    std::atomic<double>    last_vsync_qpc_sec_  {0.0};
    double                 qpc_freq_sec_        {0.0};

    // Compositor -> window-thread frame handoff. Only the latest pending
    // handle is kept; if the window thread can't keep up, older frames are
    // dropped (matches the compositor's expected pacing).
    std::mutex                pending_mutex_;
    std::condition_variable   pending_cv_;
    vr::SharedTextureHandle_t pending_handle_      = 0;
    bool                      pending_ready_       = false;

    // Cache of shared textures opened from the compositor, keyed by handle.
    // The compositor cycles a small pool (typically 3 swap textures); reopening
    // a fresh COM wrapper every frame loses DXGI's internal keyed-mutex state.
    std::unordered_map<vr::SharedTextureHandle_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>>
                              shared_texture_cache_;

    bool                   initialized_         = false;

    // OSD overlay (lazy-initialized on first frame so eye dims are known).
    std::unique_ptr<vrto3d::osd::OsdRenderer> osd_renderer_;
    StereoDisplayComponent* osd_component_     = nullptr;
    void*                   osd_headset_hwnd_  = nullptr;
    std::unique_ptr<vrto3d::osd::MenuCallbacks> osd_pending_callbacks_;
    bool                    osd_initialized_   = false;

    // Pending screenshot request — drained by WaitAndDrawPending.
    std::mutex              screenshot_mutex_;
    std::string             pending_screenshot_app_;
    bool                    screenshot_pending_ = false;

    void CaptureScreenshot(const std::string& app_name);

    // Auto-depth disparity analyzer. Owns the compute pipeline + readback
    // ring; lazy-initialized on first frame where auto-depth is enabled.
    std::unique_ptr<vrto3d::AutoDepthAnalyzer> auto_depth_;
};
