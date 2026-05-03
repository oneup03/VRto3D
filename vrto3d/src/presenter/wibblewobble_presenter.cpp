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

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "wibblewobble_presenter.h"

#include <chrono>
#include <string>

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"

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

    // GPU completion fence. The client samples our shared out_sbs_ on its
    // own internal D3D11 device; without an explicit Flush + wait, the
    // upstream CopyResource (compositor -> out_sbs_) and the OSD composite
    // pass may still be queued on our immediate context when the client
    // reads. That race shows up as the OSD blinking out on alternating
    // frames. Mirrors the COPY_WAIT path in WibbleWobbleVR.h.
    Microsoft::WRL::ComPtr<ID3D11Query> copy_wait_query;
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
        // hook installed by osd_input_win32 (lazily, only while the OSD
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
    bool was_on_top = false;
    int  reassert_counter = 0;
    uint32_t last_auto_focused_pid = 0;

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        if (!ww_hwnd || !IsWindow(ww_hwnd)) {
            ww_hwnd = FindWindowW(nullptr, L"WibbleWobble");
            if (ww_hwnd && renderer_) {
                renderer_->SetOsdHeadsetHwnd(ww_hwnd);
                LOG() << "WibbleWobblePresenter: located WibbleWobble window hwnd=0x"
                      << std::hex << reinterpret_cast<uintptr_t>(ww_hwnd);
            }
            was_on_top = false;
        }

        if (ww_hwnd) {
            const bool is_on_top   = focus_.is_on_top   && focus_.is_on_top->load();
            const bool man_on_top  = focus_.man_on_top  && focus_.man_on_top->load();
            const uint32_t pid     = focus_.app_pid ? focus_.app_pid->load() : 0;

            const bool app_running = platform::IsProcessRunning(pid);
            if (pid == 0 || !app_running) {
                last_auto_focused_pid = 0;
            }

            bool want_on_top = false;
            if (man_on_top) {
                want_on_top = true;
            } else if (is_on_top && app_running) {
                want_on_top = true;
            } else if (auto_focus_ && !is_on_top
                       && app_running && pid != 0
                       && pid != last_auto_focused_pid) {
                if (focus_.is_on_top)  focus_.is_on_top->store(true);
                if (focus_.man_on_top) focus_.man_on_top->store(true);
                last_auto_focused_pid = pid;
                want_on_top = true;
            }

            if (want_on_top != was_on_top) {
                SetWindowPos(ww_hwnd,
                             want_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                             0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                was_on_top = want_on_top;
                reassert_counter = 0;
            } else if (want_on_top && ++reassert_counter >= 20) {
                reassert_counter = 0;
                SetWindowPos(ww_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }

        Sleep(50);
    }
}

void WibbleWobblePresenter::PresentFrame(ID3D11Texture2D* sbs_input) {
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

    // Refresh the cached shared HANDLE when the source texture pointer
    // changes (resize / format change recreates out_sbs_ in Dx11Renderer).
    // GetSharedHandle returns a HANDLE owned by the texture itself — no
    // explicit close needed; it dies with the texture.
    if (sbs_input != last_sbs_input_) {
        ComPtr<IDXGIResource> dxgi_res;
        HRESULT hr = sbs_input->QueryInterface(__uuidof(IDXGIResource),
                                                reinterpret_cast<void**>(dxgi_res.GetAddressOf()));
        if (FAILED(hr) || !dxgi_res) {
            LOG() << "WibbleWobblePresenter: QueryInterface(IDXGIResource) on out_sbs_ failed hr=0x"
                  << std::hex << hr
                  << " — make sure out_sbs_ is created with D3D11_RESOURCE_MISC_SHARED";
            shared_handle_ = nullptr;
            last_sbs_input_ = sbs_input;
            return;
        }
        HANDLE h = nullptr;
        hr = dxgi_res->GetSharedHandle(&h);
        if (FAILED(hr) || !h) {
            LOG() << "WibbleWobblePresenter: GetSharedHandle failed hr=0x" << std::hex << hr;
            shared_handle_ = nullptr;
            last_sbs_input_ = sbs_input;
            return;
        }
        shared_handle_  = h;
        last_sbs_input_ = sbs_input;
        LOG() << "WibbleWobblePresenter: shared HANDLE refreshed for new out_sbs_";
    }
    if (!shared_handle_) return;

    // Force all pending work that touches out_sbs_ (the upstream compositor
    // CopyResource AND the OSD composite pass, both queued by Dx11Renderer
    // before us) to actually execute on the GPU before we hand the texture
    // off. Without this the client's read can race ahead of our queued
    // OSD draws — visible as the OSD blinking out on alternating frames.
    if (context_) {
        context_->Flush();
        if (impl_->copy_wait_query) {
            context_->End(impl_->copy_wait_query.Get());
            while (context_->GetData(impl_->copy_wait_query.Get(), nullptr, 0, 0) != S_OK) {
                Sleep(0);  // yield rather than hot-spin
            }
        }
    }

    // Direct hand-off to the in-process client. frame_id is a monotonic
    // counter — the client uses it as a pair-atomicity key and dedups
    // re-presents of the same source frame.
    impl_->pPresentFrame(impl_->client_handle, shared_handle_, ++frame_id_);
}

void WibbleWobblePresenter::Shutdown() {
    pump_stop_.store(true);
    focus_stop_.store(true);
    if (pump_thread_.joinable())  pump_thread_.join();
    if (focus_thread_.joinable()) focus_thread_.join();
    if (renderer_) renderer_->SetOsdHeadsetHwnd(nullptr);

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
    shared_handle_  = nullptr;
    frame_id_       = 0;
    renderer_ = nullptr;
    device_   = nullptr;
    context_  = nullptr;
}

}  // namespace vrto3d

#endif  // _WIN32
