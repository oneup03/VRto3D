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
#include <memory>
#include <thread>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>

#include "platform.h"
#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"

// Forward decls — full headers only included in the .cpp to keep the SDK
// dependency contained.
namespace SR { class SRContext; class IDX11Weaver1; class SwitchableLensHint; class HeadPoseTracker; }

namespace vrto3d {

// Internal head-pose listener + UDP sender (defined in .cpp). Forward-declared
// here so the impl can stash unique_ptrs without leaking SR/winsock headers.
class LeiaSrHeadPoseListener;
class LeiaSrOpenTrackSender;
class LeiaSrTrackPipeline;

class LeiaSrPresenter : public IOutputPresenter {
public:
    LeiaSrPresenter();
    ~LeiaSrPresenter() override;

    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;
    void RecordComposite(ID3D11Texture2D* sbs_input) override;
    void Present() override;
    void Shutdown() override;
    void RequestCalibrate() override;

private:
    bool CreateSwapChain(Dx11Renderer& renderer);
    void WindowThreadLoop(Dx11Renderer* renderer,
                          platform::MonitorInfo primary,
                          platform::MonitorInfo secondary);
    void FocusThreadLoop();
    void HeadTrackingThreadLoop();

    Dx11Renderer* renderer_ = nullptr;
    bool          eye_swap_ = false;
    bool          auto_focus_ = true;

    FocusContext                                  focus_{};

    std::unique_ptr<platform::PresentWindow>      window_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>       swapchain_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2>       swapchain2_;
    // FLIP_DISCARD rotates buffer 0 each present, so we recreate the RTV per
    // frame instead of caching one at swap-chain creation.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> swapchain_rtv_;
    // DWM-signaled handle that releases once per display refresh — gates the
    // window thread's pacing without blocking inside Present.
    HANDLE                                        frame_latency_wait_ = nullptr;
    uint32_t                                      swap_width_  = 0;
    uint32_t                                      swap_height_ = 0;

    SR::SRContext*          sr_context_  = nullptr;
    SR::IDX11Weaver1*       sr_weaver_   = nullptr;
    SR::SwitchableLensHint* lens_hint_   = nullptr;
    bool                    sr_initialized_ = false;

    // Cache last input texture dims so we don't call setInputViewTexture
    // every frame when nothing changed.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input_srv_;
    ID3D11Texture2D*  cached_input_ptr_ = nullptr;
    uint32_t          cached_input_w_   = 0;
    uint32_t          cached_input_h_   = 0;
    DXGI_FORMAT       cached_input_fmt_ = DXGI_FORMAT_UNKNOWN;

    std::thread       window_thread_;
    std::atomic<bool> window_stop_{false};
    std::atomic<bool> window_ready_{false};
    std::atomic<bool> window_failed_{false};

    std::thread       focus_thread_;
    std::atomic<bool> focus_stop_{false};

    // Head tracking — only spawned when use_open_track && output_mode==LeiaSR.
    // Sends OpenTrack UDP packets to 127.0.0.1:open_track_port, received by
    // MockControllerDeviceDriver::OpenTrackThread. Decouples SR types from the
    // consumer side: this presenter is the only translation unit that touches
    // SR head-tracking APIs.
    bool                                          tracking_enabled_ = false;
    int32_t                                       tracking_port_    = 4242;
    StereoDisplayDriverConfiguration              tracking_cfg_{};
    SR::HeadPoseTracker*                          sr_head_tracker_  = nullptr;
    std::unique_ptr<LeiaSrHeadPoseListener>       head_listener_;
    std::unique_ptr<LeiaSrOpenTrackSender>        ot_sender_;
    std::unique_ptr<LeiaSrTrackPipeline>          track_pipeline_;
    std::thread                                   tracking_thread_;
    std::atomic<bool>                             tracking_stop_{false};
    std::atomic<bool>                             calibrate_request_{false};
};

}  // namespace vrto3d

