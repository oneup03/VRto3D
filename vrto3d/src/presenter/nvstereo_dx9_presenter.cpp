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
#include "display_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

// SSSE3 — for the per-frame R/B byte swap in PresentFrame.
#include <tmmintrin.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include <nlohmann/json.hpp>
#include <openvr_driver.h>

#include "dx11_renderer.h"
#include "hmd_device_driver.h"
#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/win32_helper.hpp"

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


// ===========================================================================
// NvTimingsDb implementation
// ===========================================================================

namespace {

uint16_t json_u16(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing field: ") + name);
    auto x = j.at(name).get<int64_t>();
    if (x < 0 || x > 0xFFFF) throw std::runtime_error(std::string("Field out of range u16: ") + name);
    return static_cast<uint16_t>(x);
}

uint32_t json_u32(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing field: ") + name);
    auto x = j.at(name).get<int64_t>();
    if (x < 0 || x > 0xFFFFFFFF) throw std::runtime_error(std::string("Field out of range u32: ") + name);
    return static_cast<uint32_t>(x);
}

double json_double(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing field: ") + name);
    const auto& v = j.at(name);
    if (v.is_number_float()) return v.get<double>();
    if (v.is_number_integer()) return static_cast<double>(v.get<int64_t>());
    throw std::runtime_error(std::string("Field not numeric: ") + name);
}

const nlohmann::json& json_must(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing object: ") + name);
    return j.at(name);
}

NV_TIMING parseMonitorToNvTiming(const nlohmann::json& jmon) {
    NV_TIMING t{};
    std::memset(&t, 0, sizeof(t));

    t.pclk = json_u32(jmon, "frequency_10khz");

    const auto& jh = json_must(jmon, "hor");
    t.HTotal      = json_u16(jh, "total");
    t.HVisible    = json_u16(jh, "visible");
    t.HBorder     = json_u16(jh, "border");
    t.HFrontPorch = json_u16(jh, "frontPorch");
    t.HSyncWidth  = json_u16(jh, "numSync");
    t.HSyncPol    = 0;

    const auto& jv = json_must(jmon, "ver");
    t.VTotal      = json_u16(jv, "total");
    t.VVisible    = json_u16(jv, "visible");
    t.VBorder     = json_u16(jv, "border");
    t.VFrontPorch = json_u16(jv, "frontPorch");
    t.VSyncWidth  = json_u16(jv, "numSync");
    t.VSyncPol    = 0;

    t.interlaced = 0;
    std::memset(&t.etc, 0, sizeof(t.etc));

    double refresh_hz = json_double(jmon, "refresh_hz");
    t.etc.rr    = static_cast<NvU16>(std::lround(refresh_hz));
    t.etc.rrx1k = static_cast<NvU32>(std::lround(refresh_hz * 1000.0));

    return t;
}

vrto3d::NvTimingsEntry parseEntry(const nlohmann::json& jentry) {
    if (!jentry.contains("monitor_timings"))
        throw std::runtime_error("Entry missing 'monitor_timings'");
    vrto3d::NvTimingsEntry e;
    const auto& jmon = jentry.at("monitor_timings");
    e.timing     = parseMonitorToNvTiming(jmon);
    e.refresh_hz = static_cast<float>(json_double(jmon, "refresh_hz"));
    return e;
}


// Parse the EDID from the display to get the vendor+product key (e.g. "ACI_23F7").
std::wstring parse_monitor_EDID(NvU32 displayId)
{
    NV_EDID_DATA edidData = {};
    edidData.version = NV_EDID_DATA_VER;

    NvU8 edidBuffer[NV_EDID_DATA_SIZE_MAX] = {};
    edidData.pEDID = edidBuffer;
    edidData.sizeOfEDID = NV_EDID_DATA_SIZE_MAX;

    NV_EDID_FLAG edidFlag = NV_EDID_FLAG_RAW;
    NvAPI_Status status = NvAPI_DISP_GetEdidData(displayId, &edidData, &edidFlag);
    if (status != NVAPI_OK) {
        LOG() << "parse_monitor_EDID: NvAPI_DISP_GetEdidData failed status=" << status;
        return L"";
    }
    if (edidData.sizeOfEDID < 128) {
        LOG() << "parse_monitor_EDID: Bad EDID size: " << edidData.sizeOfEDID;
        return L"";
    }

    const NvU8* edid = edidData.pEDID;

    // Manufacturer ID (bytes 8-9)
    uint16_t vendor_id = (static_cast<uint16_t>(edid[8]) << 8) | static_cast<uint16_t>(edid[9]);
    char vendor_code[4] = {};
    vendor_code[0] = ((vendor_id >> 10) & 0x1F) + 'A' - 1;
    vendor_code[1] = ((vendor_id >> 5)  & 0x1F) + 'A' - 1;
    vendor_code[2] = (vendor_id         & 0x1F) + 'A' - 1;
    std::wstring vendor_string(vendor_code, vendor_code + 3);

    // Product / model ID (bytes 10-11), little-endian
    uint16_t product_id = (static_cast<uint16_t>(edid[11]) << 8) | static_cast<uint16_t>(edid[10]);
    std::wstringstream product_stream;
    product_stream << std::uppercase << std::hex
                   << std::setw(4) << std::setfill(L'0')
                   << product_id;

    return vendor_string + L"_" + product_stream.str();
}


// Pull shared display helpers into this translation unit.
using display_utils::DisplayPositionSnapshot;
using display_utils::SnapshotDisplayPositions;
using display_utils::RestoreDisplayPositions;
using display_utils::WaitForModesetSettle;

struct WindowSnapshot {
    HWND hwnd;
    RECT rect;
};

std::vector<WindowSnapshot> SnapshotProcessWindows()
{
    struct Ctx { DWORD pid; std::vector<WindowSnapshot> windows; };
    Ctx ctx{ GetCurrentProcessId(), {} };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto& c = *reinterpret_cast<Ctx*>(lp);
        DWORD wndPid = 0;
        GetWindowThreadProcessId(hwnd, &wndPid);
        if (wndPid != c.pid || !IsWindowVisible(hwnd)) return TRUE;
        RECT r{};
        if (GetWindowRect(hwnd, &r)) c.windows.push_back({ hwnd, r });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.windows;
}

void RestoreProcessWindows(const std::vector<WindowSnapshot>& snapshots)
{
    for (const auto& snap : snapshots) {
        if (!IsWindow(snap.hwnd)) continue;
        int w = snap.rect.right  - snap.rect.left;
        int h = snap.rect.bottom - snap.rect.top;
        SetWindowPos(snap.hwnd, nullptr, snap.rect.left, snap.rect.top, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    }
}

}  // namespace


// ---------------------------------------------------------------------------
// NvTimingsDb public methods
// ---------------------------------------------------------------------------

NvTimingsDb NvTimingsDb::Load(const std::string& jsonPath) {
    std::ifstream ifs(jsonPath);
    if (!ifs) throw std::runtime_error("NvTimingsDb: failed to open: " + jsonPath);
    nlohmann::json j;
    ifs >> j;
    NvTimingsDb db;
    db.data_.reserve(j.size());
    for (auto it = j.begin(); it != j.end(); ++it)
        db.data_.emplace(it.key(), parseEntry(it.value()));
    return db;
}

std::optional<NvTimingsEntry> NvTimingsDb::findExact(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

std::optional<NvTimingsEntry> NvTimingsDb::findByBaseAndRefresh(const std::string& baseKey, int refreshNearestInt) const {
    return findExact(baseKey + "_" + std::to_string(refreshNearestInt));
}

std::optional<NvTimingsEntry> NvTimingsDb::findHighestRefreshForBase(const std::string& baseKey) const {
    const std::string prefix = baseKey + "_";
    int bestRefresh = -1;
    const NvTimingsEntry* best = nullptr;
    for (const auto& kv : data_) {
        if (kv.first.size() <= prefix.size()) continue;
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string suffix = kv.first.substr(prefix.size());
        if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), ::isdigit)) continue;
        int rr = std::stoi(suffix);
        if (rr > bestRefresh) { bestRefresh = rr; best = &kv.second; }
    }
    if (best) {
        NvTimingsEntry result = *best;
        result.refresh_int = static_cast<NvU16>(bestRefresh);
        return result;
    }
    return std::nullopt;
}

