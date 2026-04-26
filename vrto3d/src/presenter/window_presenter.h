/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "platform/platform.h"
#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"


namespace vrto3d {

class WindowPresenter : public IOutputPresenter {
public:
    WindowPresenter() = default;
    ~WindowPresenter() override { Shutdown(); }

    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;

    void PresentFrame(ID3D11Texture2D* sbs_input) override;

    void Shutdown() override;

private:
    struct CBParams {
        uint32_t mode;
        uint32_t framepack_offset;
        uint32_t eye_swap;
        uint32_t vd_fsbs_hack;
        float    out_width;
        float    out_height;
        float    aspect_ratio;
        float    _pad;
    };

    bool CreateShaders();
    bool CreateSwapChain(Dx11Renderer& renderer);
    void FocusThreadLoop();
    void WindowThreadLoop(Dx11Renderer* renderer,
                          platform::MonitorInfo primary,
                          platform::MonitorInfo secondary,
                          uint32_t override_h);

    Dx11Renderer* renderer_ = nullptr;
    OutputMode    mode_     = OutputMode::SbS;
    bool          eye_swap_ = false;
    bool          vd_fsbs_hack_ = false;
    uint32_t      framepack_offset_ = 0;
    float         aspect_ratio_ = 1.7777f;
    bool          spans_two_monitors_ = false;   // true for DualDisplay / DualDisplayFlip
    bool          auto_focus_ = true;

    FocusContext                                  focus_{};
    std::unique_ptr<platform::PresentWindow>      window_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>       swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> swapchain_rtv_;
    uint32_t                                      swap_width_  = 0;
    uint32_t                                      swap_height_ = 0;

    Microsoft::WRL::ComPtr<ID3D11VertexShader>    vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>     ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>    sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>          cb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>      blend_;

    std::thread                                   focus_thread_;
    std::atomic<bool>                             focus_stop_{false};

    std::thread                                   window_thread_;
    std::atomic<bool>                             window_stop_{false};
    std::atomic<bool>                             window_ready_{false};
    std::atomic<bool>                             window_failed_{false};
};

}  // namespace vrto3d
