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


#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "wibblewobble_presenter.h"

#include <chrono>
#include <string>

#include <thread>

#include "dx11_renderer.h"
#include "focus_policy.h"
#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/win32_helper.hpp"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// WibbleWobble C source format constants. Match WWSourceFormat in the WWVROU3
// project's WibbleWobbleCapture.h. Vendored as plain ints so we don't have
// to include any of the WW headers.
// ---------------------------------------------------------------------------
constexpr int kWWSF_SideBySideFull = 3;

// ---------------------------------------------------------------------------
// WibbleWobbleClient.dll C ABI. Resolved at runtime so we don't link against
// WibbleWobbleClient.lib or pull in its heavy header chain.
// ---------------------------------------------------------------------------
using WWClientHandle = void*;
using PFN_WWClient_Create          = WWClientHandle (__cdecl*)(void* parent_hwnd, uint32_t server_process_id);
using PFN_WWClient_IsRunning       = int            (__cdecl*)(WWClientHandle);
using PFN_WWClient_Destroy         = void           (__cdecl*)(WWClientHandle);
using PFN_WWClient_SetSourceFormat = void           (__cdecl*)(WWClientHandle, int wwsf);
using PFN_WWClient_PresentFrame    = int            (__cdecl*)(WWClientHandle, void* shared_handle, uint64_t frame_id);

// ---------------------------------------------------------------------------
// Registry lookup for HKLM\SOFTWARE\PHARTGAMES\WibbleWobbleClient\install_path.
// Returns the install directory (with trailing backslash, matching the value
// the WibbleWobbleClient install .bat writes), or empty on failure.
// ---------------------------------------------------------------------------
std::wstring ReadInstallPathFromRegistry() {
    HKEY hKey = nullptr;
    LONG r = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\PHARTGAMES\\WibbleWobbleClient",
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &hKey);
    if (r != ERROR_SUCCESS) return {};

    WCHAR buf[MAX_PATH] = {};
    DWORD bytes = sizeof(buf) - sizeof(WCHAR);  // leave room for null
    DWORD type = 0;
    r = RegQueryValueExW(hKey, L"install_path", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &bytes);
    RegCloseKey(hKey);
    if (r != ERROR_SUCCESS || type != REG_SZ) return {};

    std::wstring path = buf;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    return path;
}

// ---------------------------------------------------------------------------
// WibbleWobble window WndProc subclass — intercepts WM_CLOSE (Alt+F4 or X
// button on the WW window) and triggers the same SteamVR shutdown path that
// our own present window uses. WM_CLOSE is passed through to the original
// WndProc so the WW window still closes normally.
//
// Single static slot — there's only ever one WW window at a time, and only
// one WibbleWobblePresenter instance.
// ---------------------------------------------------------------------------
WNDPROC g_ww_orig_wndproc = nullptr;
HWND    g_ww_subclassed_hwnd = nullptr;

LRESULT CALLBACK WwSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        // If a game is connected, ask it to close first so SteamVR doesn't
        // prompt — same flow as our own present window's WM_CLOSE.
        const uint32_t pid = g_current_app_pid.load();
        LOG() << "WibbleWobble window WM_CLOSE — pid=" << pid;
        std::thread([pid]{ RequestSteamVRShutdownWithApp(pid); }).detach();
    }
    WNDPROC orig = g_ww_orig_wndproc;
    if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

void InstallWwSubclass(HWND hwnd) {
    if (!hwnd || g_ww_subclassed_hwnd == hwnd) return;
    g_ww_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(WwSubclassProc)));
    if (g_ww_orig_wndproc) {
        g_ww_subclassed_hwnd = hwnd;
        LOG() << "WibbleWobblePresenter: installed WM_CLOSE subclass on WW window";
    } else {
        LOG() << "WibbleWobblePresenter: SetWindowLongPtrW failed GLE="
              << GetLastError() << " — Alt+F4 on WW window won't quit SteamVR";
    }
}

void RemoveWwSubclass() {
    if (!g_ww_subclassed_hwnd || !g_ww_orig_wndproc) return;
    // Only restore if our proc is still in place — the WW client could have
    // re-subclassed us, in which case touching it would corrupt the chain.
    auto current = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(g_ww_subclassed_hwnd, GWLP_WNDPROC));
    if (current == WwSubclassProc && IsWindow(g_ww_subclassed_hwnd)) {
        SetWindowLongPtrW(g_ww_subclassed_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_ww_orig_wndproc));
    }
    g_ww_subclassed_hwnd = nullptr;
    g_ww_orig_wndproc = nullptr;
}

}  // namespace