std::string NvTimingsDb::to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string out(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        out.data(), size_needed, nullptr, nullptr);
    return out;
}


// ---------------------------------------------------------------------------
// FSE WndProc subclass — swallows WM_ACTIVATE(WA_INACTIVE),
// WM_ACTIVATEAPP(FALSE), and WM_KILLFOCUS so that DefWindowProc never
// triggers the minimize-on-deactivate behavior for our FSE D3D9Ex window.
// ---------------------------------------------------------------------------
LRESULT CALLBACK NvStereoDx9Presenter::FseSubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<NvStereoDx9Presenter*>(
        GetWindowLongPtrW(hw, GWLP_USERDATA));
    WNDPROC orig = self ? self->orig_wndproc_ : DefWindowProcW;

    // NOTE: we deliberately do NOT mark the device dead on WM_DISPLAYCHANGE
    // — FSE D3D9Ex itself raises that message when it modesets the display
    // for fullscreen-exclusive entry, and that's a normal, expected event.
    // Treating it as a fatal signal kills the device immediately after
    // creation and breaks 3D Vision activation (the present pipeline turns
    // into a no-op on the very first frame). Hot-plug protection has to be
    // distinguished from our own modesets via something other than this
    // message alone; the periodic CheckDeviceState in PresentFrame already
    // catches the unrecoverable-device case after the fact.

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

    // 3D Vision is single-monitor by default
    platform::MonitorInfo primary{}, secondary{};
    if (!platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary)) {
        LOG() << "NvStereoDx9Presenter::Init: ResolveTargetMonitors failed";
        return false;
    }

    window_stop_.store(false);
    window_ready_.store(false);
    window_failed_.store(false);
    window_thread_ = std::thread(&NvStereoDx9Presenter::WindowThreadLoop, this, &renderer, primary);

    // 30-second timeout (3000 × 10ms) to account for LightBoost modeset
    // which can take several seconds (WaitForModesetSettle + driver time).
    for (int i = 0; i < 3000; ++i) {
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
    platform::EnablePerMonitorV2DpiAwareness();

    monitor_w_ = primary.width;
    monitor_h_ = primary.height;

    // 1. NvAPI_Initialize — guarded by SEH so missing nvapi64.dll = clean fail.
    //    Must happen before anything NVAPI-related (LightBoost, stereo, etc.).
    {
        bool dll_missing = false;
        NvAPI_Status s = TryNvAPIInitializeSEH(&dll_missing);
        if (dll_missing) {
            LOG() << "NvStereoDx9Presenter: nvapi64.dll not resolvable. "
                  << "Install NVIDIA drivers, or pick a different output_mode.";
            window_failed_.store(true);
            return;
        }
        if (s != NVAPI_OK) {
            LOG() << "NvStereoDx9Presenter: NvAPI_Initialize failed code=" << static_cast<int>(s);
            window_failed_.store(true);
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
            return;
        }
    }

    // 2b. Force stereo driver mode to AUTOMATIC. The NV3D-signed packed back-
    //     buffer route only works in AUTOMATIC mode — the driver scans for
    //     the signature and routes left/right halves per-eye. DIRECT mode
    //     disables that auto-detection: the emitter still turns on but the
    //     panel just blits the SbS surface as-is (verified empirically).
    //     We set it explicitly in case a prior session left it on DIRECT.
    {
        NvAPI_Status s = NvAPI_Stereo_SetDriverMode(NVAPI_STEREO_DRIVER_MODE_AUTOMATIC);
        if (s != NVAPI_OK) {
            LOG() << "NvStereoDx9Presenter: NvAPI_Stereo_SetDriverMode(AUTOMATIC) failed status="
                  << static_cast<int>(s);
        } else {
            LOG() << "NvStereoDx9Presenter: stereo driver mode = AUTOMATIC";
        }
    }


    // 3. LightBoost: check the current resolution/timings against the
    //    nvtimings.json database. If the monitor's current timing doesn't
    //    match a known entry, apply a LightBoost custom resolution.
    //    This must happen BEFORE creating the window so the modeset is
    //    settled and the D3D9 FSE device sees the correct timing.
    //    Non-fatal — failures are logged but never abort init.
    //    Single-display only: filtered to the configured cfg.display_index
    //    target so we never modify timings on other monitors.
    CheckAndApplyLightBoost(primary.device_name);

    // Drain WM_DISPLAYCHANGE etc. one more time before creating the present
    // window — ensures DWM/thread DPI see the new modeset before we touch
    // any HWND. (WaitForTimingMatch already pumped during the poll; this is
    // just a final belt-and-braces drain.)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    window_ = platform::CreatePresentWindow(primary, nullptr, "VRto3D-3DVision");
    if (!window_) {
        LOG() << "NvStereoDx9Presenter: CreatePresentWindow failed";
        DisableLightBoost();
        window_failed_.store(true);
        return;
    }
    HWND hwnd = static_cast<HWND>(window_->NativeHandle());

    // 4-10. Build D3D9Ex device + packed surfaces + activate stereo
    if (!BuildD3D9Stack(hwnd, primary.width, primary.height, primary.refresh_hz)) {
        LOG() << "NvStereoDx9Presenter: BuildD3D9Stack failed";
        DisableLightBoost();
        NvAPI_Unload();
        window_failed_.store(true);
        window_.reset();
        return;
    }

    // Stereo activation retry: NvAPI_Stereo_Activate sometimes needs a dummy
    // PresentEx cycle before it sticks
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

    // Graceful teardown. The previous design called TerminateProcess here,
    // which skipped all D3D9/NvAPI cleanup and was itself implicated in the
    // shutdown-time GPU driver freezes — the driver had to garbage-collect
    // state for a process that exited mid-flight, with refcounts still held
    // on the D3D9 device and NvAPI stereo handle.
    //
    // First action: hide the window NOW. Even if the rest of teardown takes
    // a couple of seconds, the user sees the VRto3D-3DVision window vanish
    // immediately when SteamVR closes.
    if (window_) {
        HWND hwnd = static_cast<HWND>(window_->NativeHandle());
        if (hwnd) {
            ShowWindow(hwnd, SW_HIDE);
        }
    }

    // Order matters. The original design did DisableLightBoost FIRST so
    // DWM/driver were back on the original modeset before any D3D objects
    // were released — but in practice we've observed that path freezes the
    // GPU on shutdown when RevertCustomDisplayTrial fails (e.g. when the
    // trial has been auto-finalized), because we then release a still-
    // engaged D3D9Ex FSE device while the GPU has an unreverted custom
    // timing applied at the NVAPI level.
    //
    // New order:
    //   1. RemoveFseSubclass    — drops WS_EX_LAYERED + WS_EX_TRANSPARENT
    //                             and pumps messages so DWM sees the style
    //                             change before any swap chain is torn down.
    //   2. Stereo_DestroyHandle — NvAPI bookkeeping cleanup.
    //   3. Release D3D9 objects — drops FSE, returns the panel to desktop
    //                             control. Done BEFORE LightBoost revert so
    //                             the GPU isn't holding the display when
    //                             NVAPI attempts the modeset.
    //   4. DisableLightBoost    — revert the custom timing. The display is
    //                             no longer FSE-locked at this point.
    //   5. NvAPI_Unload         — mirrors XR3DV's pattern.
    //   6. Destroy present window.
    LOG() << "NvStereoDx9Presenter: teardown step 1 — remove FSE WndProc subclass";
    RemoveFseSubclass();

    if (stereo_handle_) {
        LOG() << "NvStereoDx9Presenter: teardown step 2 — destroy NvAPI stereo handle";
        NvAPI_Stereo_DestroyHandle(stereo_handle_);
        stereo_handle_ = nullptr;
    }

    LOG() << "NvStereoDx9Presenter: teardown step 3 — release D3D9 objects (drops FSE)";
    back_buffer_.Reset();
    packed_default_.Reset();
    packed_input_default_.Reset();
    packed_sysmem_.Reset();
    staging_.Reset();
    device9_.Reset();
    d3d9_.Reset();

    LOG() << "NvStereoDx9Presenter: teardown step 4 — disable LightBoost";
    DisableLightBoost();

    LOG() << "NvStereoDx9Presenter: teardown step 5 — NvAPI_Unload";
    NvAPI_Unload();

    // The Win32 present window must be destroyed on the thread that pumped
    // its messages — i.e. THIS thread. The old TerminateProcess design
    // sidestepped this by killing the whole vrserver process; the new
    // graceful path has to destroy it explicitly or the window lingers
    // visibly after SteamVR closes.
    LOG() << "NvStereoDx9Presenter: teardown step 6 — destroy present window";
    window_.reset();

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
        pp.BackBufferCount        = 2;
        pp.MultiSampleType        = D3DMULTISAMPLE_NONE;
        pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow          = hwnd;
        pp.Windowed               = windowed;
        pp.EnableAutoDepthStencil = FALSE;
        pp.Flags                  = 0;
        pp.FullScreen_RefreshRateInHz = windowed ? 0 : dm.RefreshRate;
        // INTERVAL_ONE: hardware vsync inside PresentEx. The presenter is
        // split into RecordComposite (held under context_mutex_) and Present
        // (mutex released), so this vsync block does NOT stall the
        // compositor thread's next OnDirectModeFrame. 3D Vision frame-
        // sequential alternation is driven by the GPU's stereo signaling,
        // not by the per-Present sync interval, so INTERVAL_ONE is the
        // tear-free choice.
        pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
    };

    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING
                | D3DCREATE_MULTITHREADED
                | D3DCREATE_FPU_PRESERVE;

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
    // vrserver isn't normally allowed to call SetForegroundWindow directly
    // (Windows foreground-lock), but the AttachThreadInput trick inside
    // ForceForeground bypasses it. FSE D3D9Ex REQUIRES the device window
    // to be the focus window — without this, CreateDeviceEx either falls
    // back to a non-exclusive mode (breaking 3D Vision activation) or
    // returns an error.
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
        // FSE window never minimizes when the user interacts with another
        // display, and apply WS_EX_LAYERED + WS_EX_TRANSPARENT for click-
        // through.
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
    // Input-side dims (CPU memcpy target + UpdateSurface upload pair).
    const uint32_t in_pw = input_w_per_eye * 2u;
    const uint32_t in_ph = input_h;
    // Output-side dims (panel-sized so the LINEAR scale gives uniform
    // per-eye quality and the NV3D squash to back-buffer is clean 2:1).
    const uint32_t out_pw = monitor_w_ * 2u;
    const uint32_t out_ph = monitor_h_ + 1u;   // +1 row for NV3D signature

    const bool input_unchanged  = packed_sysmem_ && packed_w_in_  == in_pw && packed_h_in_  == in_ph;
    const bool output_unchanged = packed_default_ && packed_w_out_ == out_pw && packed_h_out_ == monitor_h_;
    if (input_unchanged && output_unchanged) return true;

    HRESULT hr = S_OK;

    if (!input_unchanged) {
        packed_sysmem_.Reset();
        packed_input_default_.Reset();
        hr = device9_->CreateOffscreenPlainSurface(
            in_pw, in_ph, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &packed_sysmem_, nullptr);
        if (FAILED(hr)) {
            LOG() << "NvStereoDx9Presenter: CreateOffscreenPlainSurface(packed_sysmem) failed hr=0x"
                  << std::hex << hr;
            return false;
        }
        hr = device9_->CreateOffscreenPlainSurface(
            in_pw, in_ph, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &packed_input_default_, nullptr);
        if (FAILED(hr)) {
            LOG() << "NvStereoDx9Presenter: CreateOffscreenPlainSurface(packed_input_default) failed hr=0x"
                  << std::hex << hr;
            packed_sysmem_.Reset();
            return false;
        }
        packed_w_in_ = in_pw;
        packed_h_in_ = in_ph;
    }

    if (!output_unchanged) {
        packed_default_.Reset();
        sig_valid_ = false;  // surface recreated → signature row needs rewriting
        // Lockable render target — see header comment for why both RT and
        // lockable. LINEAR stretch dest + byte-exact signature LockRect.
        hr = device9_->CreateRenderTarget(
            out_pw, out_ph, D3DFMT_X8R8G8B8,
            D3DMULTISAMPLE_NONE, 0,
            TRUE,                        // lockable
            &packed_default_, nullptr);
        if (FAILED(hr)) {
            LOG() << "NvStereoDx9Presenter: CreateRenderTarget(packed_default panel, lockable) failed hr=0x"
                  << std::hex << hr;
            return false;
        }
        packed_w_out_ = out_pw;
        packed_h_out_ = monitor_h_;
    }

    LOG() << "NvStereoDx9Presenter: packed pipeline input=" << in_pw << "x" << in_ph
          << " (per-eye " << input_w_per_eye << "x" << input_h << ")"
          << "  output=" << out_pw << "x" << monitor_h_
          << " (per-eye " << monitor_w_ << "x" << monitor_h_ << ")";
    return true;
}


