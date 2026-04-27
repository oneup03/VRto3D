/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef _WIN32

#include "nvstereo_dx9_presenter.h"

#include <chrono>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"

using Microsoft::WRL::ComPtr;

namespace vrto3d {

namespace {

// NV3D packed-stereo signature stored in the (H)th (extra) row of the packed
// surface. Driver scans for this signature on PresentEx and routes left/right
// halves of the surface to alternate eyes.
constexpr DWORD kNvStereoSignature = 0x4433564eu;   // 'N','V','3','D'

#pragma pack(push, 1)
struct NvStereoImageHeader {
    DWORD signature;
    DWORD width;     // per-eye width
    DWORD height;
    DWORD bpp;       // bits per pixel
    DWORD flags;     // 0 = normal, 1 = swap eyes
};
#pragma pack(pop)


// Force `hwnd` to be the foreground/focus window by attaching this thread's
// input queue to the current foreground thread for the duration of the call.
// vrserver typically isn't allowed to call SetForegroundWindow directly
// (Windows foreground-lock), but the AttachThreadInput trick bypasses it.
// FSE D3D9Ex requires the device window to be the focus window.
void ForceForeground(HWND hwnd)
{
    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, &fg_pid) : 0;
    DWORD my_tid = GetCurrentThreadId();

    if (fg_tid && fg_tid != my_tid) {
        AttachThreadInput(my_tid, fg_tid, TRUE);
    }
    AllowSetForegroundWindow(GetCurrentProcessId());
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (fg_tid && fg_tid != my_tid) {
        AttachThreadInput(my_tid, fg_tid, FALSE);
    }
}


// Wraps NvAPI_Initialize in SEH so a missing nvapi64.dll (delay-load failure
// on AMD/Intel boxes) returns a clean error instead of crashing vrserver.
NvAPI_Status TryNvAPIInitializeSEH(bool* dll_failure)
{
    *dll_failure = false;
    __try {
        return NvAPI_Initialize();
    } __except (
        (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
         GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH
    ) {
        *dll_failure = true;
        return NVAPI_LIBRARY_NOT_FOUND;
    }
}

}  // namespace


// ---------------------------------------------------------------------------
// FSE WndProc subclass — swallows WM_ACTIVATE(WA_INACTIVE),
// WM_ACTIVATEAPP(FALSE), and WM_KILLFOCUS so that DefWindowProc never
// triggers the minimize-on-deactivate behavior for our FSE D3D9Ex window.
// This mirrors the XR3DV GameWndSubclassProc approach.
// ---------------------------------------------------------------------------
LRESULT CALLBACK NvStereoDx9Presenter::FseSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<NvStereoDx9Presenter*>(
        GetWindowLongPtrW(hw, GWLP_USERDATA));
    WNDPROC orig = self ? self->orig_wndproc_ : DefWindowProcW;

    // When suppress_minimize_ is true (default / "on-top" active), swallow
    // deactivation messages so the FSE window never minimizes.  When false
    // (user toggled on-top off), let everything through — the FSE window
    // will minimize normally when it loses focus.
    const bool suppress = self && self->suppress_minimize_.load(std::memory_order_relaxed);

    if (suppress) {
        switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE)
                return 0;
            break;
        case WM_ACTIVATEAPP:
            if (!wp)
                return 0;
            break;
        case WM_KILLFOCUS:
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE ||
                (wp & 0xFFF0) == SC_SCREENSAVE ||
                (wp & 0xFFF0) == SC_MONITORPOWER)
                return 0;
            break;
        }
    }
    return CallWindowProcW(orig, hw, msg, wp, lp);
}


