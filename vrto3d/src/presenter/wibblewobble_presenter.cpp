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

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// Vendored IPC contract — byte-compatible with WWVROU3's WibbleWobbleCapture
// (`#pragma pack(push, 0)` is a no-op in MSVC and preserves default alignment,
// matching the producer/consumer the WibbleWobbleClient process compiles with).
// Source: WWVROU3/WibbleWobbleCapture/WibbleWobbleCapture/{IWibbleWobbleCapture,WibbleWobbleCapture}.h
// ---------------------------------------------------------------------------

constexpr int WW_CAPTURE_FRAME_COUNT = 3;

constexpr const char* k_capture_data_name       = "WW_CAPTURE_DATA";
constexpr const char* k_capture_data_mutex_name = "WW_CAPTURE_DATA_MUTEX";
constexpr const char* k_frame_ready_event_name  = "WibbleWobbleFrameReady";

enum WWCaptureFrameState {
    WWCaptureFrameState_ReadyForServerCPU,
    WWCaptureFrameState_ReadyForServerGPUCopy,
    WWCaptureFrameState_ReadyForClientGPUCopy,
};

enum WWCaptureDataState {
    WWCaptureDataState_ServerInit,
    WWCaptureDataState_ClientInit,
    WWCaptureDataState_ServingOnPresent,
    WWCaptureDataState_ServingCPU,
};

enum WWSourceFormat {
    WWSF_Single,
    WWSF_SideBySideHalf,
    WWSF_Checkerboard,
    WWSF_SideBySideFull,
};

enum WibbleWobbleCaptureType {
    WWCT_Reshade,
    WWCT_OpenVR,
};

enum WWFrameSelectionAlgorithm {
    WWFSA_BufferOldest,
    WWFSA_BufferNewest,
    WWFSA_SingleNewest,
};

// Producer (WWVROU3) writes these structs under `#pragma pack(push, 0)`.
// MSVC interprets `pack(0)` as "use default" (8 on x64) and warns C4086 if we
// pass 0 literally, so we use the resolved value explicitly.
#pragma pack(push, 8)
struct WWCaptureTrackData {
    int   m_version = 0;
    float m_headVehicle[3][4];
    float m_headWorld[3][4];
    float m_vFov = 120.0f;
    float m_hFov = 120.0f;
    float m_worldScale = 1.0f;
    float m_headVehicleYaw;
    float m_headVehiclePitch;
    float m_headVehicleRoll;
};

struct WWCaptureFrame {
    WWCaptureFrameState m_state = WWCaptureFrameState_ReadyForServerGPUCopy;
    uint64_t            m_textureResource = 0;
    int                 m_framecounter = 0;
    float               m_copytime = 0.0f;
    WWCaptureTrackData  m_trackData;
};

struct WWCaptureData {
    WWCaptureDataState        m_state = WWCaptureDataState_ServerInit;
    unsigned int              m_serverProcessId = 0;
    WWCaptureFrame            m_frames[WW_CAPTURE_FRAME_COUNT];
    int                       m_width;
    int                       m_height;
    int                       m_format;
    int                       m_samples;
    WWSourceFormat            m_sourceFormat = WWSF_Single;
    int                       m_enableEffects = 0;
    WibbleWobbleCaptureType   m_captureType = WWCT_Reshade;
    int                       m_frameCaptureDelay = 0;
    int                       m_captureDeviceCount = 0;
    int                       m_selectedCaptureDevice = 0;
    WWFrameSelectionAlgorithm m_frameSelectionAlgorithm = WWFSA_BufferOldest;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Win32 IPC primitives
// ---------------------------------------------------------------------------

struct NamedSharedMem {
    HANDLE        mapping = nullptr;
    WWCaptureData* view   = nullptr;

    bool Open(const char* name) {
        mapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(WWCaptureData), name);
        if (!mapping) return false;
        view = static_cast<WWCaptureData*>(
            MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WWCaptureData)));
        return view != nullptr;
    }

    void Close() {
        if (view)    { UnmapViewOfFile(view); view = nullptr; }
        if (mapping) { CloseHandle(mapping);  mapping = nullptr; }
    }
};

