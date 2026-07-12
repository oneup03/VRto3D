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


#include "nvstereo_dx9_presenter.h"

#include <chrono>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <openvr_driver.h>

#include <NV3D.hpp>

#include "dx11_renderer.h"
#include "focus_policy.h"
#include "hmd_device_driver.h"
#include "platform.h"
#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/win32_helper.hpp"

namespace vrto3d {

namespace {

// Set once CreateInterfaceDX11 succeeds. Read by MyDeviceProvider::Cleanup
// (via NvStereoWasActiveThisSession) to gate the TerminateProcess exit path.
std::atomic<bool> g_nv3d_was_active{false};

// Routes NV3D-Lib diagnostics into the driver log so [NV3D] lines interleave
// with our own messages — crash/TDR forensics depend on one unified timeline
// (the same pattern NV3D-Glass uses).
void Nv3dLogSink(NV3D::LogLevel level, const wchar_t* msg, void* /*user*/)
{
    if (!msg) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) {
        WideCharToMultiByte(CP_UTF8, 0, msg, -1, s.data(), n, nullptr, nullptr);
    }
    const char* lvl = "";
    switch (level) {
        case NV3D::LogLevel::Warning: lvl = " WARN:"; break;
        case NV3D::LogLevel::Error:   lvl = " ERROR:"; break;
        default: break;
    }
    LOG() << "[NV3D]" << lvl << " " << s;
}

std::wstring Utf8ToWide(const char* s)
{
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

}  // namespace

bool NvStereoWasActiveThisSession()
{
    return g_nv3d_was_active.load(std::memory_order_acquire);
}


bool NvStereoDx9Presenter::Init(Dx11Renderer& renderer,
                                const StereoDisplayDriverConfiguration& cfg,
                                const FocusContext& focus)
{
    renderer_         = &renderer;
    focus_            = focus;
    auto_focus_cache_ = cfg.auto_focus;
    last_eye_swap_    = cfg.eye_swap;
    renderer.Device()->GetImmediateContext(&ctx_);

    // 3D Vision output is single-monitor.
    platform::MonitorInfo primary{}, secondary{};
    if (!platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary)) {
        LOG() << "NvStereoDx9Presenter::Init: ResolveTargetMonitors failed";
        return false;
    }
    RECT mr{ primary.x, primary.y,
             primary.x + static_cast<LONG>(primary.width),
             primary.y + static_cast<LONG>(primary.height) };
    HMONITOR monitor = MonitorFromRect(&mr, MONITOR_DEFAULTTONEAREST);

    // Wire the log sink before any other library call so bring-up diagnostics
    // land in the driver log too.
    NV3D::SetLogSink(&Nv3dLogSink, nullptr);

    // Keep shipping the user-updatable LightBoost DB as a SteamVR resource;
    // the library falls back to its embedded copy when the path is null.
    char path8[512] = {};
    vr::VRResources()->GetResourceFullPath(
        "{vrto3d}/nvtimings.json", "", path8, sizeof(path8));
    nvtimings_path_w_ = Utf8ToWide(path8);

    NV3D::InitParams p{};
    p.target_monitor          = monitor;
    p.eye_swap                = cfg.eye_swap;
    p.on_top                  = true;
    p.enable_lightboost       = true;
    p.nvtimings_json_path     = nvtimings_path_w_.empty() ? nullptr
                                                          : nvtimings_path_w_.c_str();
    p.host_hwnd               = nullptr;   // library-owned FSE click-through popup
    p.enable_suppressor       = true;      // nvd3dumx OSD/rating/hotkey detours
    p.activation_retry_budget = 60;
    // Own-PID trick (from NV3D-Glass): IsProcessRunning(self) is always true,
    // so the library's tracked mode reduces to "visible while SetVisible(true)"
    // and our FocusThreadLoop stays authoritative. At the pinned lib commit,
    // tracked mode never force-foregrounds the tracked process (the NV3D.hpp
    // doc comment claiming it does is stale — see present_window.cpp there).
    // Do NOT pass 0: that selects minimize-on-host-focus-loss, and vrserver
    // loses focus the moment the game takes foreground, i.e. always.
    p.tracked_game_pid        = GetCurrentProcessId();