void NvStereoDx9Presenter::InstallFseSubclass(HWND hwnd)
{
    if (!hwnd || subclassed_hwnd_) return;

    // Store our this pointer in the window's GWLP_USERDATA so the static
    // callback can retrieve it.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    orig_wndproc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(FseSubclassProc)));
    if (orig_wndproc_) {
        subclassed_hwnd_ = hwnd;
        LOG() << "NvStereoDx9Presenter: FSE WndProc subclass installed";
    } else {
        LOG() << "NvStereoDx9Presenter: SetWindowLongPtrW failed GLE="
              << GetLastError() << " — FSE may minimize on focus loss";
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }

    // Make the FSE window transparent to mouse input (click-through).
    // WS_EX_TRANSPARENT tells the window manager's input routing to skip
    // this window and deliver mouse events to whatever is behind it.
    // WS_EX_LAYERED is required for WS_EX_TRANSPARENT to be effective on
    // top-level windows.
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      ex_style | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    // SetLayeredWindowAttributes is required after adding WS_EX_LAYERED.
    // Alpha=255 means fully opaque — the visual output is unchanged but
    // the window now participates in the layered-window input routing.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    LOG() << "NvStereoDx9Presenter: WS_EX_TRANSPARENT + WS_EX_LAYERED set for click-through";
}