void NvStereoDx9Presenter::RefreshSignatureIfNeeded()
{
    if (!packed_default_) return;

    bool live_swap = eye_swap_;
    if (renderer_ && renderer_->Component()) {
        live_swap = renderer_->Component()->GetConfig().eye_swap;
    }

    if (sig_valid_ &&
        sig_width_  == monitor_w_ &&
        sig_height_ == monitor_h_ &&
        sig_swap_   == live_swap) {
        return;
    }

    NvStereoImageHeader hdr{};
    hdr.signature = kNvStereoSignature;
    hdr.width     = monitor_w_;
    hdr.height    = monitor_h_;
    hdr.bpp       = 32;
    hdr.flags     = live_swap ? 1u : 0u;

    RECT hdr_lock{ 0, static_cast<LONG>(packed_h_out_), 5,
                   static_cast<LONG>(packed_h_out_) + 1 };
    D3DLOCKED_RECT hlr{};
    HRESULT lhr = packed_default_->LockRect(&hlr, &hdr_lock, 0);
    if (FAILED(lhr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: LockRect(header row) failed hr=0x"
                  << std::hex << lhr;
        (void)CheckAndMarkD3D9Dead(lhr, "LockRect(header)");
        return;
    }
    std::memcpy(hlr.pBits, &hdr, sizeof(hdr));
    packed_default_->UnlockRect();

    sig_width_  = monitor_w_;
    sig_height_ = monitor_h_;
    sig_swap_   = live_swap;
    sig_valid_  = true;
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


void NvStereoDx9Presenter::RecordComposite(ID3D11Texture2D* sbs_input)
{
    if (!device9_ || !sbs_input || !renderer_) return;

    // Circuit-breaker: once the D3D9 device has been observed in an
    // unrecoverable state, stop driving it. Continuing to submit work to a
    // dead device causes the driver to spin and can wedge the GPU.
    if (d3d9_dead_.load(std::memory_order_relaxed)) return;

    // Periodically check the D3D9Ex device state. This is the modern (D3D9Ex)
    // replacement for TestCooperativeLevel — it cheaply detects device-lost
    // / hung / removed without doing any GPU work.
    if (++frames_since_state_check_ >= 60 && device9_) {
        frames_since_state_check_ = 0;
        HRESULT cs = device9_->CheckDeviceState(subclassed_hwnd_);
        // S_OK, S_PRESENT_OCCLUDED — fine to continue.
        // S_PRESENT_MODE_CHANGED, D3DERR_DEVICELOST/HUNG/REMOVED — bail.
        if (cs != S_OK && cs != S_PRESENT_OCCLUDED) {
            (void)CheckAndMarkD3D9Dead(cs, "CheckDeviceState");
            if (d3d9_dead_.load(std::memory_order_relaxed)) return;
        }
    }

    ID3D11Device*        dev = renderer_->Device();
    ID3D11DeviceContext* ctx = renderer_->Context();

    D3D11_TEXTURE2D_DESC td{};
    sbs_input->GetDesc(&td);

    // Recreate the DX11 STAGING texture on dimension/format change.
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

    // GPU copy out_sbs_ → staging. The subsequent Map(MAP_READ) below acts
    // as the implicit GPU→CPU sync point (waits for the GPU to finish).
    ctx->CopyResource(staging_.Get(), sbs_input);

    const uint32_t per_eye_w = td.Width / 2u;
    if (!EnsurePackedSurfaces(per_eye_w, td.Height)) return;

    // GPU→CPU sync point. Use DO_NOT_WAIT + bounded poll instead of a
    // blocking Map: if the GPU is wedged (LightBoost+FSE+NV3D init has hit
    // bad-state races on startup), an indefinite Map blocks this thread
    // forever while holding context_mutex_, which then blocks the
    // compositor and the runtime appears frozen. With a deadline we mark
    // the D3D device dead, bail this frame, and the next frame's
    // circuit-breaker takes over instead of hanging.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    {
        HRESULT mhr = E_FAIL;
        const auto map_deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds(50);
        // One-time diagnostic log so if we *do* hang here next time, we
        // know it's the staging Map and not something later.
        static std::atomic<bool> logged_first{false};
        bool expected = false;
        if (logged_first.compare_exchange_strong(expected, true)) {
            LOG() << "NvStereoDx9Presenter: first PresentFrame about to Map(staging, MAP_READ) "
                  << "— if no further logs appear, the GPU is wedged at this point.";
        }
        while (true) {
            mhr = ctx->Map(staging_.Get(), 0, D3D11_MAP_READ,
                            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (SUCCEEDED(mhr)) break;
            if (mhr != DXGI_ERROR_WAS_STILL_DRAWING) {
                static std::atomic<bool> err_logged{false};
                bool e = false;
                if (err_logged.compare_exchange_strong(e, true))
                    LOG() << "NvStereoDx9Presenter: Map(staging) failed hr=0x"
                          << std::hex << mhr;
                return;
            }
            if (std::chrono::steady_clock::now() >= map_deadline) {
                static std::atomic<bool> to_logged{false};
                bool e = false;
                if (to_logged.compare_exchange_strong(e, true))
                    LOG() << "NvStereoDx9Presenter: Map(staging) DO_NOT_WAIT polled past "
                             "50ms deadline — GPU appears wedged; marking device dead.";
                // Mark DX9 dead so PresentFrame becomes a no-op on subsequent
                // frames. Also poke the DX11 renderer so its own circuit-
                // breaker doesn't keep submitting work into the wedge.
                (void)CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "Map(staging) timeout");
                return;
            }
            std::this_thread::yield();
        }
    }

    // ----- Stage 1 [CPU copy + format normalize]: pack staging into
    // packed_sysmem_. Source byte layout depends on what the compositor
    // submitted:
    //   - DXGI_FORMAT_R8G8B8A8 family (fmt 28/29): bytes are [R,G,B,A].
    //     D3D9 X8R8G8B8 stores [B,G,R,X] — we swap R↔B per pixel.
    //     SSSE3 _mm_shuffle_epi8 does 4 pixels per instruction.
    //   - DXGI_FORMAT_B8G8R8A8 family (fmt 87/91): bytes are [B,G,R,A] —
    //     already match D3D9 byte order, just a memcpy + alpha normalize.
    const bool src_is_rgba = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                              td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(packed_sysmem_->LockRect(&lr, nullptr, 0))) {
        const uint32_t copy_h    = td.Height;
        const uint32_t row_bytes = packed_w_in_ * 4u;
        if (src_is_rgba) {
            // SSSE3 byte-shuffle: read 16 bytes (4 pixels RGBA), write 16
            // bytes (4 pixels BGRA with alpha forced to 0xFF).
            const __m128i shuf = _mm_setr_epi8(
                2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
            const __m128i alpha_mask = _mm_set1_epi32(static_cast<int>(0xFF000000u));
            for (uint32_t y = 0; y < copy_h; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                uint8_t*       dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
                uint32_t x = 0;
                for (; x + 16 <= row_bytes; x += 16) {
                    __m128i v       = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x));
                    __m128i swapped = _mm_shuffle_epi8(v, shuf);
                    __m128i out     = _mm_or_si128(swapped, alpha_mask);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), out);
                }
                // Scalar tail (typical row widths are a multiple of 16, so
                // this only fires for unusual resolutions).
                for (; x < row_bytes; x += 4) {
                    dst[x + 0] = src[x + 2];
                    dst[x + 1] = src[x + 1];
                    dst[x + 2] = src[x + 0];
                    dst[x + 3] = 0xFF;
                }
            }
        } else {
            // BGRA → BGRA: straight memcpy + SIMD alpha-byte normalize.
            const __m128i alpha_mask = _mm_set1_epi32(static_cast<int>(0xFF000000u));
            for (uint32_t y = 0; y < copy_h; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                uint8_t*       dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
                std::memcpy(dst, src, row_bytes);
                uint32_t x = 0;
                for (; x + 16 <= row_bytes; x += 16) {
                    __m128i v   = _mm_loadu_si128(reinterpret_cast<__m128i*>(dst + x));
                    __m128i out = _mm_or_si128(v, alpha_mask);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), out);
                }
                for (; x < row_bytes; x += 4) {
                    dst[x + 3] = 0xFF;
                }
            }
        }
        packed_sysmem_->UnlockRect();
    }
    ctx->Unmap(staging_.Get(), 0);

    // ----- Stage 2 [GPU upload]: SYSMEM → DEFAULT plain (input-sized).
    RECT  in_body { 0, 0, static_cast<LONG>(packed_w_in_), static_cast<LONG>(td.Height) };
    POINT origin{ 0, 0 };
    HRESULT hr = device9_->UpdateSurface(packed_sysmem_.Get(), &in_body,
                                          packed_input_default_.Get(), &origin);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: UpdateSurface(body) failed hr=0x" << std::hex << hr;
        (void)CheckAndMarkD3D9Dead(hr, "UpdateSurface");
        return;
    }

    // ----- Stage 3 [uniform LINEAR scale]: input-sized DEFAULT plain →
    // panel-sized lockable RT. Each eye gets a uniform per-axis scale here.
    RECT out_body{ 0, 0, static_cast<LONG>(packed_w_out_), static_cast<LONG>(packed_h_out_) };
    hr = device9_->StretchRect(packed_input_default_.Get(), &in_body,
                                packed_default_.Get(),       &out_body,
                                D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: StretchRect(input->panel) failed hr=0x" << std::hex << hr;
        (void)CheckAndMarkD3D9Dead(hr, "StretchRect(input->panel)");
        return;
    }

    // ----- Stage 4 [signature]: ensure the NV3D header row is current.
    // No-op on the common path (only re-writes on dim/eye-swap changes).
    RefreshSignatureIfNeeded();

    // ----- Stage 5 [final squash]: lockable RT → back buffer. NV3D sees
    // the signature row, splits the body horizontally, and unsquashes each
    // half back to full panel width per eye on the stereo back buffer.
    hr = device9_->StretchRect(packed_default_.Get(), &out_body,
                                back_buffer_.Get(), nullptr, D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: StretchRect(panel->backbuf) failed hr=0x" << std::hex << hr;
        (void)CheckAndMarkD3D9Dead(hr, "StretchRect(panel->backbuf)");
        return;
    }

    // RecordComposite ends here — PresentEx + activation retry happen in
    // Present(), called by Dx11Renderer after context_mutex_ is released so
    // INTERVAL_ONE's vsync block doesn't stall the compositor thread.
}