namespace vrto3d {

// ---------------------------------------------------------------------------
// pImpl — keeps Windows.h confined to the .cpp.
// ---------------------------------------------------------------------------
struct WibbleWobblePresenter::Impl {
    HMODULE                       client_dll       = nullptr;
    WWClientHandle                client_handle    = nullptr;
    PFN_WWClient_Create           pCreate          = nullptr;
    PFN_WWClient_IsRunning        pIsRunning       = nullptr;
    PFN_WWClient_Destroy          pDestroy         = nullptr;
    PFN_WWClient_SetSourceFormat  pSetSourceFormat = nullptr;
    PFN_WWClient_PresentFrame     pPresentFrame    = nullptr;

    // GPU completion fence. The client samples our shared ring textures
    // on its own internal D3D11 device; without an explicit Flush + wait,
    // the CopyResource (out_sbs_ -> ring[idx]) and the upstream renderer
    // work (L+R copies + OSD composite into out_sbs_) may still be queued
    // on our immediate context when the client reads. Mirrors the
    // COPY_WAIT path in WibbleWobbleVR.h.
    Microsoft::WRL::ComPtr<ID3D11Query> copy_wait_query;

    // Triple-buffer ring of shared SBS textures. Each PresentFrame copies
    // out_sbs_ into ring[ring_idx] and hands ring_handles[ring_idx] to the
    // WW client, then advances. The client samples the slot on its own
    // internal D3D11 device at its own display rate; by the time we wrap
    // back to a slot it has moved on.
    //
    // Without the ring, the *next* compositor frame's L+R CopyResources
    // into out_sbs_ replace (not blend) the pixels the OSD additive-blend
    // pass wrote on top — if WW reads in that window the OSD blinks out
    // on alternating frames. The Flush + event-wait above only fixes our
    // half of the race (queued draws on our side); the ring fixes the
    // other half (next-frame writes vs. WW's still-pending read).
    static constexpr int kRingSize = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> ring[kRingSize];
    void*                                   ring_handles[kRingSize] = {};
    int                                     ring_idx = 0;
};

WibbleWobblePresenter::WibbleWobblePresenter() = default;

WibbleWobblePresenter::~WibbleWobblePresenter() {
    Shutdown();
}

bool WibbleWobblePresenter::Init(Dx11Renderer& renderer,
                                  const StereoDisplayDriverConfiguration& cfg,
                                  const FocusContext& focus)
{
    renderer_   = &renderer;
    device_     = renderer.Device();
    context_    = renderer.Context();
    auto_focus_ = cfg.auto_focus;
    focus_      = focus;
    if (!device_ || !context_) {
        LOG() << "WibbleWobblePresenter: Init failed — Dx11Renderer device/context unavailable";
        return false;
    }

    std::wstring install_path = ReadInstallPathFromRegistry();
    if (install_path.empty()) {
        LOG() << "WibbleWobblePresenter: HKLM\\SOFTWARE\\PHARTGAMES\\WibbleWobbleClient\\install_path "
                 "not found — install WibbleWobbleClient first";
        return false;
    }

    impl_ = std::make_unique<Impl>();

    // Lazily create the GPU-completion fence used in PresentFrame.
    {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_EVENT;
        device_->CreateQuery(&qd, impl_->copy_wait_query.GetAddressOf());
        if (!impl_->copy_wait_query) {
            LOG() << "WibbleWobblePresenter: CreateQuery(D3D11_QUERY_EVENT) failed — "
                     "OSD may flicker due to missing cross-device GPU sync";
        }
    }

    auto fail = [&](const char* what, DWORD err = ERROR_SUCCESS) {
        LOG() << "WibbleWobblePresenter: " << what
              << (err ? " err=" : "") << (err ? std::to_string(err) : std::string{});
        if (impl_->client_handle && impl_->pDestroy) impl_->pDestroy(impl_->client_handle);
        if (impl_->client_dll)   FreeLibrary(impl_->client_dll);
        impl_.reset();
        return false;
    };

    // Use install_path as DLL search root so dependent WibbleWobble*.dll
    // family + Data/ resolve from there rather than our own SteamVR drivers
    // folder.
    SetDllDirectoryW(install_path.c_str());
    std::wstring dll_path = install_path + L"WibbleWobbleClient.dll";
    impl_->client_dll = LoadLibraryW(dll_path.c_str());
    SetDllDirectoryW(nullptr);
    if (!impl_->client_dll) {
        return fail("LoadLibrary(WibbleWobbleClient.dll) failed", GetLastError());
    }

    impl_->pCreate          = reinterpret_cast<PFN_WWClient_Create>         (GetProcAddress(impl_->client_dll, "WWClient_Create"));
    impl_->pIsRunning       = reinterpret_cast<PFN_WWClient_IsRunning>      (GetProcAddress(impl_->client_dll, "WWClient_IsRunning"));
    impl_->pDestroy         = reinterpret_cast<PFN_WWClient_Destroy>        (GetProcAddress(impl_->client_dll, "WWClient_Destroy"));
    impl_->pSetSourceFormat = reinterpret_cast<PFN_WWClient_SetSourceFormat>(GetProcAddress(impl_->client_dll, "WWClient_SetSourceFormat"));
    impl_->pPresentFrame    = reinterpret_cast<PFN_WWClient_PresentFrame>   (GetProcAddress(impl_->client_dll, "WWClient_PresentFrame"));
    if (!impl_->pCreate || !impl_->pIsRunning || !impl_->pDestroy
     || !impl_->pSetSourceFormat || !impl_->pPresentFrame) {
        return fail("WibbleWobbleClient.dll missing one of WWClient_Create/IsRunning/Destroy/"
                    "SetSourceFormat/PresentFrame — rebuild WibbleWobbleClient with the latest "
                    "C-API shim");
    }

    impl_->client_handle = impl_->pCreate(nullptr, GetCurrentProcessId());
    if (!impl_->client_handle) {
        return fail("WWClient_Create returned null");
    }

    // VRto3D always produces canonical 2W x H side-by-side full. Tell the
    // client once at startup; SubmitExternalFrame uses this to decode.
    impl_->pSetSourceFormat(impl_->client_handle, kWWSF_SideBySideFull);

    LOG() << "WibbleWobblePresenter: WibbleWobbleClient.dll loaded in-process";

    pump_stop_.store(false);
    pump_thread_ = std::thread(&WibbleWobblePresenter::PumpThreadLoop, this);

    focus_stop_.store(false);
    focus_thread_ = std::thread(&WibbleWobblePresenter::FocusThreadLoop, this);

    return true;
}

void WibbleWobblePresenter::PumpThreadLoop() {
    while (!pump_stop_.load(std::memory_order_relaxed)) {
        if (renderer_) renderer_->WaitAndDrawPending(33);

        // Drain this thread's Windows message queue so the WH_MOUSE_LL
        // hook installed by osd_input (lazily, only while the OSD
        // menu is visible) can dispatch its callbacks.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void WibbleWobblePresenter::FocusThreadLoop() {
    // The WibbleWobbleClient names its main window "WibbleWobble". Poll for
    // it (it can take a moment to appear after Create), then mirror the
    // focus pattern from LeiaSrPresenter::FocusThreadLoop:
    //   - Track FocusContext (is_on_top / man_on_top / app_pid)
    //   - BringToTop / NotTopmost based on those flags + auto_focus
    //   - Re-assert topmost periodically (other apps can bump us)
    // Plus: feed the discovered HWND to Dx11Renderer so the OSD's cursor
    // coord mapping works against the actual lightfield surface rect.
    HWND ww_hwnd = nullptr;
    int  reassert_counter = 0;
    FocusLatchState focus_latch;   // auto-focus per-PID latch (focus_policy.h)
    bool was_on_top = false;

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        if (!ww_hwnd || !IsWindow(ww_hwnd)) {
            ww_hwnd = FindWindowW(nullptr, L"WibbleWobble");
            if (ww_hwnd && renderer_) {
                renderer_->SetOsdHeadsetHwnd(ww_hwnd);
                LOG() << "WibbleWobblePresenter: located WibbleWobble window hwnd=0x"
                      << std::hex << reinterpret_cast<uintptr_t>(ww_hwnd);
                InstallWwSubclass(ww_hwnd);
            }
        }

        if (ww_hwnd) {
            // Shared decision (see focus_policy.h) — kept identical to
            // WindowPresenter / the Linux VkRenderer focus block so they
            // can't drift.
            FocusInputs fi;
            fi.is_on_top    = focus_.is_on_top   && focus_.is_on_top->load();
            fi.man_on_top   = focus_.man_on_top  && focus_.man_on_top->load();
            fi.app_pid      = focus_.app_pid ? focus_.app_pid->load() : 0;
            fi.auto_focus   = focus_.auto_focus  ? focus_.auto_focus->load() : auto_focus_;
            fi.app_running  = platform::IsProcessRunning(fi.app_pid);
            fi.force_on_top = false;  // topmost-model window: an open OSD is
                                      // made visible/clickable via
                                      // ApplyMenuVisibility, not by forcing
                                      // topmost.
            bool set_is = false, set_man = false;
            const bool want_on_top =
                ComputeWantOnTop(fi, focus_latch, &set_is, &set_man);
            if (set_is  && focus_.is_on_top)  focus_.is_on_top->store(true);
            if (set_man && focus_.man_on_top) focus_.man_on_top->store(true);
            const uint32_t pid = fi.app_pid;

            // Reconcile against the window's actual WS_EX_TOPMOST style each
            // tick — the WibbleWobbleClient sets topmost on its window
            // asynchronously after creation, so a transition-based check
            // (was_on_top vs want_on_top) misses the WW client flipping the
            // bit on us. Reading the live style means we always converge.
            const LONG_PTR ex = GetWindowLongPtrW(ww_hwnd, GWL_EXSTYLE);
            const bool actually_topmost     = (ex & WS_EX_TOPMOST)     != 0;
            const bool actually_transparent = (ex & WS_EX_TRANSPARENT) != 0;

            if (actually_topmost != want_on_top) {
                SetWindowPos(ww_hwnd,
                             want_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                             0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                reassert_counter = 0;
            } else if (want_on_top && ++reassert_counter >= 20) {
                // Periodic re-assert pushes us above any newer topmost
                // windows (dialogs, dashboards) inside the topmost group.
                reassert_counter = 0;
                SetWindowPos(ww_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }

            // Mirror WindowPresenter / LeiaSrPresenter input semantics:
            // click-through while on-top (so input falls through to the
            // game window underneath) and solid otherwise. The WW client
            // window ships with WS_EX_TRANSPARENT set, so we have to
            // clear it on first contact to make it solid by default.
            // Like the topmost reconciliation above, this runs every tick
            // to override any async re-asserts from the WW client.
            if (actually_transparent != want_on_top) {
                SetWindowLongPtrW(ww_hwnd, GWL_EXSTYLE,
                                  want_on_top
                                      ? (ex |  WS_EX_TRANSPARENT)
                                      : (ex & ~WS_EX_TRANSPARENT));
            }

            // On the rising edge of want_on_top, spawn the same 15-iteration
            // ForceFocus watchdog that WindowPresenter / LeiaSrPresenter use.
            // The WW window being WS_EX_TOPMOST isn't enough — the game also
            // needs to be the foreground window so keyboard/mouse input
            // reaches it. When SteamVR is launched by the game the game
            // window may not exist yet at the first transition, and
            // SteamVR's bring-up (vrmonitor / status window) can grab
            // foreground after the first attempt — re-asserting once per
            // second for 15s handles both cases.
            if (want_on_top && !was_on_top && pid != 0) {
                std::thread([pid, man_on_top = focus_.man_on_top]() {
                    for (int i = 0; i < 15; ++i) {
                        if (man_on_top && !man_on_top->load()) return;
                        HWND game_hwnd = GetHWNDFromPID(pid);
                        if (game_hwnd && GetForegroundWindow() != game_hwnd) {
                            ForceFocus(game_hwnd,
                                       GetCurrentThreadId(),
                                       GetWindowThreadProcessId(game_hwnd, nullptr));
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }).detach();
            }
            was_on_top = want_on_top;
        }

        Sleep(50);
    }
}

void WibbleWobblePresenter::RecordComposite(ID3D11Texture2D* sbs_input) {
    if (!impl_ || !sbs_input || !impl_->client_handle || !impl_->pPresentFrame) return;

    // Liveness check on the in-process client. If its worker thread exited,
    // stop the pump so we don't keep doing pointless work.
    static std::atomic<bool> client_dead_logged{false};
    if (impl_->pIsRunning(impl_->client_handle) == 0) {
        bool e = false;
        if (client_dead_logged.compare_exchange_strong(e, true)) {
            LOG() << "WibbleWobblePresenter: WibbleWobbleClient worker stopped — "
                     "stopping pump thread (restart SteamVR to retry)";
            pump_stop_.store(true, std::memory_order_relaxed);
        }
        return;
    }

    // (Re)create the ring whenever the source texture changes — Dx11Renderer
    // recreates out_sbs_ on resize / format change (and on the first frame),
    // which always changes the pointer. Slots are sized to match sbs_input
    // so CopyResource below is a straight blit.
    if (sbs_input != last_sbs_input_) {
        D3D11_TEXTURE2D_DESC src{};
        sbs_input->GetDesc(&src);

        D3D11_TEXTURE2D_DESC d = src;
        d.MipLevels      = 1;
        d.ArraySize      = 1;
        d.SampleDesc     = {1, 0};
        d.Usage           = D3D11_USAGE_DEFAULT;
        d.BindFlags       = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.CPUAccessFlags  = 0;
        d.MiscFlags       = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < Impl::kRingSize; ++i) {
            impl_->ring[i].Reset();
            impl_->ring_handles[i] = nullptr;
            HRESULT hr = device_->CreateTexture2D(&d, nullptr, impl_->ring[i].GetAddressOf());
            if (FAILED(hr) || !impl_->ring[i]) {
                LOG() << "WibbleWobblePresenter: CreateTexture2D(ring[" << i << "]) failed hr=0x"
                      << std::hex << hr;
                continue;
            }
            ComPtr<IDXGIResource> dxgi_res;
            hr = impl_->ring[i]->QueryInterface(
                __uuidof(IDXGIResource),
                reinterpret_cast<void**>(dxgi_res.GetAddressOf()));
            HANDLE h = nullptr;
            if (SUCCEEDED(hr) && dxgi_res) {
                hr = dxgi_res->GetSharedHandle(&h);
            }
            if (FAILED(hr) || !h) {
                LOG() << "WibbleWobblePresenter: GetSharedHandle(ring[" << i << "]) failed hr=0x"
                      << std::hex << hr;
                continue;
            }
            impl_->ring_handles[i] = h;
        }
        impl_->ring_idx = 0;
        last_sbs_input_ = sbs_input;
        LOG() << "WibbleWobblePresenter: SBS ring (re)created " << d.Width << "x" << d.Height
              << " fmt=" << d.Format;
    }

    const int idx = impl_->ring_idx;
    if (!impl_->ring[idx] || !impl_->ring_handles[idx] || !context_) return;

    // Snapshot out_sbs_ (which now holds the freshly-composited stereo +
    // OSD) into the current ring slot. The Flush + event-wait below covers
    // the upstream renderer's queued L+R copies + OSD composite AND this
    // copy, so the client's cross-device sample sees committed bytes.
    context_->CopyResource(impl_->ring[idx].Get(), sbs_input);
    context_->Flush();
    if (impl_->copy_wait_query) {
        context_->End(impl_->copy_wait_query.Get());
        while (context_->GetData(impl_->copy_wait_query.Get(), nullptr, 0, 0) != S_OK) {
            Sleep(0);  // yield rather than hot-spin
        }
    }

    // Hand the slot off and advance. WW continues sampling ring[idx] for
    // an indeterminate time on its own device; we won't overwrite the slot
    // until we wrap (kRingSize frames from now), which decouples its read
    // from the next compositor frame's writes into out_sbs_.
    impl_->pPresentFrame(impl_->client_handle, impl_->ring_handles[idx], ++frame_id_);
    impl_->ring_idx = (idx + 1) % Impl::kRingSize;
}

void WibbleWobblePresenter::Shutdown() {
    pump_stop_.store(true);
    focus_stop_.store(true);
    if (pump_thread_.joinable())  pump_thread_.join();
    if (focus_thread_.joinable()) focus_thread_.join();
    if (renderer_) renderer_->SetOsdHeadsetHwnd(nullptr);
    RemoveWwSubclass();

    if (impl_) {
        if (impl_->client_handle && impl_->pDestroy) {
            impl_->pDestroy(impl_->client_handle);
            impl_->client_handle = nullptr;
        }
        if (impl_->client_dll) {
            FreeLibrary(impl_->client_dll);
            impl_->client_dll = nullptr;
        }
        impl_.reset();
    }

    last_sbs_input_ = nullptr;
    frame_id_       = 0;
    renderer_ = nullptr;
    device_   = nullptr;
    context_  = nullptr;
}

}  // namespace vrto3d