struct LockGuard {
    HANDLE mutex_;
    LockGuard(HANDLE m) : mutex_(m) {
        if (mutex_) WaitForSingleObject(mutex_, INFINITE);
    }
    ~LockGuard() {
        if (mutex_) ReleaseMutex(mutex_);
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

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
// WibbleWobbleClient.dll C ABI (added to the WWVROU3 project — see
// WibbleWobbleClient/WibbleWobbleClientCApi.h). Resolved at runtime so we
// don't need to link against WibbleWobbleClient.lib or pull in its heavy
// header chain (Unity Plugin / ImGui / WibbleWobbleRender / ...).
// ---------------------------------------------------------------------------
using WWClientHandle = void*;
using PFN_WWClient_Create     = WWClientHandle (__cdecl*)(void* parent_hwnd, uint32_t server_process_id);
using PFN_WWClient_IsRunning  = int            (__cdecl*)(WWClientHandle);
using PFN_WWClient_Destroy    = void           (__cdecl*)(WWClientHandle);

}  // namespace

namespace vrto3d {

// ---------------------------------------------------------------------------
// pImpl — keeps Windows.h and the vendored IPC layout out of the header.
// ---------------------------------------------------------------------------
struct WibbleWobblePresenter::Impl {
    // IPC: still used by the loaded WibbleWobbleClient.dll to receive frame
    // metadata + handles. With the client in-process the named-shared-memory +
    // named mutex collapse into intra-process objects (no kernel transitions
    // for the mutex) but the protocol is unchanged.
    NamedSharedMem  shm;
    HANDLE          mutex        = nullptr;
    HANDLE          frame_ready  = nullptr;

    // In-process WibbleWobbleClient via WibbleWobbleClient.dll C ABI.
    HMODULE                  client_dll      = nullptr;
    WWClientHandle           client_handle   = nullptr;
    PFN_WWClient_Create      pCreate         = nullptr;
    PFN_WWClient_IsRunning   pIsRunning      = nullptr;
    PFN_WWClient_Destroy     pDestroy        = nullptr;
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

    // Required: WibbleWobbleClient install path must be present in the
    // registry (set by the install .bat). Without it we have nothing to
    // launch, so abort driver activation.
    std::wstring install_path = ReadInstallPathFromRegistry();
    if (install_path.empty()) {
        LOG() << "WibbleWobblePresenter: HKLM\\SOFTWARE\\PHARTGAMES\\WibbleWobbleClient\\install_path "
                 "not found — install WibbleWobbleClient first";
        return false;
    }

    impl_ = std::make_unique<Impl>();

    if (!impl_->shm.Open(k_capture_data_name)) {
        LOG() << "WibbleWobblePresenter: CreateFileMapping(WW_CAPTURE_DATA) failed err=" << GetLastError();
        impl_.reset();
        return false;
    }
    impl_->mutex = CreateMutexA(nullptr, FALSE, k_capture_data_mutex_name);
    if (!impl_->mutex) {
        LOG() << "WibbleWobblePresenter: CreateMutex(WW_CAPTURE_DATA_MUTEX) failed err=" << GetLastError();
        impl_->shm.Close();
        impl_.reset();
        return false;
    }
    // Auto-reset, initially non-signaled — same name the client opens.
    impl_->frame_ready = CreateEventA(nullptr, FALSE, FALSE, k_frame_ready_event_name);
    if (!impl_->frame_ready) {
        LOG() << "WibbleWobblePresenter: CreateEvent(WibbleWobbleFrameReady) failed err=" << GetLastError();
        CloseHandle(impl_->mutex); impl_->mutex = nullptr;
        impl_->shm.Close();
        impl_.reset();
        return false;
    }