void NvStereoDx9Presenter::Present()
{
    if (!device9_ || d3d9_dead_.load(std::memory_order_relaxed)) return;

    // INTERVAL_ONE on the present-params blocks PresentEx for vsync — that's
    // what makes the FSE 3D Vision output tear-free. RecordComposite ran
    // under context_mutex_, this runs after release.
    HRESULT hr = device9_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true))
            LOG() << "NvStereoDx9Presenter: PresentEx failed hr=0x" << std::hex << hr;
        // Route through the D3D9 circuit-breaker — if the device went away,
        // stop driving it instead of spinning on every frame.
        (void)CheckAndMarkD3D9Dead(hr, "PresentEx");
        return;
    }


    // Deferred stereo activation: keep retrying after each real present until
    // the driver's IR emitter wakes up or we run out of attempts. Throttled
    // to one attempt per 50ms so the 60-retry budget can't burn through in
    // <1s during driver contention (e.g. while LightBoost is settling).
    const DWORD now_tick = GetTickCount();
    const bool retry_due = (now_tick - last_stereo_activate_tick_) >= 50;
    if (!stereo_activated_ && activation_retries_left_ > 0 && stereo_handle_ &&
        !d3d9_dead_.load(std::memory_order_relaxed) && retry_due) {
        last_stereo_activate_tick_ = now_tick;
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
            // Don't alarm the user — NvAPI_Stereo_IsActivated returning false
            // does NOT mean stereo isn't engaging. Our packed back-buffer carries
            // the NV3D signature in its last row, which the driver detects on
            // every PresentEx and uses to route left/right halves per-eye via
            // the IR emitter. That path engages stereo at the driver level
            // independent of the NvAPI app-registration state queried here.
            // If you see correct frame-sequential 3D output, this is the
            // expected behaviour — just ignore the IsActivated state.
            LOG() << "NvStereoDx9Presenter: NvAPI_Stereo_IsActivated remains false "
                     "after 60 retries. The NV3D packed-surface signature path "
                     "should still drive per-eye routing at the driver level. "
                     "If you genuinely see mono output, verify:";
            LOG() << "  1) NVIDIA 3D Vision driver installed (use 3D Fix Manager)";
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

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        if (!window_) break;

        const bool is_on_top   = focus_.is_on_top   && focus_.is_on_top->load();
        const bool man_on_top  = focus_.man_on_top  && focus_.man_on_top->load();
        const uint32_t pid     = focus_.app_pid ? focus_.app_pid->load() : 0;
        const bool auto_focus  = focus_.auto_focus  ? focus_.auto_focus->load() : auto_focus_;

        const bool app_running = platform::IsProcessRunning(pid);
        if (pid == 0 || !app_running) {
            last_auto_focused_pid = 0;
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
            if (man_on_top || is_on_top) {
                want_on_top = true;
            } else if (auto_focus && app_running && pid != 0
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
                // Hand input focus to the game window — FSE keeps the VR
                // window visually on top, but the user expects keystrokes
                // and mouse input to land in the game. When SteamVR is
                // launched by the app, the game window may not exist yet
                // and SteamVR's status window often grabs foreground
                // mid-bring-up; run a watch loop that re-asserts focus
                // whenever it drifts off the game.
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
        }
    }
}


