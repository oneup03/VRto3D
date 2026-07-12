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
#include <string>
#include <thread>

#include <wrl/client.h>
#include <d3d11.h>

#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"

namespace NV3D { class InterfaceDX11; }

namespace vrto3d {

// True once an NV3D-Lib interface was successfully created this session.
// Read by MyDeviceProvider::Cleanup to decide whether to TerminateProcess at
// driver unload instead of letting nvd3dumx/nvapi64/d3d9 run their
// DLL_PROCESS_DETACH (see device_provider.cpp for the NV3D-Glass rationale).
bool NvStereoWasActiveThisSession();

// NVIDIA 3D Vision output via NV3D-Lib (external/NV3D-Lib, nv3d-glass branch).
//
// The library owns everything that historically lived in this file: the FSE
// click-through popup and its message-pumping window thread, the D3D9Ex
// device, NvAPI stereo bring-up + activation retries, LightBoost custom
// timings (nvtimings DB), the in-process 3D Vision suppressor (nvd3dumx OSD /
// rating-overlay / Ctrl+F-hotkey detours — MinHook is compiled into
// NV3DLib.lib), the DX11→D3D9 shared-surface import, and an async present
// worker that runs StretchRect + PresentEx off our threads.
//
// This wrapper owns:
//   - the render-loop thread that drives Dx11Renderer::WaitAndDrawPending,
//   - the focus-policy thread: ComputeWantOnTop (focus_policy.h) over the
//     FocusContext atoms → iface_->SetVisible on transitions. The library is
//     inited with tracked_game_pid = our own PID (NV3D-Glass's trick), which
//     makes popup visibility purely SetVisible-driven — the library never
//     minimizes on its own and never steals game focus, so VRto3D's focus
//     policy stays authoritative,
//   - a JOINABLE game-focus watcher (the old detached watcher could wedge the
//     OS input chain if vrserver exited mid-ForceFocus — "display frozen,
//     reboot required"),
//   - a 3-deep MISC_SHARED staging ring. The library reads our texture through
//     a legacy KMT shared handle, which has ZERO implicit cross-API sync; its
//     async worker may still be StretchRect-reading slot N while the
//     compositor overwrites out_sbs_, so each frame is copied into a rotating
//     slot instead of handing out_sbs_ over directly (the library identity-
//     caches 4 import slots, documented >= ring + 1 transient).
//
// Interface contract notes:
//   - RecordComposite runs under Dx11Renderer::context_mutex_ and does ALL
//     the library work: the lib's Present() signals its fence / issues its
//     EVENT query on the host immediate context (caller's thread), so it must
//     be serialized against the compositor — and it returns in microseconds
//     because the actual D3D9 present happens on the lib's worker.
//   - Present() is therefore a no-op: pacing comes from WaitAndDrawPending's
//     composite_cv_ wait, and the lib worker owns PresentEx/vsync.
//   - The lib's Present() HRESULT reflects the PREVIOUS frame (async worker),
//     so failure is judged by a 40-frame streak, not a single result.
//   - On host-observed device removal we call NotifyDeviceLost() BEFORE
//     Delete() so the lib takes its non-blocking teardown path (no
//     Stereo_DestroyHandle / COM Release into a wedged kernel driver).
class NvStereoDx9Presenter : public IOutputPresenter {
public:
    NvStereoDx9Presenter() = default;
    ~NvStereoDx9Presenter() override { Shutdown(); }

    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;
    void RecordComposite(ID3D11Texture2D* sbs_input) override;
    // Everything context-bound already ran in RecordComposite; the library's
    // async worker owns PresentEx pacing.
    void Present() override {}
    void Shutdown() override;
    bool IsAlive() const override {
        return !dead_.load(std::memory_order_acquire);
    }

private:
    void RenderLoop();
    void FocusThreadLoop();
    // Joinable replacement for the old detached game-focus watcher.
    void StartForceFocusWatcher(uint32_t pid);
    void StopForceFocusWatcher();
    bool EnsureRing(ID3D11Texture2D* sbs);
    // Latch dead_ + NotifyDeviceLost so Shutdown's Delete() takes the
    // non-blocking teardown path. Idempotent.
    void MarkDead(const char* why);

    Dx11Renderer*        renderer_ = nullptr;
    NV3D::InterfaceDX11* iface_    = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    FocusContext         focus_{};
    bool                 auto_focus_cache_ = true;
    bool                 last_eye_swap_    = false;
    // InitParams copies the raw nvtimings_json_path pointer — this string
    // must outlive iface_.
    std::wstring         nvtimings_path_w_;

    // Staging ring (see class comment). Lazily (re)created from the incoming
    // texture desc so it always matches out_sbs_'s pinned dims/format exactly
    // (CopyResource requires an exact match, including _SRGB-ness).
    static constexpr UINT kRing = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> ring_[kRing];
    UINT                 ring_idx_ = 0;
    UINT                 ring_w_   = 0;
    UINT                 ring_h_   = 0;
    DXGI_FORMAT          ring_fmt_ = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D*     last_tex_ = nullptr;  // identity cache for SetInputTexture

    int frames_since_dev_check_ = 0;
    int present_fail_streak_    = 0;

    std::atomic<bool> dead_{false};
    // Mirrors the last SetVisible state. Stored BEFORE calling SetVisible so
    // RecordComposite stops submitting before/while the lib drains its worker
    // (PresentEx against a minimized FSE window wedges some drivers).
    std::atomic<bool> want_visible_{true};

    std::thread       render_thread_;
    std::atomic<bool> render_stop_{false};
    std::thread       focus_thread_;
    std::atomic<bool> focus_stop_{false};
    std::thread       focus_watcher_thread_;
    std::shared_ptr<std::atomic<bool>> focus_watcher_stop_;
};

}  // namespace vrto3d
