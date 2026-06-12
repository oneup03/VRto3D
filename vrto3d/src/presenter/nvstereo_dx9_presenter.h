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
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <wrl/client.h>
#include <d3d11.h>
#include <d3d9.h>

#include <nvapi.h>

#include "platform.h"
#include "presenter/output_presenter.h"
#include "vrto3dlib/stereo_config.h"

namespace vrto3d {

// One entry in the nvtimings.json database, keyed by "VENDOR_PRODUCT_REFRESH".
struct NvTimingsEntry {
    std::string    monitor_EDID;
    NV_TIMING      timing{};       // NVAPI monitor timing
    float          refresh_hz{};   // convenience (from JSON)
    NvU16          refresh_int{};
};

// Loads and queries the nvtimings.json file.
class NvTimingsDb {
public:
    static NvTimingsDb Load(const std::string& jsonPath);

    std::optional<NvTimingsEntry> findExact(const std::string& key) const;
    std::optional<NvTimingsEntry> findByBaseAndRefresh(const std::string& baseKey, int refreshNearestInt) const;
    std::optional<NvTimingsEntry> findHighestRefreshForBase(const std::string& baseKey) const;

    static std::string to_utf8(const std::wstring& ws);

private:
    std::unordered_map<std::string, NvTimingsEntry> data_;
};

// NVIDIA 3D Vision via NVAPI + D3D9Ex (DIRECT driver mode).
//
// D3D9Ex device is created on the user's chosen 3D-Vision-capable display.
// Dx11Renderer pins out_sbs_ to panel-derived dims (panel_w*2 × panel_h,
// BGRA) when output_mode is NvidiaDX9, so its shared handle is stable
// across landscape↔game transitions. We import that handle once into
// D3D9Ex via IDXGIResource::GetSharedHandle and reuse it for the entire
// session.
//
// Per frame:
//   1. Fence: ID3D11Query EVENT to wait for DX11 writes to complete.
//   2. NvAPI_Stereo_SetActiveEye(LEFT) + StretchRect(shared, left half →
//      back_buffer, POINT). Driver routes write to LEFT eye plane.
//   3. SetActiveEye(RIGHT) + StretchRect(shared, right half → back_buffer).
//   4. PresentEx — driver alternates eye planes via the IR emitter.
//
// DIRECT mode (not AUTOMATIC) is required because NV3D's AUTOMATIC-mode
// signature scanning got the LEFT eye plane stuck on the prior frame's
// content when the source resource changed, even with a stable signed RT
// in the chain (proven with a ColorFill diagnostic). DIRECT mode bypasses
// the auto-detection: SetActiveEye is authoritative.
class NvStereoDx9Presenter : public IOutputPresenter {
public:
    NvStereoDx9Presenter() = default;
    ~NvStereoDx9Presenter() override { Shutdown(); }

    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;
    void RecordComposite(ID3D11Texture2D* sbs_input) override;
    void Present() override;
    void Shutdown() override;
    bool IsAlive() const override {
        return !d3d9_dead_.load(std::memory_order_acquire);
    }

private:
    void WindowThreadLoop(Dx11Renderer* renderer,
                          platform::MonitorInfo primary);
    bool BuildD3D9Stack(HWND hwnd,
                        uint32_t monitor_w,
                        uint32_t monitor_h,
                        float    refresh_hz);

    // Open the DX11 SbS composite texture as a D3D9 IDirect3DTexture9 via the
    // legacy DXGI shared-handle path. Cached by source pointer + dims + fmt;
    // because Dx11Renderer pins out_sbs_ to fixed dims for NvidiaDX9 mode,
    // this is a cache-hit after the first frame.
    bool EnsureSharedSurface(ID3D11Texture2D* sbs);

    void StereoActivationRetry();
    void FocusThreadLoop();
    void InstallFseSubclass(HWND hwnd);
    void RemoveFseSubclass();

    // LightBoost: check current resolution/timings against the nvtimings.json
    // database and apply a LightBoost custom resolution if the monitor's
    // current timing doesn't match a known entry.
    // All three are non-fatal — failures are logged but never abort init.
    void CheckAndApplyLightBoost(const std::string& target_gdi_device);
    void EnableLightBoost();
    void DisableLightBoost();

    // Resolve a Windows GDI device name (e.g. "\\.\\DISPLAY3") to the
    // matching NVAPI displayId. Returns 0 on failure.
    static NvU32 ResolveNvDisplayId(const std::string& gdi_device_name);