// ===========================================================================
// LightBoost — check current resolution/timings against the nvtimings.json
// database. If a match is found, apply the LightBoost custom resolution via
// NvAPI_DISP_TryCustomDisplay so the monitor enters low-persistence mode.
// ===========================================================================

// Resolve a GDI device name (e.g. "\\.\DISPLAY3") to the NVAPI displayId.
// Returns 0 on failure.
NvU32 NvStereoDx9Presenter::ResolveNvDisplayId(const std::string& gdi_device_name)
{
    if (gdi_device_name.empty()) return 0;
    char narrow[NVAPI_SHORT_STRING_MAX] = {};
    const size_t n = (std::min)(gdi_device_name.size(), sizeof(narrow) - 1);
    std::memcpy(narrow, gdi_device_name.data(), n);
    NvU32 displayId = 0;
    NvAPI_Status s = NvAPI_DISP_GetDisplayIdByDisplayName(narrow, &displayId);
    if (s != NVAPI_OK) {
        LOG() << "ResolveNvDisplayId: NvAPI_DISP_GetDisplayIdByDisplayName('"
              << narrow << "') failed status=" << s;
        return 0;
    }
    return displayId;
}


// Pump messages and poll NvAPI_DISP_GetTiming on `displayId` until the live
// timing matches `expected` (HTotal/VTotal within 1 unit, pclk within 0.1%)
// or `timeout_ms` elapses. Returns true if a match was observed.
bool NvStereoDx9Presenter::WaitForTimingMatch(NvU32 displayId,
                                              const NV_TIMING& expected,
                                              DWORD timeout_ms)
{
    if (!displayId) {
        // No displayId to poll — fall back to a fixed wait so callers still
        // get *some* settle when the resolution lookup failed earlier.
        display_utils::WaitForModesetSettle(timeout_ms);
        return false;
    }
    const DWORD start = GetTickCount();
    while (true) {
        // Drain WM_DISPLAYCHANGE etc. so DWM sees the modeset complete.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        NV_TIMING live = {};
        NV_TIMING_INPUT ti = {}; ti.version = NV_TIMING_INPUT_VER;
        NvAPI_Status s = NvAPI_DISP_GetTiming(displayId, &ti, &live);
        if (s == NVAPI_OK) {
            const bool h_match = (live.HTotal == expected.HTotal);
            const bool v_match = (live.VTotal == expected.VTotal);
            // pclk in 10kHz units; allow 0.1% tolerance.
            const NvU32 pclk_a = live.pclk, pclk_b = expected.pclk;
            const NvU32 pclk_tol = pclk_b ? (std::max<NvU32>)(1u, pclk_b / 1000u) : 1u;
            const bool p_match = (pclk_a == 0 || pclk_b == 0) ? true
                : (pclk_a >= pclk_b - pclk_tol && pclk_a <= pclk_b + pclk_tol);
            if (h_match && v_match && p_match) {
                const DWORD elapsed = GetTickCount() - start;
                LOG() << "WaitForTimingMatch: settled after " << elapsed
                      << "ms (HTotal=" << live.HTotal << " VTotal=" << live.VTotal
                      << " pclk=" << live.pclk << ")";
                return true;
            }
        }
        if (GetTickCount() - start >= timeout_ms) {
            LOG() << "WaitForTimingMatch: TIMEOUT after " << timeout_ms
                  << "ms — expected HTotal=" << expected.HTotal
                  << " VTotal=" << expected.VTotal << " pclk=" << expected.pclk;
            return false;
        }
        Sleep(50);
    }
}