void NvStereoDx9Presenter::RemoveFseSubclass()
{
    if (!subclassed_hwnd_ || !orig_wndproc_) return;

    // Remove WS_EX_TRANSPARENT + WS_EX_LAYERED FIRST, then pump messages
    // so the DWM processes the style change before any D3D9 teardown.
    // Without this settle step the DWM's internal compositing state for this
    // window can race with the D3D9 device release and freeze the display.
    LONG_PTR ex_style = GetWindowLongPtrW(subclassed_hwnd_, GWL_EXSTYLE);
    if (ex_style & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) {
        SetWindowLongPtrW(subclassed_hwnd_, GWL_EXSTYLE,
                          ex_style & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT));
        LOG() << "NvStereoDx9Presenter: WS_EX_LAYERED | WS_EX_TRANSPARENT removed";
        // Pump pending messages so the DWM/compositor sees the style change.
        MSG msg;
        for (int i = 0; i < 5; ++i) {
            while (PeekMessageW(&msg, subclassed_hwnd_, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(500);
        }
        LOG() << "NvStereoDx9Presenter: DWM settle complete after style removal";
    }

    SetWindowLongPtrW(subclassed_hwnd_, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(orig_wndproc_));
    Sleep(1000);  
    SetWindowLongPtrW(subclassed_hwnd_, GWLP_USERDATA, 0);
    Sleep(1000);  
    LOG() << "NvStereoDx9Presenter: FSE WndProc subclass removed";
    orig_wndproc_    = nullptr;
    subclassed_hwnd_ = nullptr;
}


bool NvStereoDx9Presenter::Init(Dx11Renderer& renderer,
                                 const StereoDisplayDriverConfiguration& cfg,
                                 const FocusContext& focus)
{
    renderer_   = &renderer;
    eye_swap_   = cfg.eye_swap;
    auto_focus_ = cfg.auto_focus;
    focus_      = focus;

    // 3D Vision is single-monitor by definition.
    platform::MonitorInfo primary{}, secondary{};
    if (!platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary)) {
        LOG() << "NvStereoDx9Presenter::Init: ResolveTargetMonitors failed";
        return false;
    }

    window_stop_.store(false);
    window_ready_.store(false);
    window_failed_.store(false);
    window_thread_ = std::thread(&NvStereoDx9Presenter::WindowThreadLoop, this, &renderer, primary);

    for (int i = 0; i < 500; ++i) {
        if (window_ready_.load() || window_failed_.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (window_failed_.load() || !window_ready_.load()) {
        LOG() << "NvStereoDx9Presenter::Init: window thread failed to come up";
        Shutdown();
        return false;
    }

    LOG() << "NvStereoDx9Presenter: ready, target_monitor=" << primary.device_name
          << " " << primary.width << "x" << primary.height << "@" << primary.refresh_hz << "Hz";

    focus_stop_.store(false);
    focus_thread_ = std::thread(&NvStereoDx9Presenter::FocusThreadLoop, this);
    return true;
}


void NvStereoDx9Presenter::WindowThreadLoop(Dx11Renderer* renderer, platform::MonitorInfo primary)
{
    monitor_w_ = primary.width;
    monitor_h_ = primary.height;

    window_ = platform::CreatePresentWindow(primary, nullptr, 0, "VRto3D-3DVision");
    if (!window_) {
        LOG() << "NvStereoDx9Presenter: CreatePresentWindow failed";
        window_failed_.store(true);
        return;
    }
    HWND hwnd = static_cast<HWND>(window_->NativeHandle());

    // 1. NvAPI_Initialize — guarded by SEH so missing nvapi64.dll = clean fail
    {
        bool dll_missing = false;
        NvAPI_Status s = TryNvAPIInitializeSEH(&dll_missing);
        if (dll_missing) {
            LOG() << "NvStereoDx9Presenter: nvapi64.dll not resolvable. "
                  << "Install NVIDIA drivers, or pick a different output_mode.";
            window_failed_.store(true);
            window_.reset();
            return;
        }
        if (s != NVAPI_OK) {
            LOG() << "NvStereoDx9Presenter: NvAPI_Initialize failed code=" << static_cast<int>(s);
            window_failed_.store(true);
            window_.reset();
            return;
        }
    }

    // 2. NvAPI_Stereo_IsEnabled — confirm 3D Vision is enabled in NVCP
    {
        NvU8 enabled = 0;
        NvAPI_Status s = NvAPI_Stereo_IsEnabled(&enabled);
        if (s != NVAPI_OK || !enabled) {
            LOG() << "NvStereoDx9Presenter: 3D Vision is not enabled in NVCP. "
                  << "Enable it in NVIDIA Control Panel under 'Set up stereoscopic 3D'.";
            NvAPI_Unload();
            window_failed_.store(true);
            window_.reset();
            return;
        }
    }

    // (driver-version check removed — patched drivers expose 3D Vision again)

    // 3-10. Build D3D9Ex device + packed surfaces + activate stereo
    if (!BuildD3D9Stack(hwnd, primary.width, primary.height, primary.refresh_hz)) {
        LOG() << "NvStereoDx9Presenter: BuildD3D9Stack failed";
        NvAPI_Unload();
        window_failed_.store(true);
        window_.reset();
        return;
    }

    // Stereo activation retry: NvAPI_Stereo_Activate sometimes needs a dummy
    // PresentEx cycle before it sticks (matches XR3DV behavior).
    StereoActivationRetry();

    // If still not activated, arm a deferred-retry loop that fires inside
    // PresentFrame for the first ~60 real-content frames. Driver activation
    // can need several real present cycles before the IR emitter wakes up.
    if (!stereo_activated_) activation_retries_left_ = 60;

    window_ready_.store(true);
    LOG() << "NvStereoDx9Presenter: D3D9Ex + NVAPI ready, stereo_activated="
          << (stereo_activated_ ? "yes" : "no")
          << (activation_retries_left_ > 0 ? " (deferred retries armed)" : "");

    while (!window_stop_.load(std::memory_order_relaxed)) {
        renderer->WaitAndDrawPending(33);
        if (window_) window_->PollEvents();
    }

    // Tear down on this thread.
    LOG() << "NvStereoDx9Presenter: teardown step 1 — remove FSE WndProc subclass";
    RemoveFseSubclass();
    LOG() << "NvStereoDx9Presenter: teardown step 2 — TerminateProcess (hard exit)";
    TerminateProcess(GetCurrentProcess(), 0);
    LOG() << "NvStereoDx9Presenter: window thread exited";
}


bool NvStereoDx9Presenter::BuildD3D9Stack(HWND hwnd, uint32_t monitor_w, uint32_t monitor_h, float refresh_hz)
{
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9_);
    if (FAILED(hr) || !d3d9_) {
        LOG() << "NvStereoDx9Presenter: Direct3DCreate9Ex failed hr=0x" << std::hex << hr;
        return false;
    }

    // Pick the D3D9 adapter that corresponds to the HMONITOR our window is on.
    // CreateDeviceEx(FULLSCREEN) on the wrong adapter index returns D3DERR_INVALIDCALL.
    UINT adapter = D3DADAPTER_DEFAULT;
    {
        HMONITOR target = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        const UINT count = d3d9_->GetAdapterCount();
        for (UINT i = 0; i < count; ++i) {
            if (d3d9_->GetAdapterMonitor(i) == target) { adapter = i; break; }
        }
    }

    // Use the adapter's actual current display mode for back-buffer params.
    // FSE CreateDeviceEx is picky about non-matching values — using the
    // current desktop mode is the safest path for a virtual presenter.
    D3DDISPLAYMODE dm{};
    if (FAILED(d3d9_->GetAdapterDisplayMode(adapter, &dm))) {
        dm.Width  = monitor_w;
        dm.Height = monitor_h;
        dm.Format = D3DFMT_X8R8G8B8;
        dm.RefreshRate = (refresh_hz > 1.0f) ? static_cast<UINT>(refresh_hz + 0.5f) : 0;
    }

    auto fill_pp = [&](D3DPRESENT_PARAMETERS& pp, BOOL windowed) {
        pp = D3DPRESENT_PARAMETERS{};
        pp.BackBufferWidth        = dm.Width;
        pp.BackBufferHeight       = dm.Height;
        pp.BackBufferFormat       = dm.Format;
        pp.BackBufferCount        = 2;        // matches XR3DV; D3D9 prefers 2 for FSE
        pp.MultiSampleType        = D3DMULTISAMPLE_NONE;
        pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow          = hwnd;
        pp.Windowed               = windowed;
        pp.EnableAutoDepthStencil = FALSE;
        pp.Flags                  = 0;
        pp.FullScreen_RefreshRateInHz = windowed ? 0 : dm.RefreshRate;
        pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
    };

    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING
                | D3DCREATE_MULTITHREADED
                | D3DCREATE_FPU_PRESERVE;   // XR3DV uses this; avoids host FPU state surprises

    // Validate the requested mode actually exists for this adapter — if not,
    // FSE CreateDeviceEx is guaranteed to fail with INVALIDCALL.
    {
        bool mode_found = false;
        UINT mode_count = d3d9_->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8);
        for (UINT i = 0; i < mode_count; ++i) {
            D3DDISPLAYMODE m{};
            if (SUCCEEDED(d3d9_->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, i, &m))
                && m.Width == dm.Width && m.Height == dm.Height && m.RefreshRate == dm.RefreshRate) {
                mode_found = true;
                break;
            }
        }
        if (!mode_found) {
            LOG() << "NvStereoDx9Presenter: WARNING adapter " << adapter
                  << " does not advertise mode " << dm.Width << "x" << dm.Height
                  << "@" << dm.RefreshRate << "Hz — FSE may refuse";
        }
    }

    // Force our window to the foreground / focus before FSE CreateDeviceEx.
    // vrserver isn't normally allowed to do this; the AttachThreadInput
    // trick inside ForceForeground bypasses Windows' foreground-lock.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0,
                 static_cast<int>(dm.Width), static_cast<int>(dm.Height),
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    ForceForeground(hwnd);
    // Pump messages 3× so any cross-thread sync messages from the foreground
    // transition complete before CreateDeviceEx.
    {
        MSG msg;
        for (int i = 0; i < 3; ++i) {
            Sleep(20);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    // Try fullscreen-exclusive first (3D Vision activation is most reliable here).
    // CRITICAL: D3D9Ex FSE requires a non-null D3DDISPLAYMODEEX as the 5th arg —
    // passing nullptr is an automatic D3DERR_INVALIDCALL.
    D3DPRESENT_PARAMETERS pp{};
    fill_pp(pp, FALSE);
    D3DDISPLAYMODEEX fs_mode{};
    fs_mode.Size            = sizeof(fs_mode);
    fs_mode.Width           = dm.Width;
    fs_mode.Height          = dm.Height;
    fs_mode.RefreshRate     = dm.RefreshRate;
    fs_mode.Format          = D3DFMT_X8R8G8B8;
    fs_mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

    hr = d3d9_->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, hwnd,
                                flags, &pp, &fs_mode, &device9_);
    if (FAILED(hr) || !device9_) {
        LOG() << "NvStereoDx9Presenter: CreateDeviceEx FSE failed hr=0x" << std::hex << hr
              << " adapter=" << std::dec << adapter
              << " mode=" << dm.Width << "x" << dm.Height << "@" << dm.RefreshRate << "Hz fmt=" << dm.Format
              << "; falling back to windowed";

        // Fallback: windowed device. Lets the path be smoke-tested even if FSE
        // is denied. Stereo activation in windowed mode requires NVCP-side
        // settings that may or may not be present.
        device9_.Reset();
        fill_pp(pp, TRUE);
        hr = d3d9_->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, hwnd,
                                    flags, &pp, nullptr, &device9_);
        if (FAILED(hr) || !device9_) {
            LOG() << "NvStereoDx9Presenter: CreateDeviceEx WINDOWED also failed hr=0x"
                  << std::hex << hr;
            return false;
        }
        LOG() << "NvStereoDx9Presenter: D3D9Ex device created (windowed fallback)";
        is_fse_ = false;
    } else {
        LOG() << "NvStereoDx9Presenter: D3D9Ex device created (FSE) adapter=" << adapter
              << " " << dm.Width << "x" << dm.Height << "@" << dm.RefreshRate << "Hz";
        is_fse_ = true;
        // Install WndProc subclass to swallow deactivation messages so the
        // FSE window never minimizes when the user interacts with another display.
        InstallFseSubclass(hwnd);
    }

    // Globally make sure stereo is enabled (idempotent — no-op if already on).
    NvAPI_Stereo_Enable();

    NvAPI_Status s = NvAPI_Stereo_CreateHandleFromIUnknown(device9_.Get(), &stereo_handle_);
    if (s != NVAPI_OK || !stereo_handle_) {
        LOG() << "NvStereoDx9Presenter: NvAPI_Stereo_CreateHandleFromIUnknown failed code=" << static_cast<int>(s);
        return false;
    }

    // Tell the driver to flag every render target / back buffer this device
    // creates as stereo-capable. Required when the device wasn't created in
    // FSE on a 3D Vision panel — without this, the back buffer is mono and
    // the NV3D-signed packed surface just blits as plain SbS pixels.
    NvAPI_Stereo_SetSurfaceCreationMode(stereo_handle_, NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);

    s = NvAPI_Stereo_Activate(stereo_handle_);
    {
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(stereo_handle_, &active);
        stereo_activated_ = (active != 0);

        float sep = 0.0f, conv = 0.0f;
        NvAPI_Stereo_GetSeparation(stereo_handle_, &sep);
        NvAPI_Stereo_GetConvergence(stereo_handle_, &conv);

        LOG() << "NvStereoDx9Presenter: NvAPI_Stereo_Activate code=" << static_cast<int>(s)
              << " IsActivated=" << (stereo_activated_ ? "yes" : "no (retry scheduled)")
              << " separation=" << sep << "% convergence=" << conv;
    }

    hr = device9_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer_);
    if (FAILED(hr) || !back_buffer_) {
        LOG() << "NvStereoDx9Presenter: GetBackBuffer failed hr=0x" << std::hex << hr;
        return false;
    }

    return true;
}