    LOG() << "NvStereoDx9Presenter::Init: creating NV3D interface on "
          << primary.device_name << " " << primary.width << "x" << primary.height
          << " lightboost_db=" << (p.nvtimings_json_path ? "resource" : "embedded");

    // Blocks through window creation, FSE CreateDeviceEx, LightBoost modeset
    // and stereo bring-up — can take seconds (parity with the old in-tree
    // implementation, which blocked this same driver thread up to 30s).
    const HRESULT hr = NV3D::CreateInterfaceDX11(renderer.Device(), &p, &iface_);
    if (FAILED(hr) || !iface_) {
        LOG() << "NvStereoDx9Presenter::Init: CreateInterfaceDX11 failed hr=0x"
              << std::hex << static_cast<unsigned long>(hr);
        iface_ = nullptr;
        return false;
    }
    g_nv3d_was_active.store(true, std::memory_order_release);

    render_stop_.store(false);
    render_thread_ = std::thread(&NvStereoDx9Presenter::RenderLoop, this);
    focus_stop_.store(false);
    focus_thread_ = std::thread(&NvStereoDx9Presenter::FocusThreadLoop, this);

    LOG() << "NvStereoDx9Presenter: ready (NV3D-Lib), target_monitor="
          << primary.device_name;
    return true;
}


