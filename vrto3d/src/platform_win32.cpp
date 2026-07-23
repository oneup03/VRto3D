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
#include "platform.h"

#include <windows.h>
#include <d3d10.h>      // ID3D10Multithread
#include <d3d11_1.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cstring>

#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/win32_helper.hpp"

#include <thread>

namespace platform {

namespace {

struct EnumState {
    std::vector<MonitorInfo>* out;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam)
{
    auto* state = reinterpret_cast<EnumState*>(lParam);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMonitor, &info)) return TRUE;

    MonitorInfo m;
    m.index       = static_cast<int32_t>(state->out->size()) + 1;
    m.x           = info.rcMonitor.left;
    m.y           = info.rcMonitor.top;
    m.width       = static_cast<uint32_t>(info.rcMonitor.right  - info.rcMonitor.left);
    m.height      = static_cast<uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
    m.is_primary  = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    // Narrow device name to UTF-8 for logs / config matching.
    char narrow[CCHDEVICENAME * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, narrow, sizeof(narrow), nullptr, nullptr);
    m.device_name = narrow;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        m.refresh_hz = static_cast<float>(dm.dmDisplayFrequency);
    }

    state->out->push_back(m);
    return TRUE;
}

}  // namespace


std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> out;
    EnumState state{ &out };
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&state));

    // Ensure primary is at index 1 if possible (Windows enumerates in unspecified order).
    auto primary_it = std::find_if(out.begin(), out.end(), [](const MonitorInfo& m){ return m.is_primary; });
    if (primary_it != out.end() && primary_it != out.begin()) {
        std::iter_swap(out.begin(), primary_it);
        for (size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<int32_t>(i) + 1;
    }
    return out;
}


bool ResolveTargetMonitors(int32_t display_index,
                           bool multi_display,
                           MonitorInfo& out_primary,
                           MonitorInfo& out_secondary)
{
    out_secondary = {};
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) return false;

    // 0 or out-of-range -> primary (first).
    if (display_index <= 0 || display_index > static_cast<int32_t>(monitors.size())) {
        out_primary = monitors.front();
    } else {
        out_primary = monitors[static_cast<size_t>(display_index - 1)];
    }

    if (multi_display) {
        // Find a contiguous right neighbor with matching width/height + same top Y.
        const int32_t right_x = out_primary.x + static_cast<int32_t>(out_primary.width);
        for (const auto& m : monitors) {
            if (m.index == out_primary.index) continue;
            if (m.y == out_primary.y
                && m.x == right_x
                && m.width  == out_primary.width
                && m.height == out_primary.height) {
                out_secondary = m;
                break;
            }
        }
        if (out_secondary.width == 0) {
            LOG() << "multi_display=true but no contiguous right neighbor matched; continuing single-monitor.";
        }
    }
    return true;
}


float QueryRefreshHz(const MonitorInfo& monitor, float fallback_hz)
{
    if (monitor.refresh_hz > 1.0f) return monitor.refresh_hz;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);

    std::wstring wide;
    wide.resize(monitor.device_name.size() + 1);
    int n = MultiByteToWideChar(CP_UTF8, 0, monitor.device_name.c_str(), -1,
                                 wide.data(), static_cast<int>(wide.size()));
    if (n > 0 && EnumDisplaySettingsW(wide.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmDisplayFrequency > 1) return static_cast<float>(dm.dmDisplayFrequency);
    }
    return fallback_hz;
}


LUID PrimaryAdapterLuid()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return LUID{};

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) return LUID{};

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) return LUID{};
    return desc.AdapterLuid;
}


bool CreateD3D11Device(LUID adapter_luid,
                       Microsoft::WRL::ComPtr<ID3D11Device>& out_device,
                       Microsoft::WRL::ComPtr<ID3D11DeviceContext>& out_context,
                       Microsoft::WRL::ComPtr<IDXGIAdapter1>& out_adapter)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    // Match on LUID; fall back to adapter 0 if no match.
    for (UINT i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        a->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart  == adapter_luid.LowPart
         && desc.AdapterLuid.HighPart == adapter_luid.HighPart) {
            out_adapter = a;
            break;
        }
    }
    if (!out_adapter) {
        if (FAILED(factory->EnumAdapters1(0, &out_adapter))) return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(out_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                   levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, &out_device, &got, &out_context);
#ifdef _DEBUG
    if (FAILED(hr)) {
        // Debug layer may not be installed; retry without it.
        hr = D3D11CreateDevice(out_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, static_cast<UINT>(std::size(levels)),
                               D3D11_SDK_VERSION, &out_device, &got, &out_context);
    }
#endif
    if (FAILED(hr)) return false;

    // Turn on D3D11's per-call immediate-context lock. Our own code serializes
    // context access with a mutex, but external present-chain hooks (RTSS,
    // Discord overlay, Steam overlay, OBS game-capture) draw their overlay
    // from inside IDXGISwapChain::Present using the *same* immediate context,
    // outside our mutex. Without this flag the runtime assumes single-threaded
    // context use and the overlay's draws can race our compositor thread's
    // OnDirectModeFrame work — manifesting as a device-removed crash within
    // seconds (reported against RTSS at 7680x2160 + Cyberpunk).
    Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(out_context.As(&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
    }
    return true;
}


bool ImportSharedTexture(ID3D11Device* device,
                         vr::SharedTextureHandle_t handle,
                         Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture)
{
    if (!device || handle == 0) return false;

    HANDLE h = reinterpret_cast<HANDLE>(handle);

    // Try the NT-handle path first (Prop_GraphicsAdapterLuid + OpenSharedResource1).
    Microsoft::WRL::ComPtr<ID3D11Device1> dev1;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev1)))) {
        if (SUCCEEDED(dev1->OpenSharedResource1(h, IID_PPV_ARGS(&out_texture)))) {
            return true;
        }
    }
    // Fall back to legacy shared handle.
    if (SUCCEEDED(device->OpenSharedResource(h, IID_PPV_ARGS(&out_texture)))) {
        return true;
    }
    return false;
}