    // Publish initial server state. Width/height/format are filled in on
    // the first PresentFrame once we know the source texture dimensions.
    {
        LockGuard lock(impl_->mutex);
        WWCaptureData* d = impl_->shm.view;
        d->m_state                   = WWCaptureDataState_ServerInit;
        d->m_serverProcessId         = GetCurrentProcessId();
        d->m_captureType             = WWCT_OpenVR;
        d->m_frameCaptureDelay       = 0;
        // SingleNewest: client always pulls the most recent frame and we
        // bypass the 3-deep ring queue. Minimizes latency for live VR
        // (head-tracking-bound) at the cost of dropping older frames the
        // client hasn't picked up yet — exactly what we want.
        d->m_frameSelectionAlgorithm = WWFSA_SingleNewest;
        d->m_sourceFormat            = WWSF_SideBySideFull;
        for (int i = 0; i < WW_CAPTURE_FRAME_COUNT; ++i) {
            d->m_frames[i].m_state            = WWCaptureFrameState_ReadyForServerGPUCopy;
            d->m_frames[i].m_textureResource  = 0;
            d->m_frames[i].m_framecounter     = 0;
            d->m_frames[i].m_copytime         = 0.0f;
        }
    }

    // Load WibbleWobbleClient.dll in-process. Use SetDllDirectoryW so the
    // dependent WibbleWobble*.dll family (Capture/Common/Render/Track + the
    // Data/ Plugins/ tree) is found from install_path even though our own
    // module lives elsewhere (under SteamVR drivers).
    auto fail = [&](const char* what, DWORD err = ERROR_SUCCESS) {
        LOG() << "WibbleWobblePresenter: " << what
              << (err ? " err=" : "") << (err ? std::to_string(err) : std::string{});
        if (impl_->client_handle && impl_->pDestroy) impl_->pDestroy(impl_->client_handle);
        if (impl_->client_dll)   FreeLibrary(impl_->client_dll);
        if (impl_->frame_ready)  CloseHandle(impl_->frame_ready);
        if (impl_->mutex)        CloseHandle(impl_->mutex);
        impl_->shm.Close();
        impl_.reset();
        return false;
    };

    SetDllDirectoryW(install_path.c_str());
    std::wstring dll_path = install_path + L"WibbleWobbleClient.dll";
    impl_->client_dll = LoadLibraryW(dll_path.c_str());
    SetDllDirectoryW(nullptr);  // restore default search order
    if (!impl_->client_dll) {
        return fail("LoadLibrary(WibbleWobbleClient.dll) failed", GetLastError());
    }

    impl_->pCreate    = reinterpret_cast<PFN_WWClient_Create>   (GetProcAddress(impl_->client_dll, "WWClient_Create"));
    impl_->pIsRunning = reinterpret_cast<PFN_WWClient_IsRunning>(GetProcAddress(impl_->client_dll, "WWClient_IsRunning"));
    impl_->pDestroy   = reinterpret_cast<PFN_WWClient_Destroy>  (GetProcAddress(impl_->client_dll, "WWClient_Destroy"));
    if (!impl_->pCreate || !impl_->pIsRunning || !impl_->pDestroy) {
        return fail("WibbleWobbleClient.dll missing one of WWClient_Create/IsRunning/Destroy — "
                    "rebuild WibbleWobbleClient with the C-API shim");
    }

    // GetCurrentProcessId so the client knows which process to track for
    // window focus/follow logic (matches WWReshadeAddon's call site).
    impl_->client_handle = impl_->pCreate(nullptr, GetCurrentProcessId());
    if (!impl_->client_handle) {
        return fail("WWClient_Create returned null");
    }
    LOG() << "WibbleWobblePresenter: WibbleWobbleClient.dll loaded in-process";

    // Spawn the pump thread that drives WaitAndDrawPending — this is what
    // composites the OSD into out_sbs_ and ultimately calls PresentFrame on us.
    pump_stop_.store(false);
    pump_thread_ = std::thread(&WibbleWobblePresenter::PumpThreadLoop, this);

    // Spawn the focus thread that finds the WibbleWobble window, applies
    // BringToTop logic, and feeds the HWND to the OSD for cursor mapping.
    focus_stop_.store(false);
    focus_thread_ = std::thread(&WibbleWobblePresenter::FocusThreadLoop, this);

