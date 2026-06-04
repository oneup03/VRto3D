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

// NVIDIA 3D Vision via NVAPI + D3D9Ex.
//
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
    void RecordComposite(ID3D11Texture2D* sbs_input) override;
    void Present() override;
    void Shutdown() override;

private:
    void WindowThreadLoop(Dx11Renderer* renderer,
                          platform::MonitorInfo primary);
    bool BuildD3D9Stack(HWND hwnd,
                        uint32_t monitor_w,
                        uint32_t monitor_h,
                        float    refresh_hz);
    // Ensures input-side packed_sysmem_/packed_input_default_ are sized to
    // input dims, and panel-side packed_default_ is sized to monitor dims.
    bool EnsurePackedSurfaces(uint32_t input_w_per_eye, uint32_t input_h);

    // Refresh the NV3D signature row if any of (panel dims, eye swap) have
    // changed since last write. LockRect's per-frame GPU sync is skipped on
    // the common "nothing changed" path.
    void RefreshSignatureIfNeeded();
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

    // Three-stage packed-surface pipeline. The intermediate step exists so
    // each eye gets a UNIFORM per-axis stretch to panel resolution before
    // NV3D's 2:1 horizontal squash, instead of a single non-uniform stretch
    // that distorted detail (e.g. 2880x1620 source → 1280x1440 per-eye
    // region had different X/Y scales — the visible "squashy" artifact).
    //
    //   1. staging_              (DX11 STAGING, input-sized) — Map(READ) target
    //   2. packed_sysmem_        (SYSTEMMEM,    input-sized) — CPU memcpy lands here
    //   3. packed_input_default_ (DEFAULT plain, input-sized) — UpdateSurface target
    //   4. packed_default_       (LOCKABLE RT,  panel-sized + header row) — final SbS
    //
    // Why CPU readback rather than cross-API texture sharing: NVIDIA's
    // legacy DXGI shared-handle path between D3D11 and D3D9Ex is unreliable
    // for our workload (observed right-eye-stuck artifacts even with
    // event-query CPU waits). Map(MAP_READ) on a STAGING texture is the
    // proven-correct sync primitive — slower but reliable.
    //
    // packed_default_ is a LOCKABLE render target — the LINEAR stretch
    // destination has to be a render target (NVIDIA refuses LINEAR
    // offscreen-plain → offscreen-plain), and lockable lets us LockRect
    // the header row to write the 20-byte NV3D signature with all four
    // bytes per pixel intact (incl. the X byte that holds 'D').
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        staging_;
    uint32_t                                       staging_w_ = 0;
    uint32_t                                       staging_h_ = 0;
    DXGI_FORMAT                                    staging_fmt_ = DXGI_FORMAT_UNKNOWN;

    Microsoft::WRL::ComPtr<IDirect3DSurface9>      packed_sysmem_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>      packed_input_default_;
    uint32_t                                       packed_w_in_ = 0;  // 2 * input per-eye width
    uint32_t                                       packed_h_in_ = 0;  // input body height

    Microsoft::WRL::ComPtr<IDirect3DSurface9>      packed_default_;

    // Output dims (panel per-eye × 2, panel height) — packed_default_.
    uint32_t                                      packed_w_out_ = 0;  // 2 * panel per-eye width
    uint32_t                                      packed_h_out_ = 0;  // panel body height (no header)

    // Cached NV3D signature state. The header row in packed_default_ only
    // needs to be re-written when one of these changes (or the surface is
    // recreated) — saves a per-frame LockRect+GPU-sync.
    uint32_t                                      sig_width_  = 0;
    uint32_t                                      sig_height_ = 0;
    bool                                          sig_swap_   = false;
    bool                                          sig_valid_  = false;

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