// Like WaitForTimingMatch but waits for the timing to *differ* from `previous`
// (used after RevertCustomDisplayTrial so we know the revert took effect).
bool NvStereoDx9Presenter::WaitForTimingChange(NvU32 displayId,
                                               const NV_TIMING& previous,
                                               DWORD timeout_ms)
{
    if (!displayId) {
        display_utils::WaitForModesetSettle(timeout_ms);
        return false;
    }
    const DWORD start = GetTickCount();
    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        NV_TIMING live = {};
        NV_TIMING_INPUT ti = {}; ti.version = NV_TIMING_INPUT_VER;
        NvAPI_Status s = NvAPI_DISP_GetTiming(displayId, &ti, &live);
        if (s == NVAPI_OK) {
            if (live.HTotal != previous.HTotal || live.VTotal != previous.VTotal) {
                const DWORD elapsed = GetTickCount() - start;
                LOG() << "WaitForTimingChange: settled after " << elapsed
                      << "ms (now HTotal=" << live.HTotal << " VTotal=" << live.VTotal << ")";
                return true;
            }
        }
        if (GetTickCount() - start >= timeout_ms) {
            LOG() << "WaitForTimingChange: TIMEOUT after " << timeout_ms << "ms";
            return false;
        }
        Sleep(50);
    }
}


// Once any D3D9Ex call returns a state-loss / removed / hung HRESULT, we mark
// the device dead and the present pipeline becomes a no-op. We never attempt
// live ResetEx — driver semantics around DEVICEREMOVED on D3D9Ex are flaky,
// and the failure mode we're protecting against (a wedged driver) is exactly
// the one where ResetEx is least likely to work. "Stop, log, user restarts" is
// the honest contract; the next SteamVR launch rebuilds the whole stack.
bool NvStereoDx9Presenter::CheckAndMarkD3D9Dead(HRESULT hr, const char* origin)
{
    const bool is_lost =
        hr == D3DERR_DEVICELOST ||
        hr == D3DERR_DEVICEHUNG ||
        hr == D3DERR_DEVICEREMOVED ||
        hr == D3DERR_DRIVERINTERNALERROR ||
        hr == S_PRESENT_MODE_CHANGED;
    if (!is_lost) return false;
    bool expected = false;
    if (d3d9_dead_.compare_exchange_strong(expected, true)) {
        LOG() << "NvStereoDx9Presenter: D3D9Ex device entered terminal state in "
              << (origin ? origin : "?") << " hr=0x" << std::hex << hr
              << " — stopping device. SteamVR restart required.";
    }
    return true;
}