bool NvStereoDx9Presenter::EnsurePackedSurfaces(uint32_t input_w_per_eye, uint32_t input_h)
{
    const uint32_t pw = input_w_per_eye * 2u;
    const uint32_t ph = input_h + 1u;

    if (packed_sysmem_ && packed_w_ == pw && packed_h_ == input_h) return true;

    packed_default_.Reset();
    packed_sysmem_.Reset();

    HRESULT hr = device9_->CreateOffscreenPlainSurface(
        pw, ph, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &packed_sysmem_, nullptr);
    if (FAILED(hr)) {
        LOG() << "NvStereoDx9Presenter: CreateOffscreenPlainSurface(SYSMEM) failed hr=0x" << std::hex << hr;
        return false;
    }
    hr = device9_->CreateOffscreenPlainSurface(
        pw, ph, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &packed_default_, nullptr);
    if (FAILED(hr)) {
        LOG() << "NvStereoDx9Presenter: CreateOffscreenPlainSurface(DEFAULT) failed hr=0x" << std::hex << hr;
        packed_sysmem_.Reset();
        return false;
    }

    packed_w_ = pw;
    packed_h_ = input_h;
    LOG() << "NvStereoDx9Presenter: packed surfaces " << pw << "x" << ph
          << " (per-eye " << input_w_per_eye << "x" << input_h << ")";
    return true;
}