    return true;
}

void WibbleWobblePresenter::PumpThreadLoop() {
    while (!pump_stop_.load(std::memory_order_relaxed)) {
        if (renderer_) renderer_->WaitAndDrawPending(33);

        // Drain this thread's Windows message queue. OsdRenderer is lazily
        // initialized on this thread inside WaitAndDrawPending, which causes
        // osd_input_win32 to install a WH_MOUSE_LL hook owned by this thread.
        // Low-level hooks are dispatched via the thread's message queue; if
        // we never pump it, Windows queues events, throttles/times out the
        // hook (causing system-wide cursor jitter), and mouse-button events
        // never reach ImGui (so OSD highlights but won't click).
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void WibbleWobblePresenter::FocusThreadLoop() {
    // The WibbleWobbleClient names its main window "WibbleWobble". Poll for
    // it (it can take a moment to appear after CreateProcess), then mirror
    // the focus pattern from LeiaSrPresenter::FocusThreadLoop:
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
        // (Re)discover the WibbleWobble window if we don't have it or it
        // disappeared (client restart, etc.).
        if (!ww_hwnd || !IsWindow(ww_hwnd)) {
            ww_hwnd = FindWindowW(nullptr, L"WibbleWobble");
            if (ww_hwnd && renderer_) {
                renderer_->SetOsdHeadsetHwnd(ww_hwnd);
                LOG() << "WibbleWobblePresenter: located WibbleWobble window hwnd=0x"
                      << std::hex << reinterpret_cast<uintptr_t>(ww_hwnd);
            }
            was_on_top = false;  // reset state so we re-apply on next loop
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
    if (!impl_ || !sbs_input || !context_) return;

    D3D11_TEXTURE2D_DESC desc{};
    sbs_input->GetDesc(&desc);

    bool needs_publish = (desc.Width != last_published_w_)
                      || (desc.Height != last_published_h_)
                      || (desc.Format != last_published_fmt_);

    // Liveness check on the in-process client. If its worker thread exited,
    // stop the pump so we don't keep consuming GPU bandwidth on dead frames.
    static std::atomic<bool> client_dead_logged{false};
    if (impl_->client_handle && impl_->pIsRunning
     && impl_->pIsRunning(impl_->client_handle) == 0) {
        bool e = false;
        if (client_dead_logged.compare_exchange_strong(e, true)) {
            LOG() << "WibbleWobblePresenter: WibbleWobbleClient worker stopped — "
                     "stopping pump thread (restart SteamVR to retry)";
            pump_stop_.store(true, std::memory_order_relaxed);
        }
        return;
    }

    // Outcomes after the critical section. Process them outside the lock.
    enum class Outcome { Published, WaitingForClient, Copied, NoSlot } outcome = Outcome::WaitingForClient;
    int copy_frame_idx = -1;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dst_tex;

    {
        LockGuard lock(impl_->mutex);
        WWCaptureData* d = impl_->shm.view;

        if (needs_publish || d->m_state == WWCaptureDataState_ServerInit) {
            d->m_width        = static_cast<int>(desc.Width);
            d->m_height       = static_cast<int>(desc.Height);
            d->m_format       = static_cast<int>(desc.Format);
            d->m_samples      = static_cast<int>(desc.SampleDesc.Count);
            d->m_sourceFormat = WWSF_SideBySideFull;
            d->m_state        = WWCaptureDataState_ClientInit;
            last_published_w_   = desc.Width;
            last_published_h_   = desc.Height;
            last_published_fmt_ = desc.Format;
            outcome = Outcome::Published;
        } else if (d->m_state == WWCaptureDataState_ServingOnPresent
                || d->m_state == WWCaptureDataState_ServingCPU) {
            // Pick the destination frame: any slot the client has marked as
            // ReadyForServerGPUCopy. Mirrors SyncServerGPUCopyBufferOldest in
            // WibbleWobbleCapture.h — we sort by m_framecounter descending and
            // apply m_frameCaptureDelay as an index offset.
            int candidates[WW_CAPTURE_FRAME_COUNT];
            int n = 0;
            for (int i = 0; i < WW_CAPTURE_FRAME_COUNT; ++i) {
                if (d->m_frames[i].m_textureResource != 0
                 && d->m_frames[i].m_state == WWCaptureFrameState_ReadyForServerGPUCopy) {
                    candidates[n++] = i;
                }
            }
            if (n > 0) {
                std::sort(candidates, candidates + n,
                          [d](int a, int b) {
                              return d->m_frames[a].m_framecounter > d->m_frames[b].m_framecounter;
                          });
                int sorter_idx = std::min(d->m_frameCaptureDelay, n - 1);
                copy_frame_idx = candidates[sorter_idx];
                WWCaptureFrame& frame = d->m_frames[copy_frame_idx];

                // Cache the opened destination texture per slot. Reopen if
                // the client published a fresh HANDLE (e.g. after a resize).
                Slot& slot = dst_cache_[copy_frame_idx];
                if (slot.handle != frame.m_textureResource || !slot.tex) {
                    slot.tex.Reset();
                    slot.handle = frame.m_textureResource;
                    ID3D11Resource* res = nullptr;
                    HRESULT hr = device_->OpenSharedResource(
                        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(slot.handle)),
                        __uuidof(ID3D11Resource),
                        reinterpret_cast<void**>(&res));
                    if (FAILED(hr) || !res) {
                        LOG() << "WibbleWobblePresenter: OpenSharedResource failed hr=0x"
                              << std::hex << hr;
                        slot.handle = 0;
                        outcome = Outcome::NoSlot;
                    } else {
                        hr = res->QueryInterface(__uuidof(ID3D11Texture2D),
                                                 reinterpret_cast<void**>(slot.tex.GetAddressOf()));
                        res->Release();
                        if (FAILED(hr) || !slot.tex) {
                            LOG() << "WibbleWobblePresenter: QueryInterface(ID3D11Texture2D) failed hr=0x"
                                  << std::hex << hr;
                            slot.handle = 0;
                            outcome = Outcome::NoSlot;
                        }
                    }
                }
                if (slot.tex) {
                    dst_tex = slot.tex;
                    outcome = Outcome::Copied;
                    // Mark slot ready and stamp time INSIDE the lock so the
                    // client can't race ahead reading a stale state.
                    const float now_secs = static_cast<float>(
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    frame.m_state    = WWCaptureFrameState_ReadyForClientGPUCopy;
                    frame.m_copytime = now_secs;
                }
            } else {
                outcome = Outcome::NoSlot;
            }
        }
    }  // mutex released

    if (outcome == Outcome::Published) {
        LOG() << "WibbleWobblePresenter: published metadata "
              << desc.Width << "x" << desc.Height << " fmt=" << desc.Format;
        return;
    }

    if (outcome == Outcome::Copied && dst_tex) {
        // CopyResource + Flush happen outside the cross-process mutex. The
        // dst slot is already marked Ready so the client may begin its own
        // GPU copy immediately on receiving the wake event.
        context_->CopyResource(dst_tex.Get(), sbs_input);
        context_->Flush();
        SetEvent(impl_->frame_ready);
    }
}

void WibbleWobblePresenter::Shutdown() {
    // Stop the pump thread first so it can't call WaitAndDrawPending while
    // we tear down IPC + the launched client process.
    pump_stop_.store(true);
    focus_stop_.store(true);
    if (pump_thread_.joinable())  pump_thread_.join();
    if (focus_thread_.joinable()) focus_thread_.join();
    if (renderer_) renderer_->SetOsdHeadsetHwnd(nullptr);

    if (!impl_) {
        renderer_ = nullptr;
        device_   = nullptr;
        context_  = nullptr;
        return;
    }

    // Tear the in-process client down. WWClient_Destroy joins the worker
    // thread internally (Stop() + delete in the C++ side).
    if (impl_->client_handle && impl_->pDestroy) {
        impl_->pDestroy(impl_->client_handle);
        impl_->client_handle = nullptr;
    }
    if (impl_->client_dll) {
        FreeLibrary(impl_->client_dll);
        impl_->client_dll = nullptr;
    }

    if (impl_->shm.view && impl_->mutex) {
        LockGuard lock(impl_->mutex);
        impl_->shm.view->m_state           = WWCaptureDataState_ServerInit;
        impl_->shm.view->m_serverProcessId = 0;
    }

    if (impl_->frame_ready) { CloseHandle(impl_->frame_ready); impl_->frame_ready = nullptr; }
    if (impl_->mutex)       { CloseHandle(impl_->mutex);       impl_->mutex       = nullptr; }
    impl_->shm.Close();

    for (auto& s : dst_cache_) { s.tex.Reset(); s.handle = 0; }

    impl_.reset();
    renderer_ = nullptr;
    device_   = nullptr;
    context_  = nullptr;
}

}  // namespace vrto3d

#endif  // _WIN32