    // Poll NvAPI_DISP_GetTiming on `displayId` until the live timing matches
    // `expected.HTotal/VTotal/pclk` (within 1-unit tolerance) or `timeout_ms`
    // elapses. Pumps Win32 messages during the poll so WM_DISPLAYCHANGE drains.
    // Returns true if the timing was observed to match before timeout.
    static bool WaitForTimingMatch(NvU32 displayId, const NV_TIMING& expected,
                                    DWORD timeout_ms);
    static bool WaitForTimingChange(NvU32 displayId, const NV_TIMING& previous,
                                     DWORD timeout_ms);

    // Detect that the D3D9Ex device has hit an unrecoverable state.
    // On the first observed bad HRESULT we log GetDeviceRemovedReason-style
    // info and set d3d9_dead_; subsequent calls are no-ops.
    bool CheckAndMarkD3D9Dead(HRESULT hr, const char* origin);

    Dx11Renderer* renderer_ = nullptr;
    bool          eye_swap_ = false;
    bool          auto_focus_ = true;
    bool          is_fse_ = false;  // true when device is fullscreen-exclusive

    FocusContext  focus_{};

    std::unique_ptr<platform::PresentWindow>      window_;

    // WndProc subclass to suppress minimize-on-deactivate in FSE mode.
    // suppress_minimize_ is toggled by FocusThreadLoop; when false the
    // subclass passes deactivation messages through so the FSE window
    // can minimize normally (e.g. when the user turns off "on-top").
    static LRESULT CALLBACK FseSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);
    HWND               subclassed_hwnd_   = nullptr;
    WNDPROC            orig_wndproc_      = nullptr;
    std::atomic<bool>  suppress_minimize_{true};

    // D3D9Ex objects — owned and used only by the window thread.
    Microsoft::WRL::ComPtr<IDirect3D9Ex>           d3d9_;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex>     device9_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>      back_buffer_;

    // D3D9Ex view of the DX11 out_sbs_ MISC_SHARED texture, opened via
    // IDXGIResource::GetSharedHandle → device9_->CreateTexture(pSharedHandle).
    // Because Dx11Renderer pins out_sbs_ to fixed panel-derived dims in
    // NvidiaDX9 mode, this is imported once per session and the identity
    // cache hits every subsequent frame.
    Microsoft::WRL::ComPtr<IDirect3DTexture9>      shared_input_tex_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>      shared_input_sfc_;
    void*                                          shared_src_ptr_    = nullptr;
    HANDLE                                         shared_src_handle_ = nullptr;
    uint32_t                                       shared_src_w_      = 0;
    uint32_t                                       shared_src_h_      = 0;
    DXGI_FORMAT                                    shared_src_fmt_    = DXGI_FORMAT_UNKNOWN;

    // Cross-device fence. D3D11_QUERY_EVENT signalled at the end of the
    // window thread's D3D11 work (compositor writes + OSD draws), polled
    // before the D3D9 StretchRect that reads the shared resource. The
    // analogue of the OLD CPU-readback path's Map(MAP_READ) implicit
    // sync — guarantees DX11 shader writes are visible to the D3D9 device.
    Microsoft::WRL::ComPtr<ID3D11Query>            sync_query_;

    uint32_t                                      monitor_w_ = 0;
    uint32_t                                      monitor_h_ = 0;

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

    // LightBoost state
    bool                   lightboost_enabled_ = false;
    NvTimingsEntry         monitor_timings_{};
    std::vector<NvU32>     primary_display_ids_;
    NV_TIMING              original_target_timing_{};   // captured pre-LightBoost timing of the target
    bool                   has_original_target_timing_ = false;

    // GDI device name ("\\.\\DISPLAYn") of the LightBoost target. Captured at
    // CheckAndApplyLightBoost time so DisableLightBoost can use Win32
    // ChangeDisplaySettingsExW as a fallback when NVAPI_DISP_RevertCustomDisplayTrial
    // fails (the trial state gets invalidated by FSE D3D9Ex modesets during
    // the session, leaving the panel stuck at the LightBoost timing).
    std::wstring           target_gdi_device_;
    // OS-stored DEVMODE captured before the LightBoost trial was applied.
    // ChangeDisplaySettingsExW with this mode forces a Windows-level modeset
    // that pushes the original timing to the panel.
    DEVMODEW               original_devmode_{};
    bool                   has_original_devmode_ = false;

    // D3D9Ex device-state circuit-breaker. Once set (by PresentFrame error,
    // CheckDeviceState, or WM_DISPLAYCHANGE), the present pipeline becomes a
    // no-op and stops driving a wedged driver. Cleared only by reinitialization.
    std::atomic<bool>      d3d9_dead_{false};
    // Frame counter purely for periodic CheckDeviceState — not exposed.
    int                    frames_since_state_check_ = 0;
    // Throttles per-frame Stereo_Activate retries so the 60-retry budget
    // can't burn through in <1s during driver contention.
    DWORD                  last_stereo_activate_tick_ = 0;

};

}  // namespace vrto3d