void NvStereoDx9Presenter::StereoActivationRetry()
{
    if (stereo_activated_ || !device9_ || !stereo_handle_) return;

    // One clear+present so the driver sees real device activity, then retry.
    device9_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    device9_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    Sleep(50);
    NvAPI_Stereo_Activate(stereo_handle_);

    NvU8 active = 0;
    NvAPI_Stereo_IsActivated(stereo_handle_, &active);
    stereo_activated_ = (active != 0);
    LOG() << "NvStereoDx9Presenter: stereo activation retry -> "
          << (stereo_activated_ ? "ACTIVE" : "still inactive (packed-surface fallback handles it)");
}


void NvStereoDx9Presenter::PresentFrame(ID3D11Texture2D* sbs_input)
{
    if (!device9_ || !sbs_input || !renderer_) return;

    ID3D11Device*        dev = renderer_->Device();
    ID3D11DeviceContext* ctx = renderer_->Context();

    D3D11_TEXTURE2D_DESC td{};
    sbs_input->GetDesc(&td);

    // Recreate staging on dimension/format change.
    if (!staging_ || staging_w_ != td.Width || staging_h_ != td.Height || staging_fmt_ != td.Format) {
        staging_.Reset();
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.BindFlags      = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags      = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging_))) {
            static std::atomic<bool> logged{false};
            bool e = false;
            if (logged.compare_exchange_strong(e, true))
                LOG() << "NvStereoDx9Presenter: CreateTexture2D(staging) failed";
            return;
        }
        staging_w_   = td.Width;
        staging_h_   = td.Height;
        staging_fmt_ = td.Format;
    }

    // Copy compositor SBS texture → CPU-readable staging.
    ctx->CopyResource(staging_.Get(), sbs_input);

    // Recreate D3D9 packed surfaces if input dims changed. td.Width is 2*per_eye.
    const uint32_t per_eye_w = td.Width / 2u;
    if (!EnsurePackedSurfaces(per_eye_w, td.Height)) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

    // Lock the D3D9 sysmem packed surface and copy rows. D3D11 texture is
    // R8G8B8A8 (RGBA byte order); D3D9 X8R8G8B8 stores BGRA. Swap channels
    // per-pixel during the copy.
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(packed_sysmem_->LockRect(&lr, nullptr, 0))) {
        const uint32_t copy_h = td.Height;
        for (uint32_t y = 0; y < copy_h; ++y) {
            const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            uint8_t*       dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
            // Per-pixel swap r<->b. RowBytes = packed_w_ * 4.
            const uint32_t row_bytes = packed_w_ * 4u;
            for (uint32_t x = 0; x < row_bytes; x += 4) {
                dst[x + 0] = src[x + 2];   // B <- src.B (offset 2 in RGBA)
                dst[x + 1] = src[x + 1];   // G
                dst[x + 2] = src[x + 0];   // R <- src.R (offset 0)
                dst[x + 3] = 0xFF;
            }
        }
        // Stamp the NV3D signature into row index td.Height (the extra row).
        // The compositor lays out our SbS input as [left | right]; NVIDIA's
        // 3D Vision driver routes the left half of the packed surface to the
        // right eye by default for our pipeline, so we flip the swap-eyes bit
        // by default. eye_swap_ in the config returns it to non-swapped.
        NvStereoImageHeader hdr{};
        hdr.signature = kNvStereoSignature;
        hdr.width     = per_eye_w;
        hdr.height    = td.Height;
        hdr.bpp       = 32;
        hdr.flags     = eye_swap_ ? 0u : 1u;
        uint8_t* hdr_row = static_cast<uint8_t*>(lr.pBits) + td.Height * lr.Pitch;
        std::memcpy(hdr_row, &hdr, sizeof(hdr));

        packed_sysmem_->UnlockRect();
    }
    ctx->Unmap(staging_.Get(), 0);

    // Push to GPU.
    HRESULT hr = device9_->UpdateSurface(packed_sysmem_.Get(), nullptr,
                                          packed_default_.Get(), nullptr);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: UpdateSurface failed hr=0x" << std::hex << hr;
        return;
    }

    // Stretch packed (excluding header row) to back buffer.
    RECT src{ 0, 0, static_cast<LONG>(packed_w_), static_cast<LONG>(packed_h_) };
    hr = device9_->StretchRect(packed_default_.Get(), &src,
                                back_buffer_.Get(), nullptr, D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: StretchRect failed hr=0x" << std::hex << hr;
        return;
    }

    hr = device9_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: PresentEx failed hr=0x" << std::hex << hr;
    }

    // Deferred stereo activation: keep retrying after each real present until
    // the driver's IR emitter wakes up or we run out of attempts.
    if (!stereo_activated_ && activation_retries_left_ > 0 && stereo_handle_) {
        --activation_retries_left_;
        NvAPI_Stereo_Activate(stereo_handle_);
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(stereo_handle_, &active);
        if (active) {
            stereo_activated_ = true;
            LOG() << "NvStereoDx9Presenter: stereo activated after "
                  << (60 - activation_retries_left_) << " present cycle(s)";
        } else if (activation_retries_left_ == 0 && !activation_summary_logged_) {
            activation_summary_logged_ = true;
            LOG() << "NvStereoDx9Presenter: 3D Vision did NOT engage after 60 "
                     "present cycles. Frames are being delivered as a mono SbS "
                     "image. Requirements:";
            LOG() << "  1) NVIDIA 3D Vision driver installed (driver <= 425.31 - "
                     "newer drivers dropped 3D Vision support on most SKUs).";
            LOG() << "  2) NVIDIA Control Panel > 'Set up stereoscopic 3D' > "
                     "enabled, with a paired IR emitter + glasses.";
            LOG() << "  3) Target display is 3D Vision-certified (typically a "
                     "120Hz panel/projector). Set 'display_index' to that monitor.";
            LOG() << "  4) Best results with the 3D display set as the Windows "
                     "primary (enables FSE D3D9Ex from vrserver - currently "
                     "we are in windowed fallback).";
        }
    }
}


