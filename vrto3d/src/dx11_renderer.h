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
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "openvr_driver.h"
#include "platform.h"
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

    // Per-layer eye-texture pair, passed from DirectModeComponent::Present.
    // The textures are owned by the DirectModeComponent's swap-texture pool;
    // the ComPtr here keeps them alive until the window thread is done with
    // the frame.
    struct DirectModeLayerPair {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> left;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> right;
    };
    static constexpr int kMaxLayers = 8;

    // Called from IVRDriverDirectModeComponent::Present (compositor thread).
    // Composes the per-layer eye textures into out_sbs_ synchronously on the
    // compositor thread, gated by an AcquireSync/ReleaseSync pair around the
    // syncTexture's keyed mutex (matches ALVR's pattern). Doing the read+
    // composite here while the syncTexture mutex is held guarantees we never
    // sample mid-render from UEVR's eye textures, which is the root cause of
    // the "frame jumping back" stutter we saw with the deferred-window-thread
    // design. The immediate context is shared with the window thread via
    // context_mutex_, so only one side at a time issues D3D11 commands.
    // Layer 0 is the base scene (CopySubresourceRegion); layers 1+ are
    // alpha-blended via the composite shader pipeline.
    void OnDirectModeFrame(const DirectModeLayerPair* layers,
                           int layer_count,
                           vr::SharedTextureHandle_t sync_handle);

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

    // Update the headset_hwnd used by the OSD for cursor coord mapping.
    // Used by presenters whose display surface is owned by an external
    // process (WibbleWobble) and only becomes available some time after
    // ConfigureOsd was called.
    void SetOsdHeadsetHwnd(void* hwnd);

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

    // Frame counter + last-vsync timestamp (QPC seconds), updated each time
    // the window thread completes a present.
    uint64_t FrameCounter()    const { return frame_counter_.load(std::memory_order_relaxed); }
    double   LastVsyncQpcSec() const { return last_vsync_qpc_sec_.load(std::memory_order_relaxed); }

private:
    void EnsureOutputTexture(const D3D11_TEXTURE2D_DESC& incoming);
    void VsyncTickThread();

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

    // Last-seen per-eye dimensions, for one-line-per-change diagnostic.
    UINT                   last_left_w_         {0};
    UINT                   last_left_h_         {0};
    UINT                   last_right_w_        {0};
    UINT                   last_right_h_        {0};
    DXGI_FORMAT            last_eye_format_     {DXGI_FORMAT_UNKNOWN};

    // Dedicated VsyncEvent timer thread. Drives the compositor's submit
    // cadence at a stable per-eye-frequency tick (matches HMD properties
    // wired up in MockControllerDeviceDriver::Activate — half the panel
    // rate for frame-sequential output modes like WibbleWobble/NvStereoDX9).
    std::thread            vsync_thread_;
    std::atomic<bool>      vsync_stop_{false};
    std::chrono::nanoseconds frame_interval_ns_{8'333'333};  // refined from cfg in Init

    // Compositor → window thread handoff. OnDirectModeFrame sets ready=true
    // after composite + Flush so the window thread wakes immediately and
    // presents the freshly-composed out_sbs_. Matches the smooth pacing of
    // the old IVRVirtualDisplay path — the window thread runs at the
    // compositor's submit rate rather than a phase-offset internal timer,
    // which minimizes the latency variance between compositor frame arrival
    // and display update.
    std::condition_variable composite_cv_;
    std::mutex              composite_ready_mutex_;
    bool                    composite_ready_ = false;

    // Serializes immediate-context access between the compositor thread
    // (OnDirectModeFrame: layer copies + composite + Flush) and the window
    // thread (WaitAndDrawPending: OSD render + presenter PresentFrame).
    // Held briefly on each side — ~1ms typical — so the compositor never
    // waits significantly on us, and vice versa.
    std::mutex context_mutex_;

    // Compositor -> window-thread frame handoff. Only the latest pending
    // layer batch is kept; if the window thread can't keep up, older frames
    // are dropped (matches the compositor's expected pacing). The per-layer
    // ComPtrs keep the swap textures alive even if DestroySwapTextureSet
    // fires between the stash and the drain.
    std::mutex                                pending_mutex_;
    std::condition_variable                   pending_cv_;
    DirectModeLayerPair                       pending_layers_[kMaxLayers]{};
    int                                       pending_layer_count_ = 0;
    vr::SharedTextureHandle_t                 pending_sync_handle_ = 0;
    bool                                      pending_ready_       = false;

    // Cache of shared textures opened from the compositor, keyed by handle.
    // The compositor reuses sync texture handles across frames; reopening a
    // fresh COM wrapper every frame loses DXGI's internal keyed-mutex state.
    std::unordered_map<vr::SharedTextureHandle_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>>
                              shared_texture_cache_;

    bool                   initialized_         = false;

    // Composite pipeline state — used to alpha-blend overlay layers (layer
    // 1+) onto out_sbs_. Layer 0 is still a straight CopySubresourceRegion.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>     composite_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>      composite_ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>     composite_sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>       composite_blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  composite_raster_;
    // Cached RTV on out_sbs_, recreated whenever out_sbs_ is recreated.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> out_sbs_rtv_;
    bool                   composite_pipeline_ready_ = false;

    // Per-source-texture SRV cache for the composite blit. UEVR and the
    // compositor reuse a small set of swap textures across frames, so we
    // hash by ID3D11Texture2D* and keep the SRV alive across the session.
    // Cleared whenever out_sbs_ is (re)created (covers game-start/-exit
    // boundaries when the underlying textures get destroyed).
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>
                           composite_srv_cache_;

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

    // Background-encode result handoff: the encode thread writes the toast
    // text here, the window thread picks it up on its next tick and calls
    // OsdRenderer::SetText. Separated from screenshot_mutex_ since the
    // encode thread runs much later than the capture request.
    std::mutex              screenshot_result_mutex_;
    std::string             pending_screenshot_toast_;

    // Deferred staging readback. Synchronous Map(MAP_READ) blocks the
    // immediate context for as long as it takes the GPU to flush the
    // CopyResource — at 120Hz that's enough delay for SteamVR's compositor
    // to partially desync us (we'd briefly drop a layer when it caught back
    // up). Instead we queue the CopyResource on the screenshot frame and
    // poll Map(DO_NOT_WAIT) on subsequent frames until it succeeds.
    struct PendingShot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        std::string app_name;
        uint32_t    width  = 0;
        uint32_t    height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        float       target_eye_aspect = 0.0f;
        // Number of WaitAndDrawPending iterations since the CopyResource was
        // queued — used both for an early-frame guard (give the GPU at least
        // one frame to finish) and a timeout (give up if the staging is
        // never readable, so the slot doesn't stay stuck).
        int         age_frames = 0;
    };
    PendingShot pending_shot_;
    bool        has_pending_shot_ = false;

    void CaptureScreenshot(const std::string& app_name);
    void DrainPendingShot();

    // Auto-depth disparity analyzer. Owns the compute pipeline + readback
    // ring; lazy-initialized on first frame where auto-depth is enabled.
    std::unique_ptr<vrto3d::AutoDepthAnalyzer> auto_depth_;
};
