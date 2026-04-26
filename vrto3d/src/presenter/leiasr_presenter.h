/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#ifdef _WIN32

#include <atomic>
#include <memory>
#include <thread>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "platform/platform.h"
#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"

// Forward decls — full headers only included in the .cpp to keep the SDK
// dependency contained.
namespace SR { class SRContext; class IDX11Weaver1; }

namespace vrto3d {

class LeiaSrPresenter : public IOutputPresenter {
public:
    LeiaSrPresenter() = default;
    ~LeiaSrPresenter() override { Shutdown(); }

    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;
    void PresentFrame(ID3D11Texture2D* sbs_input) override;
    void Shutdown() override;

private:
    bool CreateSwapChain(Dx11Renderer& renderer);
    void WindowThreadLoop(Dx11Renderer* renderer,
                          platform::MonitorInfo primary,
                          platform::MonitorInfo secondary);
    void FocusThreadLoop();

    Dx11Renderer* renderer_ = nullptr;
    bool          eye_swap_ = false;
    bool          auto_focus_ = true;

    FocusContext                                  focus_{};

    std::unique_ptr<platform::PresentWindow>      window_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>       swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> swapchain_rtv_;
    uint32_t                                      swap_width_  = 0;
    uint32_t                                      swap_height_ = 0;

    SR::SRContext*       sr_context_ = nullptr;
    SR::IDX11Weaver1*    sr_weaver_  = nullptr;
    bool                 sr_initialized_ = false;

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
};

}  // namespace vrto3d

#endif  // _WIN32
