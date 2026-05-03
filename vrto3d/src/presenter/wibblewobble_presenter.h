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

#ifdef _WIN32

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"

class Dx11Renderer;

namespace vrto3d {

// WibbleWobble lightfield bridge.
//
// Loads WibbleWobbleClient.dll in-process (path discovered via the
// HKLM\SOFTWARE\PHARTGAMES\WibbleWobbleClient\install_path registry value)
// and pushes the canonical 2W x H side-by-side texture each frame via the
// WWClient_PresentFrame C-ABI shim — no shared-memory IPC, no per-frame
// CopyResource into a destination ring buffer. The client opens our shared
// HANDLE on its own internal D3D11 device.
class WibbleWobblePresenter : public IOutputPresenter {
public:
    WibbleWobblePresenter();
    ~WibbleWobblePresenter() override;

    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;
    void PresentFrame(ID3D11Texture2D* sbs_input) override;
    void Shutdown() override;

private:
    void PumpThreadLoop();
    void FocusThreadLoop();

    // Borrowed from Dx11Renderer; valid for the lifetime of this presenter.
    Dx11Renderer*        renderer_ = nullptr;
    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // Drives Dx11Renderer::WaitAndDrawPending so OSD composite + PresentFrame
    // get called. Other presenters do this from their window/render thread;
    // we have no window so we run a bare pump.
    std::thread          pump_thread_;
    std::atomic<bool>    pump_stop_{false};

    // Tracks the WibbleWobbleClient's visible window (title "WibbleWobble"),
    // applies BringToTop focus logic per FocusContext, and feeds the HWND
    // to the OSD as headset_hwnd so cursor coords map correctly.
    std::thread          focus_thread_;
    std::atomic<bool>    focus_stop_{false};
    bool                 auto_focus_ = true;
    FocusContext         focus_{};

    // Per-frame state for WWClient_PresentFrame.
    // shared_handle_ is the HANDLE returned by IDXGIResource::GetSharedHandle
    // on the current out_sbs_ texture; refreshed when the source texture
    // pointer changes (resize, format change, presenter init).
    ID3D11Texture2D*     last_sbs_input_  = nullptr;
    void*                shared_handle_   = nullptr;
    uint64_t             frame_id_        = 0;

    // pImpl holds Windows.h-flavored state (HMODULE, function pointers,
    // PROCESS_INFORMATION). Confined to the .cpp so this header doesn't
    // drag in <windows.h>.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vrto3d

#endif  // _WIN32