void NvStereoDx9Presenter::FocusThreadLoop()
{
    using namespace std::chrono_literals;
    bool was_on_top = false;
    int  reassert_counter = 0;
    uint32_t last_auto_focused_pid = 0;
    uint32_t last_ue3d_focused_pid = 0;

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        if (!window_) break;

        const bool is_on_top   = focus_.is_on_top   && focus_.is_on_top->load();
        const bool man_on_top  = focus_.man_on_top  && focus_.man_on_top->load();
        const bool ue3d_on_top = focus_.ue3d_on_top && focus_.ue3d_on_top->load();
        const uint32_t pid     = focus_.app_pid ? focus_.app_pid->load() : 0;

        const bool app_running = platform::IsProcessRunning(pid);
        if (pid == 0 || !app_running) {
            last_auto_focused_pid = 0;
            last_ue3d_focused_pid = 0;
        }

        // FSE mode: D3D9 FSE bypasses the normal window Z-order, so
        // HWND_TOPMOST / HWND_NOTOPMOST has no visible effect.  Instead
        // we toggle suppress_minimize_: when true the WndProc subclass
        // swallows WM_ACTIVATE(WA_INACTIVE) etc. so the FSE window stays
        // put; when false those messages pass through and the window can
        // minimize normally, letting the user interact with the desktop or
        // another display.
        //
        // To bring the FSE window back after a minimize we call
        // ShowWindow(SW_RESTORE) + ForceForeground.
        if (is_fse_) {
            bool want_on_top = false;
            if (man_on_top || is_on_top || ue3d_on_top) {
                want_on_top = true;
            } else if (auto_focus_ && app_running && pid != 0
                       && pid != last_auto_focused_pid) {
                if (focus_.is_on_top)  focus_.is_on_top->store(true);
                if (focus_.man_on_top) focus_.man_on_top->store(true);
                last_auto_focused_pid = pid;
                want_on_top = true;
            }

            // Update the suppress flag so the WndProc knows what to do.
            suppress_minimize_.store(want_on_top, std::memory_order_relaxed);

            HWND hwnd = static_cast<HWND>(window_->NativeHandle());

            if (want_on_top && !was_on_top) {
                // Transition OFF → ON: restore and foreground the FSE window.
                if (hwnd) {
                    ShowWindow(hwnd, SW_RESTORE);
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    ForceForeground(hwnd);
                }
                was_on_top = true;
                reassert_counter = 0;
            } else if (!want_on_top && was_on_top) {
                // Transition ON → OFF: actively minimize the FSE window.
                // Just clearing suppress_minimize_ isn't enough — D3D9 FSE
                // won't spontaneously receive deactivation messages, so we
                // must explicitly minimize it.
                if (hwnd) {
                    ShowWindow(hwnd, SW_MINIMIZE);
                }
                was_on_top = false;
                reassert_counter = 0;
            } else if (want_on_top && ++reassert_counter >= 10) {
                // Periodically reassert foreground while on-top is active.
                reassert_counter = 0;
                if (hwnd) {
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }

            std::this_thread::sleep_for(50ms);
            continue;
        }

        // Windowed fallback: standard focus logic (same as other presenters).
        bool want_on_top = false;
        if (man_on_top) {
            want_on_top = true;
        } else if (is_on_top && app_running) {
            want_on_top = true;
        } else if (auto_focus_ && !is_on_top && !ue3d_on_top
                   && app_running && pid != 0
                   && pid != last_auto_focused_pid) {
            if (focus_.is_on_top)  focus_.is_on_top->store(true);
            if (focus_.man_on_top) focus_.man_on_top->store(true);
            last_auto_focused_pid = pid;
            want_on_top = true;
        } else if (ue3d_on_top && pid != 0 && pid != last_ue3d_focused_pid) {
            last_ue3d_focused_pid = pid;
            want_on_top = true;
        }

        if (want_on_top != was_on_top) {
            if (want_on_top) window_->BringToTop();
            else             window_->ReleaseTopmost();
            was_on_top = want_on_top;
            reassert_counter = 0;
        } else if (want_on_top && ++reassert_counter >= 20) {
            reassert_counter = 0;
            window_->BringToTop();
        }

        std::this_thread::sleep_for(50ms);
    }
}


void NvStereoDx9Presenter::Shutdown()
{
    if (!window_thread_.joinable() && !window_) return;
    LOG() << "NvStereoDx9Presenter: Shutdown called";

    focus_stop_.store(true);
    if (focus_thread_.joinable()) focus_thread_.join();

    window_stop_.store(true);
    if (window_thread_.joinable()) window_thread_.join();

    renderer_ = nullptr;
}

}  // namespace vrto3d

#endif  // _WIN32