//==========================================================================
// Win32 present window
//==========================================================================

namespace {

constexpr wchar_t kWndClassName[] = L"VRto3D_PresentWindow";

LRESULT CALLBACK PresentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CLOSE: {
            // Alt+F4 (or the window's "X" button) on our present window
            // routes here. If a game is currently connected to SteamVR,
            // post WM_CLOSE to it first so SteamVR doesn't prompt "an
            // application is using SteamVR, are you sure?" — then wait for
            // the game process to exit before tearing SteamVR down. With
            // nothing connected, exits SteamVR immediately. Detached so
            // the poll loops inside the helper don't stall this pump.
            const uint32_t pid = g_current_app_pid.load();
            LOG() << "VRto3D window WM_CLOSE — pid=" << pid;
            SetPropW(hwnd, L"vrto3d_close", reinterpret_cast<HANDLE>(1));
            std::thread([pid]{ RequestSteamVRShutdownWithApp(pid); }).detach();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // we paint everything via D3D

        // Eat DPI-resize messages. Our swap chain is sized at window creation
        // and we never want Windows to grow our client rect to a DPI-scaled
        // suggested size — DefWindowProc would do that on Per-Monitor-V2
        // windows and the swap chain would be stretched off the visible
        // monitor.
        case 0x02E0:  // WM_DPICHANGED
        case 0x02E4:  // WM_GETDPISCALEDSIZE
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureClass()
{
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PresentWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWndClassName;
    RegisterClassExW(&wc);
    registered = true;
}

class Win32PresentWindow : public PresentWindow {
public:
    Win32PresentWindow(HWND h, uint32_t w, uint32_t ht) : hwnd_(h), w_(w), h_(ht) {}
    ~Win32PresentWindow() override
    {
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void*    NativeHandle() const override { return hwnd_; }
    uint32_t Width()        const override { return w_; }
    uint32_t Height()       const override { return h_; }

    void PollEvents() override
    {
        MSG msg;
        while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void Show() override
    {
        if (IsWindowVisible(hwnd_)) return;
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }

    void BringToTop() override
    {
        // Always re-assert: even if we believe we're topmost, other apps can
        // bump us with their own SetWindowPos. Re-applying is cheap.
        if (!IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        is_topmost_ = true;
    }

    void ReleaseTopmost() override
    {
        if (!is_topmost_) return;
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        is_topmost_ = false;
    }

    void MultiDisplayNudge() override
    {
        RECT r{};
        if (!GetWindowRect(hwnd_, &r)) return;
        SetWindowPos(hwnd_, nullptr, r.left + 1, r.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    bool ShouldClose() const override
    {
        return GetPropW(hwnd_, L"vrto3d_close") != nullptr;
    }

private:
    HWND     hwnd_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
    bool     is_topmost_ = false;
};

}  // namespace


std::unique_ptr<PresentWindow> CreatePresentWindow(const MonitorInfo& primary,
                                                   const MonitorInfo* secondary_for_multi_display,
                                                   const char* title,
                                                   bool start_hidden)
{
    EnsureClass();

    int x = primary.x;
    int y = primary.y;
    uint32_t w = primary.width;
    uint32_t h = primary.height;

    if (secondary_for_multi_display && secondary_for_multi_display->width > 0) {
        w = primary.width + secondary_for_multi_display->width;
    }

    std::wstring wtitle;
    if (title) {
        int n = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
        wtitle.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle.data(), n);
    } else {
        wtitle = L"VRto3D";
    }

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW,
                                 kWndClassName, wtitle.c_str(),
                                 WS_POPUP | (start_hidden ? 0 : WS_VISIBLE),
                                 x, y, static_cast<int>(w), static_cast<int>(h),
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        LOG() << "CreateWindowExW failed; last-error=" << GetLastError();
        return nullptr;
    }
    // Window starts non-topmost. WindowPresenter::FocusThreadLoop asserts
    // HWND_TOPMOST via BringToTop() when the focus flags request it
    // (Ctrl+F8 toggle, auto_focus on tracked-app launch, UE3D IPC).
    // WS_EX_APPWINDOW keeps us in the alt-tab list.
    if (!start_hidden) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    LOG() << "Win32: present window created hwnd=" << hwnd
          << " rect=(" << x << "," << y << " " << w << "x" << h << ")"
          << (start_hidden ? " hidden until first frame" : "");
    return std::make_unique<Win32PresentWindow>(hwnd, w, h);
}


void EnablePerMonitorV2DpiAwareness()
{
    using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto set_ctx = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) {
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
}


bool IsProcessRunning(uint32_t pid)
{
    if (pid == 0) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    DWORD wait = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return wait == WAIT_TIMEOUT;
}

}  // namespace platform
