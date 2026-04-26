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
#include <vector>

#include <wrl/client.h>
#include <d3d11.h>
#include <d3d9.h>

#include <nvapi.h>

#include "platform/platform.h"
#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"

namespace vrto3d {

// NVIDIA 3D Vision via NVAPI + D3D9Ex.
//
// Pattern mirrors XR3DV (../XR3DV/src/nvapi_stereo.cpp): a fullscreen-exclusive
// D3D9Ex device is created on the user's chosen 3D-Vision-capable display, and
// each frame the SBS DX11 texture from the compositor is copied into a packed-
// stereo D3D9 surface (2W x (H+1)) carrying the NVSTEREOIMAGEHEADER signature
// in its extra row. The NVIDIA driver detects the signature on PresentEx and
// routes left/right halves to alternate eyes through the 3D Vision IR emitter.
class NvStereoDx9Presenter : public IOutputPresenter {
public:
    NvStereoDx9Presenter() = default;
    ~NvStereoDx9Presenter() override { Shutdown(); }

    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;
    void PresentFrame(ID3D11Texture2D* sbs_input) override;
    void Shutdown() override;

private:
    void WindowThreadLoop(Dx11Renderer* renderer,
                          platform::MonitorInfo primary);
    bool BuildD3D9Stack(HWND hwnd,
                        uint32_t monitor_w,
                        uint32_t monitor_h,
                        float    refresh_hz);
    bool EnsurePackedSurfaces(uint32_t input_w_per_eye,
                               uint32_t input_h);
    void StereoActivationRetry();
    void FocusThreadLoop();

    Dx11Renderer* renderer_ = nullptr;
    bool          eye_swap_ = false;
    bool          auto_focus_ = true;

    FocusContext  focus_{};

    std::unique_ptr<platform::PresentWindow>      window_;

    // D3D9Ex objects — owned and used only by the window thread.
    Microsoft::WRL::ComPtr<IDirect3D9Ex>           d3d9_;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex>     device9_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>      back_buffer_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>      packed_sysmem_;   // 2W x (H+1) D3DPOOL_SYSTEMMEM
    Microsoft::WRL::ComPtr<IDirect3DSurface9>      packed_default_;  // 2W x (H+1) D3DPOOL_DEFAULT
    uint32_t                                      packed_w_ = 0;     // 2 * input per-eye width
    uint32_t                                      packed_h_ = 0;     // input height
    uint32_t                                      monitor_w_ = 0;
    uint32_t                                      monitor_h_ = 0;

    // DX11 staging texture for CPU readback of the compositor's SBS output.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        staging_;
    uint32_t                                       staging_w_ = 0;
    uint32_t                                       staging_h_ = 0;
    DXGI_FORMAT                                    staging_fmt_ = DXGI_FORMAT_UNKNOWN;

    // NVAPI stereo handle (opaque pointer to NVAPI's internal state).
    StereoHandle  stereo_handle_ = nullptr;
    bool          stereo_activated_ = false;
    int           activation_retries_left_ = 0;   // counts down inside PresentFrame
    bool          activation_summary_logged_ = false;

    std::thread       window_thread_;
    std::atomic<bool> window_stop_{false};
    std::atomic<bool> window_ready_{false};
    std::atomic<bool> window_failed_{false};

    std::thread       focus_thread_;
    std::atomic<bool> focus_stop_{false};
};

}  // namespace vrto3d

#endif  // _WIN32