void NvStereoDx9Presenter::CheckAndApplyLightBoost(const std::string& target_gdi_device)
{
    LOG() << "NvStereoDx9Presenter::CheckAndApplyLightBoost target='" << target_gdi_device << "'";

    NvAPI_Status  status;

    // Resolve the GDI device name to a single NVAPI displayId. LightBoost is
    // strictly single-display — applying timings to other monitors is unsafe
    // and was the root cause of the multi-monitor freezes.
    const NvU32 target_id = ResolveNvDisplayId(target_gdi_device);
    if (!target_id) {
        LOG() << "CheckAndApplyLightBoost: could not resolve target display '"
              << target_gdi_device << "' to an NVAPI displayId — skipping LightBoost.";
        return;
    }
    LOG() << "CheckAndApplyLightBoost: target displayId=" << target_id;

    NvPhysicalGpuHandle gpu_handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
    NvU32               gpu_count = 0;

    status = NvAPI_EnumPhysicalGPUs(gpu_handles, &gpu_count);
    if (status != NVAPI_OK || gpu_count == 0) {
        LOG() << "CheckAndApplyLightBoost: Failed to enumerate NVIDIA GPUs. Status=" << status;
        return;
    }

    // Load the nvtimings.json database via VRResources.
    NvTimingsDb db;
    try {
        char path[512] = {};
        vr::VRResources()->GetResourceFullPath(
            "{vrto3d}/nvtimings.json", "", path, sizeof(path));
        LOG() << "CheckAndApplyLightBoost: loading nvtimings.json from: " << path;
        db = NvTimingsDb::Load(path);
    } catch (const std::exception& ex) {
        LOG() << "CheckAndApplyLightBoost: Error loading nvtimings.json: " << ex.what();
        return;
    }

    // Snapshot the OS-stored DEVMODE for the target GDI device. This is what
    // Windows considers the "real" desktop mode independent of any NVAPI
    // custom-display trial. We'll use it in DisableLightBoost as a
    // ChangeDisplaySettingsExW fallback when the trial revert fails (the
    // trial state gets invalidated by FSE D3D9Ex modesets during the
    // session, and once that happens NvAPI_DISP_RevertCustomDisplayTrial
    // returns -1 and the panel stays at the LightBoost timing).
    target_gdi_device_.clear();
    has_original_devmode_ = false;
    if (!target_gdi_device.empty()) {
        const int n = MultiByteToWideChar(
            CP_UTF8, 0, target_gdi_device.c_str(), -1, nullptr, 0);
        if (n > 0) {
            target_gdi_device_.resize(static_cast<size_t>(n - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, target_gdi_device.c_str(), -1,
                                target_gdi_device_.data(), n);
            DEVMODEW dm{}; dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(target_gdi_device_.c_str(),
                                        ENUM_CURRENT_SETTINGS, &dm, 0)) {
                original_devmode_     = dm;
                has_original_devmode_ = true;
                LOG() << "CheckAndApplyLightBoost: snapshotted DEVMODE "
                      << dm.dmPelsWidth << "x" << dm.dmPelsHeight << "@"
                      << dm.dmDisplayFrequency << "Hz for fallback revert";
            }
        }
    }

    // NOTE: no speculative revert here. If the panel was left in a
    // LightBoost timing from a previous session, the normal MISMATCH path
    // below just re-applies the same custom timing — that's cheap and
    // non-disruptive (the panel is already showing the right signal).
    // Kicking it back to the original timing via Revert+ChangeDisplaySettings
    // here just to immediately re-apply causes an unnecessary modeset
    // flicker. The real cleanup happens in DisableLightBoost via the
    // ChangeDisplaySettingsExW fallback that uses the DEVMODE we
    // snapshotted above.

    primary_display_ids_.clear();
    has_original_target_timing_ = false;
    bool target_found = false;
    bool already_good = false;

    for (NvU32 gpuIndex = 0; gpuIndex < gpu_count && !target_found; ++gpuIndex) {
        NvPhysicalGpuHandle gpu = gpu_handles[gpuIndex];

        NV_GPU_DISPLAYIDS display_ids[NVAPI_MAX_DISPLAYS] = {};
        NvU32 display_count = NVAPI_MAX_DISPLAYS;
        for (NvU32 i = 0; i < display_count; ++i)
            display_ids[i].version = NV_GPU_DISPLAYIDS_VER;

        status = NvAPI_GPU_GetConnectedDisplayIds(gpu, display_ids, &display_count, 0);
        if (status != NVAPI_OK || display_count == 0) continue;

        for (NvU32 d = 0; d < display_count; ++d) {
            const NvU32 displayId = display_ids[d].displayId;

            // Single-display filter — only the SteamVR target gets LightBoost.
            if (displayId != target_id) continue;

            std::wstring monitor_edid = parse_monitor_EDID(displayId);
            if (monitor_edid.empty()) {
                LOG() << "CheckAndApplyLightBoost: target displayId=" << displayId
                      << " has no readable EDID — skipping LightBoost.";
                continue;
            }

            // Query the current timing for this display.
            NV_TIMING       timing  = {};
            NV_TIMING_INPUT current = {};
            current.version = NV_TIMING_INPUT_VER;

            status = NvAPI_DISP_GetTiming(displayId, &current, &timing);
            if (status != NVAPI_OK) {
                LOG() << "CheckAndApplyLightBoost: NvAPI_DISP_GetTiming failed for target display "
                      << displayId << " status=" << status;
                continue;
            }

            LOG() << "CheckAndApplyLightBoost: GPU " << gpuIndex
                  << " target Display EDID=" << NvTimingsDb::to_utf8(monitor_edid)
                  << " rr=" << timing.etc.rr
                  << " VTotal=" << timing.VTotal << " HTotal=" << timing.HTotal;

            std::string baseKey = NvTimingsDb::to_utf8(monitor_edid);

            // Look up: exact base+refresh first, then highest available refresh.
            std::optional<NvTimingsEntry> e = db.findByBaseAndRefresh(baseKey, timing.etc.rr);
            bool usedFallback = false;
            if (!e) {
                e = db.findHighestRefreshForBase(baseKey);
                usedFallback = e.has_value();
                if (usedFallback) {
                    LOG() << "CheckAndApplyLightBoost: " << baseKey
                          << " no exact match for refresh=" << timing.etc.rr
                          << " — falling back to highest available: " << (int)e->refresh_int << " Hz";
                }
            }

            if (!e) {
                LOG() << "CheckAndApplyLightBoost: target display EDID '" << baseKey
                      << "' not in nvtimings.json — skipping LightBoost.";
                target_found = true;  // resolved but no DB entry
                break;
            }

            target_found = true;
            primary_display_ids_.push_back(displayId);

            monitor_timings_ = *e;
            monitor_timings_.refresh_int  = usedFallback ? e->refresh_int : timing.etc.rr;
            monitor_timings_.monitor_EDID = baseKey;

            // Cache the original timing so we can confirm the post-revert
            // settle during DisableLightBoost.
            original_target_timing_     = timing;
            has_original_target_timing_ = true;

            // Tight match check — exact HTotal+VTotal+pclk. If already
            // matched, the panel is already in LightBoost mode and we
            // shouldn't reapply the modeset (which itself triggers a
            // disruptive driver event).
            const bool h_match = (timing.HTotal == e->timing.HTotal);
            const bool v_match = (timing.VTotal == e->timing.VTotal);
            const NvU32 pclk_tol = e->timing.pclk ? (std::max<NvU32>)(1u, e->timing.pclk / 1000u) : 1u;
            const bool p_match = (e->timing.pclk == 0) ? true
                : (timing.pclk + pclk_tol >= e->timing.pclk &&
                   timing.pclk <= e->timing.pclk + pclk_tol);
            already_good = (h_match && v_match && p_match);
            if (already_good) {
                LOG() << "CheckAndApplyLightBoost: target timings already match DB entry — "
                      << "LightBoost not needed.";
            } else {
                LOG() << "CheckAndApplyLightBoost: timings MISMATCH. "
                      << "Current VTotal=" << timing.VTotal << " DB VTotal=" << e->timing.VTotal
                      << " Current HTotal=" << timing.HTotal << " DB HTotal=" << e->timing.HTotal
                      << " — will apply LightBoost resolution.";
            }
            break;  // matched target — done
        }
    }

    if (!target_found) {
        LOG() << "CheckAndApplyLightBoost: target NVAPI displayId=" << target_id
              << " was not found via NvAPI_GPU_GetConnectedDisplayIds.";
        return;
    }
    if (primary_display_ids_.empty() || already_good) {
        // Nothing more to do — either no DB match, or the timing is already
        // what we'd apply. Skip the modeset entirely.
        return;
    }

    LOG() << "CheckAndApplyLightBoost: primary_display_ids count=" << primary_display_ids_.size()
          << " (filtered to single target)";

    // Attempt to enable LightBoost with the matched timings.
    EnableLightBoost();
}