void NvStereoDx9Presenter::RenderLoop()
{
    // WaitAndDrawPending paces off the compositor's submit signal (33ms
    // timeout fall-through re-presents, keeping stereo alive when idle) and
    // invokes RecordComposite under context_mutex_ / Present() outside it.
    // It returns false before the renderer finishes initializing and after
    // device death — sleep briefly then so the loop can't spin hot.
    while (!render_stop_.load(std::memory_order_relaxed)) {
        if (!renderer_->WaitAndDrawPending(33)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}


bool NvStereoDx9Presenter::EnsureRing(ID3D11Texture2D* sbs)
{
    D3D11_TEXTURE2D_DESC in{};
    sbs->GetDesc(&in);
    if (ring_[0] && in.Width == ring_w_ && in.Height == ring_h_ &&
        in.Format == ring_fmt_) {
        return true;
    }

    // Dims/format changed (in practice: once, on the first frame — the
    // renderer pins out_sbs_ for NvidiaDX9 mode). Old slots may still be
    // referenced by the library's import cache; SetInputTexture with the new
    // pointers below replaces them.
    for (auto& t : ring_) t.Reset();
    last_tex_ = nullptr;

    D3D11_TEXTURE2D_DESC d{};
    d.Width              = in.Width;
    d.Height             = in.Height;
    d.MipLevels          = 1;
    d.ArraySize          = 1;
    d.Format             = in.Format;
    d.SampleDesc.Count   = 1;
    d.Usage              = D3D11_USAGE_DEFAULT;
    d.BindFlags          = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Legacy KMT share — the only flavor the 3D Vision driver's D3D9 side
    // can open (NT handles are rejected).
    d.MiscFlags          = D3D11_RESOURCE_MISC_SHARED;

    for (UINT i = 0; i < kRing; ++i) {
        const HRESULT hr = renderer_->Device()->CreateTexture2D(&d, nullptr, &ring_[i]);
        if (FAILED(hr)) {
            LOG() << "NvStereoDx9Presenter: staging ring slot " << i
                  << " create failed hr=0x" << std::hex
                  << static_cast<unsigned long>(hr);
            for (auto& t : ring_) t.Reset();
            return false;
        }
    }
    ring_w_   = in.Width;
    ring_h_   = in.Height;
    ring_fmt_ = in.Format;
    ring_idx_ = 0;
    LOG() << "NvStereoDx9Presenter: staging ring " << kRing << "x "
          << ring_w_ << "x" << ring_h_ << " fmt=" << ring_fmt_;
    return true;
}


void NvStereoDx9Presenter::RecordComposite(ID3D11Texture2D* sbs_input)
{
    if (dead_.load(std::memory_order_relaxed) || !iface_ || !sbs_input || !ctx_) {
        return;
    }

    // Host-side device-removed detection. The library often can't observe a
    // TDR itself (a hidden popup means no D3D9 call runs), so we watch our
    // own D3D11 device and hand the dead-mark over via NotifyDeviceLost.
    if (++frames_since_dev_check_ >= 60) {
        frames_since_dev_check_ = 0;
        if (FAILED(renderer_->Device()->GetDeviceRemovedReason())) {
            MarkDead("D3D11 device removed");
            return;
        }
    }

    // Occluded-present protection: PresentEx against a minimized FSE popup
    // wedges some drivers. want_visible_ flips before the focus thread calls
    // SetVisible, so submissions stop before/while the library drains.
    if (!want_visible_.load(std::memory_order_relaxed)) {
        return;
    }

    if (!EnsureRing(sbs_input)) return;
    ring_idx_ = (ring_idx_ + 1) % kRing;
    ID3D11Texture2D* slot = ring_[ring_idx_].Get();
    // The previous slots may still be mid-StretchRect on the library's worker
    // (KMT sharing has no implicit sync) — that's exactly why this copy goes
    // into a rotating slot rather than handing out_sbs_ over directly.
    ctx_->CopyResource(slot, sbs_input);

    // Live eye-swap: hotkey/OSD toggles land in the config; the library
    // applies the change on the next frame without re-init.
    if (renderer_->Component()) {
        const bool swap = renderer_->Component()->GetConfig().eye_swap;
        if (swap != last_eye_swap_) {
            last_eye_swap_ = swap;
            iface_->SetEyeSwap(swap);
        }
    }

    if (slot != last_tex_) {
        const HRESULT hr = iface_->SetInputTexture(slot);
        if (FAILED(hr)) {
            LOG() << "NvStereoDx9Presenter: SetInputTexture failed hr=0x"
                  << std::hex << static_cast<unsigned long>(hr);
            return;
        }
        last_tex_ = slot;
    }

    // Signals the lib's fence / EVENT query on our immediate context (hence
    // under context_mutex_) and returns in microseconds; the D3D9 StretchRect
    // + PresentEx run on the library's async worker. The HRESULT reflects the
    // PREVIOUS frame, so judge failure by streak, not a single result.
    if (FAILED(iface_->Present())) {
        if (++present_fail_streak_ >= 40) {
            MarkDead("40-frame present failure streak");
        }
    } else {
        present_fail_streak_ = 0;
    }
}


void NvStereoDx9Presenter::FocusThreadLoop()
{
    using namespace std::chrono_literals;

    // Popup starts visible (the library creates it shown with FSE engaged);
    // act on policy transitions only, like the old loop.
    bool was_on_top = false;
    FocusLatchState latch;   // auto-focus per-PID latch (focus_policy.h)

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        FocusInputs fi;
        fi.is_on_top    = focus_.is_on_top  && focus_.is_on_top->load();
        fi.man_on_top   = focus_.man_on_top && focus_.man_on_top->load();
        fi.app_pid      = focus_.app_pid ? focus_.app_pid->load() : 0;
        // Live mirror — falls back to the init-time cache if the driver
        // didn't plumb the pointer.
        fi.auto_focus   = focus_.auto_focus ? focus_.auto_focus->load()
                                            : auto_focus_cache_;
        fi.app_running  = platform::IsProcessRunning(fi.app_pid);
        fi.force_on_top = false;

        bool set_is = false, set_man = false;
        const bool want = ComputeWantOnTop(fi, latch, &set_is, &set_man);
        if (set_is  && focus_.is_on_top)  focus_.is_on_top->store(true);
        if (set_man && focus_.man_on_top) focus_.man_on_top->store(true);

        if (want != was_on_top) {
            // Stop submissions first (see RecordComposite), then let the
            // library's window thread do the SW_MINIMIZE/SW_RESTORE + topmost
            // + suppression sequencing. Never touch the popup HWND from here:
            // cross-thread ShowWindow on a FSE D3D9Ex device window wedges
            // DWM. SetVisible(false) may block ~1s (worker drain + GPU idle)
            // — acceptable on this thread, never call it under context_mutex_.
            want_visible_.store(want, std::memory_order_relaxed);
            iface_->SetVisible(want);
            if (want) {
                StartForceFocusWatcher(fi.app_pid);
            } else {
                StopForceFocusWatcher();
            }
            was_on_top = want;
        }

        std::this_thread::sleep_for(50ms);
    }
}


void NvStereoDx9Presenter::StartForceFocusWatcher(uint32_t pid)
{
    StopForceFocusWatcher();
    if (pid == 0 || pid == GetCurrentProcessId()) return;

    // When SteamVR is launched by the app, the game window may not exist yet,
    // or SteamVR's bring-up (vrmonitor / status window) may grab foreground —
    // re-assert focus onto the game whenever it drifts, for up to 15s.
    //
    // JOINABLE by design: a detached watcher mid-ForceFocus at process exit
    // leaves the OS-wide input chain attached to a dying thread ("display
    // frozen, cursor stuck, reboot required"). Joining caps the wedge window
    // at one iteration; the stop flag is polled in 100ms slices so joins are
    // prompt.
    focus_watcher_stop_ = std::make_shared<std::atomic<bool>>(false);
    auto stop       = focus_watcher_stop_;
    auto man_on_top = focus_.man_on_top;
    focus_watcher_thread_ = std::thread([pid, stop, man_on_top]() {
        for (int i = 0; i < 15; ++i) {
            if (stop->load(std::memory_order_relaxed)) return;
            if (man_on_top && !man_on_top->load()) return;
            HWND game_hwnd = GetHWNDFromPID(pid);
            if (game_hwnd && GetForegroundWindow() != game_hwnd) {
                ForceFocus(game_hwnd,
                           GetCurrentThreadId(),
                           GetWindowThreadProcessId(game_hwnd, nullptr));
            }
            for (int j = 0; j < 10; ++j) {
                if (stop->load(std::memory_order_relaxed)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}


void NvStereoDx9Presenter::StopForceFocusWatcher()
{
    if (focus_watcher_stop_) {
        focus_watcher_stop_->store(true, std::memory_order_relaxed);
    }
    if (focus_watcher_thread_.joinable()) {
        focus_watcher_thread_.join();
    }
    focus_watcher_stop_.reset();
}


void NvStereoDx9Presenter::MarkDead(const char* why)
{
    if (dead_.exchange(true, std::memory_order_acq_rel)) return;
    LOG() << "NvStereoDx9Presenter: marking dead (" << why
          << ") — output disabled until SteamVR restart";
    // Hand the dead-mark to the library BEFORE Delete() so teardown takes the
    // non-blocking path (no Stereo_DestroyHandle, Detach instead of Release —
    // both can block indefinitely against a wedged kernel driver).
    if (iface_) iface_->NotifyDeviceLost();
}


void NvStereoDx9Presenter::Shutdown()
{
    // Join order is load-bearing: focus thread (calls SetVisible) and watcher
    // first, then the render thread (calls Present via RecordComposite), then
    // the library teardown — Delete() drains its own worker, unhooks the
    // suppressor, releases D3D9/NvAPI (Detach path when dead) and destroys
    // its window last.
    focus_stop_.store(true);
    if (focus_thread_.joinable()) focus_thread_.join();
    StopForceFocusWatcher();

    render_stop_.store(true);
    if (render_thread_.joinable()) render_thread_.join();

    if (iface_) {
        if (dead_.load(std::memory_order_acquire)) {
            iface_->NotifyDeviceLost();   // idempotent — belt & braces
        }
        LOG() << "NvStereoDx9Presenter: Shutdown — deleting NV3D interface";
        iface_->Delete();
        iface_ = nullptr;
    }

    for (auto& t : ring_) t.Reset();
    last_tex_ = nullptr;
    ring_w_ = ring_h_ = 0;
    ring_fmt_ = DXGI_FORMAT_UNKNOWN;
    ctx_.Reset();
    renderer_ = nullptr;
}

}  // namespace vrto3d