void NvStereoDx9Presenter::EnableLightBoost()
{
    NvAPI_Status status = NVAPI_OK;

    if (lightboost_enabled_) return;

    LOG() << "NvStereoDx9Presenter::EnableLightBoost";

    if (primary_display_ids_.empty()) {
        LOG() << "EnableLightBoost: primary_display_ids_ is empty.";
        return;
    }

    std::vector<NvU32>             idArray;
    std::vector<NV_CUSTOM_DISPLAY> displayArray;

    for (NvU32 displayId : primary_display_ids_) {
        // Fetch live timing to pick up HSyncPol, VSyncPol, interlaced, and etc fields.
        NV_TIMING       liveTiming = {};
        NV_TIMING_INPUT current    = {};
        current.version = NV_TIMING_INPUT_VER;

        status = NvAPI_DISP_GetTiming(displayId, &current, &liveTiming);
        if (status != NVAPI_OK) {
            LOG() << "EnableLightBoost: Failed to get live timing for displayId=" << displayId
                  << " Status=" << status << " — skipping.";
            continue;
        }

        // Merge DB-sourced core timing with live polarity/interlaced/etc fields.
        NV_TIMING merged  = monitor_timings_.timing;
        merged.HSyncPol   = liveTiming.HSyncPol;
        merged.VSyncPol   = liveTiming.VSyncPol;
        merged.interlaced = liveTiming.interlaced;
        merged.etc        = liveTiming.etc;
        merged.etc.rr     = monitor_timings_.timing.etc.rr;
        merged.etc.rrx1k  = monitor_timings_.timing.etc.rrx1k;

        LOG() << "EnableLightBoost: display=" << displayId
              << " pclk=" << merged.pclk
              << " HTotal=" << merged.HTotal << " VTotal=" << merged.VTotal
              << " rr=" << merged.etc.rr << " rrx1k=" << merged.etc.rrx1k;

        NV_CUSTOM_DISPLAY cd = {};
        cd.version       = NV_CUSTOM_DISPLAY_VER;
        cd.timing        = merged;
        cd.srcPartition  = { 0.0f, 0.0f, 1.0f, 1.0f };
        cd.width         = merged.HVisible;
        cd.height        = merged.VVisible;
        cd.colorFormat   = NV_FORMAT_A8R8G8B8;
        cd.xRatio        = 1.0f;
        cd.yRatio        = 1.0f;
        cd.depth         = 32;

        idArray.push_back(displayId);
        displayArray.push_back(cd);
    }

    if (idArray.empty()) {
        LOG() << "EnableLightBoost: No displays could be prepared.";
        return;
    }

    // Check for active NVIDIA Surround/Mosaic topology — TryCustomDisplay
    // cannot modify individual displays when they are grouped.
    {
        NV_MOSAIC_TOPO_BRIEF      topoBrief   = {};
        NV_MOSAIC_DISPLAY_SETTING dispSetting = {};
        NvS32 overlapX = 0, overlapY = 0;
        topoBrief.version   = NVAPI_MOSAIC_TOPO_BRIEF_VER;
        dispSetting.version = NVAPI_MOSAIC_DISPLAY_SETTING_VER;

        NvAPI_Status ms = NvAPI_Mosaic_GetCurrentTopo(&topoBrief, &dispSetting, &overlapX, &overlapY);
        if (ms == NVAPI_OK && topoBrief.enabled) {
            LOG() << "EnableLightBoost: Surround/Mosaic active — skipping TryCustomDisplay.";
            return;
        }
    }

    LOG() << "EnableLightBoost: Calling NvAPI_DISP_TryCustomDisplay for "
          << idArray.size() << " display(s).";

    auto posSnapshot = SnapshotDisplayPositions();
    auto wndSnapshot = SnapshotProcessWindows();
    WaitForModesetSettle(100);

    // Snapshot pre-apply timing so we can detect ANY change after
    // TryCustomDisplay — much more robust than exact-match comparison, since
    // NVAPI normalises live timing fields in ways that don't always match
    // what we wrote.
    NV_TIMING preApply = original_target_timing_;

    status = NvAPI_DISP_TryCustomDisplay(idArray.data(),
                                          static_cast<NvU32>(idArray.size()),
                                          displayArray.data());
    if (status != NVAPI_OK) {
        LOG() << "EnableLightBoost: NvAPI_DISP_TryCustomDisplay failed Status=" << status
              << " — reverting.";
        NvAPI_DISP_RevertCustomDisplayTrial(idArray.data(), static_cast<NvU32>(idArray.size()));
        WaitForModesetSettle(500);
        RestoreDisplayPositions(posSnapshot);
        RestoreProcessWindows(wndSnapshot);
        return;
    }

    // Drain WM_DISPLAYCHANGE / let DWM observe the modeset for a beat, then
    // poll for the live timing to differ from the pre-apply snapshot. Most
    // modesets settle in well under 1s; the user has observed worst-case
    // ~10s so we cap there. We do NOT revert on timeout — if GetTiming never
    // reports a change, the modeset probably still took but isn't visible
    // via this API path; ripping it back out would just thrash the driver.
    WaitForModesetSettle(200);
    const bool settled = has_original_target_timing_
        ? WaitForTimingChange(idArray.front(), preApply, 10000)
        : (WaitForModesetSettle(500), false);
    if (!settled) {
        LOG() << "EnableLightBoost: did not observe a timing change via GetTiming "
                 "after the trial — proceeding under the assumption that the "
                 "modeset succeeded (TryCustomDisplay returned OK).";
    }
    RestoreDisplayPositions(posSnapshot);
    RestoreProcessWindows(wndSnapshot);

    lightboost_enabled_ = true;
    LOG() << "EnableLightBoost: NvAPI_DISP_TryCustomDisplay succeeded for "
          << idArray.size() << " display(s).";
}


void NvStereoDx9Presenter::DisableLightBoost()
{
    if (!lightboost_enabled_) return;

    LOG() << "NvStereoDx9Presenter::DisableLightBoost";

    if (primary_display_ids_.empty()) {
        LOG() << "DisableLightBoost: primary_display_ids_ is empty.";
        lightboost_enabled_ = false;
        return;
    }

    std::vector<NvU32> idArray(primary_display_ids_.begin(), primary_display_ids_.end());

    LOG() << "DisableLightBoost: Calling NvAPI_DISP_RevertCustomDisplayTrial for "
          << idArray.size() << " display(s).";

    auto posSnapshot = SnapshotDisplayPositions();
    auto wndSnapshot = SnapshotProcessWindows();

    // Capture pre-revert timing so we can poll for the actual change to land,
    // up to 10s — mirrors EnableLightBoost's polled settle. Without this the
    // D3D9 device release races with the GPU driver still completing the
    // revert, which has been observed to freeze the display on multi-monitor.
    NV_TIMING preRevert = {};
    {
        NV_TIMING_INPUT ti = {}; ti.version = NV_TIMING_INPUT_VER;
        NvAPI_DISP_GetTiming(idArray.front(), &ti, &preRevert);
    }

    NvAPI_Status status = NvAPI_DISP_RevertCustomDisplayTrial(
        idArray.data(), static_cast<NvU32>(idArray.size()));

    LOG() << "DisableLightBoost: NvAPI_DISP_RevertCustomDisplayTrial returned " << status;

    // Minimum drain so DWM processes WM_DISPLAYCHANGE before D3D9 release.
    WaitForModesetSettle(200);
    // Best-effort wait for the timing to change back. As with EnableLightBoost
    // we don't trust an exact-match check (NVAPI normalises live values).
    // WaitForTimingChange returns on ANY change vs. preRevert, which is the
    // signal we actually want here.
    WaitForTimingChange(idArray.front(), preRevert, 3000);

    if (status == NVAPI_OK) {
        RestoreDisplayPositions(posSnapshot);
        RestoreProcessWindows(wndSnapshot);
        LOG() << "DisableLightBoost: revert succeeded.";
    } else if (has_original_devmode_ && !target_gdi_device_.empty()) {
        // Fallback: force a Windows-level modeset to the OS-stored DEVMODE we
        // snapshotted before applying. The NVAPI trial gets invalidated
        // mid-session by FSE D3D9Ex modesets (NV3D activation, FSE entry/exit),
        // and once invalidated RevertCustomDisplayTrial returns -1 while the
        // panel stays at the LightBoost timing. ChangeDisplaySettingsExW
        // pushes a real Windows mode change which the panel honours.
        LOG() << "DisableLightBoost: NVAPI revert failed (trial invalidated) — "
                 "falling back to ChangeDisplaySettingsExW(" << status << ")";
        DEVMODEW dm = original_devmode_;  // copy so we don't mutate the cache
        LONG rc = ChangeDisplaySettingsExW(target_gdi_device_.c_str(), &dm,
                                            nullptr, CDS_FULLSCREEN, nullptr);
        if (rc != DISP_CHANGE_SUCCESSFUL) {
            LOG() << "DisableLightBoost: ChangeDisplaySettingsExW failed rc=" << rc
                  << " — trying CDS_RESET";
            ChangeDisplaySettingsExW(target_gdi_device_.c_str(), &dm,
                                      nullptr, CDS_RESET | CDS_FULLSCREEN, nullptr);
        }
        WaitForModesetSettle(500);
        RestoreDisplayPositions(posSnapshot);
        RestoreProcessWindows(wndSnapshot);
    }

    lightboost_enabled_ = false;
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

