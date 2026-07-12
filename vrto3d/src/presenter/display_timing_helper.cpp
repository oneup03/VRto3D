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


#include "display_timing_helper.h"
#include "display_utils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "platform.h"
#include "vrto3dlib/debug_log.hpp"

// NVAPI include must be at file scope (before any C++ namespace) so its
// typedefs (NvU32, NvAPI_Status, …) land in the global namespace, not in
// vrto3d. This is the only NVAPI consumer left in the driver itself — the
// NvidiaDX9 presenter's NVAPI usage now lives inside NV3D-Lib.
#include "nvapi.h"

// AMD ADL — pure C headers, no extra namespace concerns.
#include "adl_sdk.h"
#include "adl_structures.h"
#include "adl_defines.h"

namespace vrto3d {

// ===========================================================================
// Shared helpers
// ===========================================================================

namespace {

using display_utils::DisplayPositionSnapshot;
using display_utils::SnapshotDisplayPositions;
using display_utils::RestoreDisplayPositions;
using display_utils::WaitForModesetSettle;

// Resolve the GDI device name for a given display_index.
std::wstring ResolveDeviceName(int32_t display_index)
{
    auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) return {};

    size_t idx = 0;
    if (display_index > 0 && display_index <= static_cast<int32_t>(monitors.size())) {
        idx = static_cast<size_t>(display_index - 1);
    }

    std::wstring wide;
    const auto& name = monitors[idx].device_name;
    int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    wide.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), n);
    return wide;
}

// Snapshot the current DEVMODE so we can force the desktop back to its
// original resolution after a backend revert. NVAPI's RevertCustomDisplayTrial
// usually does this on its own, but if the user reboots / kills the process
// between Apply and Revert we want a stable fallback for next launch too.
bool SnapshotCurrentMode(const std::wstring& device_name, DEVMODEW& out_mode)
{
    out_mode = {};
    out_mode.dmSize = sizeof(out_mode);
    return EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &out_mode, 0) != 0;
}

// Restore desktop to a saved DEVMODE. Returns true on success or no-op.
bool RestoreSavedMode(const std::wstring& device_name, DEVMODEW& saved_mode)
{
    if (device_name.empty() || saved_mode.dmSize == 0) return false;

    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &current, 0)) {
        // Skip the modeset if we're already back at the saved mode.
        if (current.dmPelsWidth        == saved_mode.dmPelsWidth
         && current.dmPelsHeight       == saved_mode.dmPelsHeight
         && current.dmDisplayFrequency == saved_mode.dmDisplayFrequency) {
            return true;
        }
    }

    LONG result = ChangeDisplaySettingsExW(
        device_name.c_str(), &saved_mode, nullptr, CDS_FULLSCREEN, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        LOG() << "RestoreSavedMode: ChangeDisplaySettingsExW failed result=" << result
              << " — falling back to driver default";
        ChangeDisplaySettingsExW(device_name.c_str(), nullptr, nullptr, 0, nullptr);
    }
    return result == DISP_CHANGE_SUCCESSFUL;
}

// Compute pixel clock in 10 kHz units from timing spec.
uint32_t ComputePixelClock10kHz(const FramePackTimingSpec& spec)
{
    // pclk = h_total * v_total * refresh_hz, in 10 kHz units
    double pclk_hz = static_cast<double>(spec.h_total)
                   * static_cast<double>(spec.v_total)
                   * static_cast<double>(spec.refresh_hz);
    return static_cast<uint32_t>(std::round(pclk_hz / 10000.0));
}

}  // namespace


// ===========================================================================
// Backend state structures
// ===========================================================================

struct DisplayTimingHelper::NvidiaState {
    std::vector<NvU32> display_ids;
    std::wstring       device_name;
    DEVMODEW           original_mode{};
};

struct DisplayTimingHelper::AmdState {
    int          adapter_index  = -1;
    int          display_index  = -1;
    std::wstring device_name;
    DEVMODEW     original_mode{};
};

struct DisplayTimingHelper::CruState {
    std::wstring device_name;
    DEVMODEW     original_mode{};
};


// ===========================================================================
// DisplayTimingHelper lifecycle
// ===========================================================================

DisplayTimingHelper::~DisplayTimingHelper()
{
    Revert();
}


bool DisplayTimingHelper::Apply(const FramePackTimingSpec& spec,
                                int32_t display_index)
{
    if (active_) {
        LOG() << "DisplayTimingHelper::Apply: already active, reverting first";
        Revert();
    }

    LOG() << "DisplayTimingHelper::Apply: "
          << spec.active_w << "x" << spec.active_h
          << " @" << spec.refresh_hz << "Hz"
          << " h_total=" << spec.h_total << " v_total=" << spec.v_total
          << " display_index=" << display_index;

    // Try backends in priority order.
    if (TryNvidia(spec, display_index)) {
        backend_ = Backend::Nvidia;
        active_  = true;
        LOG() << "DisplayTimingHelper: NVIDIA backend succeeded";
        return true;
    }

    if (TryAmd(spec, display_index)) {
        backend_ = Backend::Amd;
        active_  = true;
        LOG() << "DisplayTimingHelper: AMD backend succeeded";
        return true;
    }

    if (TryCruFallback(spec, display_index)) {
        backend_ = Backend::CruFallback;
        active_  = true;
        LOG() << "DisplayTimingHelper: CRU fallback succeeded";
        return true;
    }

    LOG() << "DisplayTimingHelper::Apply: all backends failed. "
          << "The display is in its original mode. Frame-pack TaB rendering "
          << "will still work visually, but the HDMI 3D InfoFrame may not be sent. "
          << "Consider using CRU (Custom Resolution Utility) to pre-add the timing.";
    return false;
}


void DisplayTimingHelper::Revert()
{
    if (!active_) return;

    LOG() << "DisplayTimingHelper::Revert (backend="
          << static_cast<int>(backend_) << ")";

    switch (backend_) {
        case Backend::Nvidia:      RevertNvidia();      break;
        case Backend::Amd:         RevertAmd();         break;
        case Backend::CruFallback: RevertCruFallback(); break;
        default: break;
    }

    // Free backend state.
    if (backend_state_) {
        switch (backend_) {
            case Backend::Nvidia:
                delete static_cast<NvidiaState*>(backend_state_);
                break;
            case Backend::Amd:
                delete static_cast<AmdState*>(backend_state_);
                break;
            case Backend::CruFallback:
                delete static_cast<CruState*>(backend_state_);
                break;
            default: break;
        }
        backend_state_ = nullptr;
    }

    active_  = false;
    backend_ = Backend::None;
}


// ===========================================================================
// NVIDIA backend — NvAPI_DISP_TryCustomDisplay (delay-loaded via nvapi64.lib)
// ===========================================================================

namespace {

// SEH-guarded probe for delay-loaded nvapi64.dll — same pattern NV3D-Lib
// uses inside CreateInterfaceDX11 (a missing DLL raises a delay-load
// exception instead of returning an error).
bool NvApiAvailable()
{
    __try {
        NvAPI_Status status = NvAPI_Initialize();
        return (status == NVAPI_OK);
    } __except (
        (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
         GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH
    ) {
        return false;
    }
}

}  // namespace


bool DisplayTimingHelper::TryNvidia(const FramePackTimingSpec& spec,
                                     int32_t display_index)
{
    if (!NvApiAvailable()) {
        LOG() << "DisplayTimingHelper::TryNvidia: NVAPI not available";
        return false;
    }

    // Map the GDI device name for display_index to an NVAPI displayId.
    std::wstring w_name = ResolveDeviceName(display_index);
    if (w_name.empty()) {
        LOG() << "DisplayTimingHelper::TryNvidia: could not resolve device name for index "
              << display_index;
        return false;
    }

    // GDI device names are pure ASCII (e.g. "\\.\DISPLAY1") — cast is safe.
    char narrow_name[NVAPI_SHORT_STRING_MAX] = {};
    for (size_t i = 0; i < w_name.size() && i < sizeof(narrow_name) - 1; ++i)
        narrow_name[i] = static_cast<char>(w_name[i]);

    NvU32 displayId = 0;
    NvAPI_Status status = NvAPI_DISP_GetDisplayIdByDisplayName(narrow_name, &displayId);
    if (status != NVAPI_OK) {
        LOG() << "DisplayTimingHelper::TryNvidia: NvAPI_DISP_GetDisplayIdByDisplayName failed"
              << " name='" << narrow_name << "' status=" << status;
        return false;
    }

    // Check for active Surround/Mosaic topology — TryCustomDisplay cannot
    // change individual displays while they are grouped.
    {
        NV_MOSAIC_TOPO_BRIEF      topoBrief   = {};
        NV_MOSAIC_DISPLAY_SETTING dispSetting = {};
        NvS32 overlapX = 0, overlapY = 0;
        topoBrief.version   = NVAPI_MOSAIC_TOPO_BRIEF_VER;
        dispSetting.version = NVAPI_MOSAIC_DISPLAY_SETTING_VER;
        NvAPI_Status ms = NvAPI_Mosaic_GetCurrentTopo(&topoBrief, &dispSetting, &overlapX, &overlapY);
        if (ms == NVAPI_OK && topoBrief.enabled) {
            LOG() << "DisplayTimingHelper::TryNvidia: Surround/Mosaic active — skipping.";
            return false;
        }
    }

    // Snapshot the desktop mode before we touch anything so Revert can force
    // a return to it even if the NVAPI trial revert leaves the desktop in an
    // intermediate state.
    DEVMODEW original_mode{};
    SnapshotCurrentMode(w_name, original_mode);

    // Fetch the live timing so we can merge polarity and interlaced flags.
    NV_TIMING live_timing = {};
    {
        NV_TIMING_INPUT ti = {};
        ti.version = NV_TIMING_INPUT_VER;
        NvAPI_Status ts = NvAPI_DISP_GetTiming(displayId, &ti, &live_timing);
        if (ts != NVAPI_OK) {
            LOG() << "DisplayTimingHelper::TryNvidia: NvAPI_DISP_GetTiming failed status=" << ts
                  << " — proceeding with default polarity";
        }
    }

    // Build NV_TIMING from the frame-pack spec, merging live polarity fields.
    NV_TIMING t = {};
    t.pclk        = static_cast<NvU32>(ComputePixelClock10kHz(spec));
    t.HTotal      = spec.h_total;
    t.HVisible    = static_cast<NvU16>(spec.active_w);
    t.HBorder     = 0;
    t.HFrontPorch = spec.h_front_porch;
    t.HSyncWidth  = spec.h_sync_width;
    t.HSyncPol    = live_timing.HSyncPol;
    t.VTotal      = spec.v_total;
    t.VVisible    = static_cast<NvU16>(spec.active_h);
    t.VBorder     = 0;
    t.VFrontPorch = spec.v_front_porch;
    t.VSyncWidth  = spec.v_sync_width;
    t.VSyncPol    = live_timing.VSyncPol;
    t.interlaced  = 0;
    t.etc.rr      = static_cast<NvU16>(spec.refresh_hz + 0.5f);
    t.etc.rrx1k   = static_cast<NvU32>(spec.refresh_hz * 1000.0f + 0.5f);

    NV_CUSTOM_DISPLAY cd = {};
    cd.version      = NV_CUSTOM_DISPLAY_VER;
    cd.timing       = t;
    cd.srcPartition = { 0.0f, 0.0f, 1.0f, 1.0f };
    cd.width        = spec.active_w;
    cd.height       = spec.active_h;
    cd.colorFormat  = NV_FORMAT_A8R8G8B8;
    cd.xRatio       = 1.0f;
    cd.yRatio       = 1.0f;
    cd.depth        = 32;

    LOG() << "DisplayTimingHelper::TryNvidia: applying "
          << spec.active_w << "x" << spec.active_h << "@" << spec.refresh_hz << "Hz"
          << " pclk=" << t.pclk << " HTotal=" << t.HTotal << " VTotal=" << t.VTotal
          << " displayId=" << displayId;

    auto posSnapshot = SnapshotDisplayPositions();
    WaitForModesetSettle(100);

    NvU32          ids[1] = { displayId };
    NV_CUSTOM_DISPLAY cds[1] = { cd };
    status = NvAPI_DISP_TryCustomDisplay(ids, 1, cds);
    if (status != NVAPI_OK) {
        LOG() << "DisplayTimingHelper::TryNvidia: NvAPI_DISP_TryCustomDisplay failed status="
              << status << " — reverting trial";
        NvAPI_DISP_RevertCustomDisplayTrial(ids, 1);
        WaitForModesetSettle(500);
        RestoreDisplayPositions(posSnapshot);
        return false;
    }

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);

    auto* state = new NvidiaState();
    state->display_ids.push_back(displayId);
    state->device_name   = w_name;
    state->original_mode = original_mode;
    backend_state_ = state;

    LOG() << "DisplayTimingHelper::TryNvidia: succeeded displayId=" << displayId;
    return true;
}


void DisplayTimingHelper::RevertNvidia()
{
    if (!backend_state_) return;
    auto* state = static_cast<NvidiaState*>(backend_state_);

    if (state->display_ids.empty()) return;

    LOG() << "DisplayTimingHelper::RevertNvidia: reverting "
          << state->display_ids.size() << " display(s)";

    auto posSnapshot = SnapshotDisplayPositions();
    WaitForModesetSettle(100);

    NvAPI_DISP_RevertCustomDisplayTrial(state->display_ids.data(),
                                        static_cast<NvU32>(state->display_ids.size()));

    WaitForModesetSettle(500);

    // Force the desktop back to the original mode if the trial revert left it
    // somewhere else (e.g. the driver picked a fallback mode after dropping
    // the custom timing).
    if (!state->device_name.empty()) {
        RestoreSavedMode(state->device_name, state->original_mode);
        WaitForModesetSettle(300);
    }

    RestoreDisplayPositions(posSnapshot);
}


// ===========================================================================
// AMD backend — ADL_Display_ModeTimingOverride_Set
// ===========================================================================

namespace {

// ADL entry points we resolve at runtime. Names match the exports of
// atiadlxx.dll / atiadlxy.dll exactly. The DLL uses default (cdecl) calling
// convention for its exports; only the malloc callback is __stdcall.
using ADL_MAIN_CONTROL_CREATE_t          = int (*)(ADL_MAIN_MALLOC_CALLBACK, int);
using ADL_MAIN_CONTROL_DESTROY_t         = int (*)();
using ADL_ADAPTER_NUMBEROFADAPTERS_GET_t = int (*)(int*);
using ADL_ADAPTER_ADAPTERINFO_GET_t      = int (*)(LPAdapterInfo, int);
using ADL_DISPLAY_DISPLAYINFO_GET_t      = int (*)(int, int*, ADLDisplayInfo**, int);
using ADL_DISPLAY_MODETIMINGOVERRIDE_SET_t =
    int (*)(int iAdapterIndex, int iDisplayIndex,
            ADLDisplayModeInfo* lpMode, int iForceUpdate);
using ADL_DISPLAY_MODETIMINGOVERRIDE_DELETE_t =
    int (*)(int iAdapterIndex, int iDisplayIndex,
            ADLDisplayMode* lpMode, int iForceUpdate);

// ADL's required malloc callback. __stdcall on Windows.
void* __stdcall AdlMallocCallback(int sz) { return malloc(static_cast<size_t>(sz)); }

HMODULE LoadAdlLibrary()
{
    HMODULE hmod = LoadLibraryW(L"atiadlxx.dll");
    if (!hmod) hmod = LoadLibraryW(L"atiadlxy.dll");
    return hmod;
}

// RAII wrapper for an ADL session so we always destroy on scope exit.
class AdlSession {
public:
    AdlSession() = default;
    ~AdlSession() {
        if (destroy_) destroy_();
        if (dll_)     FreeLibrary(dll_);
    }

    bool Init() {
        dll_ = LoadAdlLibrary();
        if (!dll_) return false;

        auto pfnCreate = reinterpret_cast<ADL_MAIN_CONTROL_CREATE_t>(
            GetProcAddress(dll_, "ADL_Main_Control_Create"));
        destroy_ = reinterpret_cast<ADL_MAIN_CONTROL_DESTROY_t>(
            GetProcAddress(dll_, "ADL_Main_Control_Destroy"));
        if (!pfnCreate || !destroy_) {
            FreeLibrary(dll_); dll_ = nullptr;
            return false;
        }
        if (pfnCreate(AdlMallocCallback, 1) != ADL_OK) {
            destroy_ = nullptr;
            FreeLibrary(dll_); dll_ = nullptr;
            return false;
        }

        num_adapters_get_ = reinterpret_cast<ADL_ADAPTER_NUMBEROFADAPTERS_GET_t>(
            GetProcAddress(dll_, "ADL_Adapter_NumberOfAdapters_Get"));
        adapter_info_get_ = reinterpret_cast<ADL_ADAPTER_ADAPTERINFO_GET_t>(
            GetProcAddress(dll_, "ADL_Adapter_AdapterInfo_Get"));
        display_info_get_ = reinterpret_cast<ADL_DISPLAY_DISPLAYINFO_GET_t>(
            GetProcAddress(dll_, "ADL_Display_DisplayInfo_Get"));
        timing_set_       = reinterpret_cast<ADL_DISPLAY_MODETIMINGOVERRIDE_SET_t>(
            GetProcAddress(dll_, "ADL_Display_ModeTimingOverride_Set"));
        timing_delete_    = reinterpret_cast<ADL_DISPLAY_MODETIMINGOVERRIDE_DELETE_t>(
            GetProcAddress(dll_, "ADL_Display_ModeTimingOverride_Delete"));

        return num_adapters_get_ && adapter_info_get_
            && display_info_get_ && timing_set_;
    }

    int NumAdapters(int* out) const { return num_adapters_get_(out); }
    int AdapterInfoGet(LPAdapterInfo info, int size) const { return adapter_info_get_(info, size); }
    int DisplayInfoGet(int adapter, int* num, ADLDisplayInfo** info, int force) const {
        return display_info_get_(adapter, num, info, force);
    }
    int TimingSet(int adapter, int display, ADLDisplayModeInfo* mode, int force) const {
        return timing_set_(adapter, display, mode, force);
    }
    int TimingDelete(int adapter, int display, ADLDisplayMode* mode, int force) const {
        return timing_delete_ ? timing_delete_(adapter, display, mode, force) : ADL_ERR;
    }
    bool HasDelete() const { return timing_delete_ != nullptr; }

private:
    HMODULE dll_ = nullptr;
    ADL_MAIN_CONTROL_DESTROY_t          destroy_           = nullptr;
    ADL_ADAPTER_NUMBEROFADAPTERS_GET_t  num_adapters_get_  = nullptr;
    ADL_ADAPTER_ADAPTERINFO_GET_t       adapter_info_get_  = nullptr;
    ADL_DISPLAY_DISPLAYINFO_GET_t       display_info_get_  = nullptr;
    ADL_DISPLAY_MODETIMINGOVERRIDE_SET_t    timing_set_    = nullptr;
    ADL_DISPLAY_MODETIMINGOVERRIDE_DELETE_t timing_delete_ = nullptr;
};

// Resolve (adapter_index, display_logical_index) for the target Windows
// display. AdapterInfo.iOSDisplayIndex is generated from EnumDisplayDevices,
// so display_index 1 (= "\\.\DISPLAY1") corresponds to iOSDisplayIndex 0.
bool FindAdlAdapterDisplay(const AdlSession& adl,
                           int32_t target_display_index,
                           int& out_adapter,
                           int& out_display)
{
    int num_adapters = 0;
    if (adl.NumAdapters(&num_adapters) != ADL_OK || num_adapters <= 0) {
        LOG() << "FindAdlAdapterDisplay: no ADL adapters";
        return false;
    }

    std::vector<AdapterInfo> adapters(static_cast<size_t>(num_adapters));
    std::memset(adapters.data(), 0, adapters.size() * sizeof(AdapterInfo));
    if (adl.AdapterInfoGet(adapters.data(),
                           static_cast<int>(adapters.size() * sizeof(AdapterInfo))) != ADL_OK) {
        LOG() << "FindAdlAdapterDisplay: AdapterInfo_Get failed";
        return false;
    }

    const int target_os_index = target_display_index - 1;   // 1-based → 0-based

    // First pass: prefer an adapter whose iOSDisplayIndex matches our target.
    // Fall back to the first present adapter so single-GPU systems still work
    // when the OS index isn't populated.
    int adapter_index = -1;
    for (const auto& a : adapters) {
        if (!a.iPresent) continue;
        if (a.iOSDisplayIndex == target_os_index) {
            adapter_index = a.iAdapterIndex;
            break;
        }
    }
    if (adapter_index < 0) {
        for (const auto& a : adapters) {
            if (a.iPresent) { adapter_index = a.iAdapterIndex; break; }
        }
    }
    if (adapter_index < 0) {
        LOG() << "FindAdlAdapterDisplay: no present adapter (likely no AMD GPU)";
        return false;
    }

    // Enumerate displays on that adapter; pick the first connected+mapped one.
    int             num_displays = 0;
    ADLDisplayInfo* display_info = nullptr;
    if (adl.DisplayInfoGet(adapter_index, &num_displays, &display_info, 0) != ADL_OK
        || num_displays <= 0 || !display_info) {
        LOG() << "FindAdlAdapterDisplay: DisplayInfo_Get failed for adapter " << adapter_index;
        return false;
    }

    int display_index = -1;
    constexpr int kMappedMask =
        ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED |
        ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED;
    for (int i = 0; i < num_displays; ++i) {
        if ((display_info[i].iDisplayInfoValue & kMappedMask) == kMappedMask) {
            display_index = display_info[i].displayID.iDisplayLogicalIndex;
            break;
        }
    }
    free(display_info);   // allocated by AdlMallocCallback

    if (display_index < 0) {
        LOG() << "FindAdlAdapterDisplay: no mapped display on adapter " << adapter_index;
        return false;
    }

    out_adapter = adapter_index;
    out_display = display_index;
    return true;
}

// Build an ADLDisplayModeInfo from our timing spec.
void FillAdlModeInfo(const FramePackTimingSpec& spec, ADLDisplayModeInfo& out)
{
    out = {};
    out.iTimingStandard  = ADL_DL_MODETIMING_STANDARD_CUSTOM;
    out.iPossibleStandard = 0;
    out.iRefreshRate     = static_cast<int>(spec.refresh_hz + 0.5f);
    out.iPelsWidth       = spec.active_w;
    out.iPelsHeight      = spec.active_h;

    ADLDetailedTiming& dt = out.sDetailedTiming;
    dt.iSize         = sizeof(dt);
    dt.sTimingFlags  = 0;  // progressive, no double-scan, polarity default (+,+)
    dt.sHTotal       = static_cast<short>(spec.h_total);
    dt.sHDisplay     = static_cast<short>(spec.active_w);
    dt.sHSyncStart   = static_cast<short>(spec.h_front_porch);  // offset from active end
    dt.sHSyncWidth   = static_cast<short>(spec.h_sync_width);
    dt.sVTotal       = static_cast<short>(spec.v_total);
    dt.sVDisplay     = static_cast<short>(spec.active_h);
    dt.sVSyncStart   = static_cast<short>(spec.v_front_porch);
    dt.sVSyncWidth   = static_cast<short>(spec.v_sync_width);
    dt.sPixelClock   = static_cast<short>(ComputePixelClock10kHz(spec));
}

}  // namespace


bool DisplayTimingHelper::TryAmd(const FramePackTimingSpec& spec,
                                  int32_t display_index)
{
    AdlSession adl;
    if (!adl.Init()) {
        LOG() << "DisplayTimingHelper::TryAmd: ADL not available (not AMD GPU?)";
        return false;
    }

    int adapter = -1, ddisplay = -1;
    if (!FindAdlAdapterDisplay(adl, display_index, adapter, ddisplay)) {
        return false;
    }

    std::wstring w_name = ResolveDeviceName(display_index);
    DEVMODEW original_mode{};
    SnapshotCurrentMode(w_name, original_mode);

    ADLDisplayModeInfo mode_info{};
    FillAdlModeInfo(spec, mode_info);

    LOG() << "DisplayTimingHelper::TryAmd: applying timing override "
          << spec.active_w << "x" << spec.active_h << "@" << spec.refresh_hz << "Hz"
          << " adapter=" << adapter << " display=" << ddisplay
          << " HTotal=" << mode_info.sDetailedTiming.sHTotal
          << " VTotal=" << mode_info.sDetailedTiming.sVTotal
          << " pclk(10kHz)=" << static_cast<unsigned>(static_cast<uint16_t>(mode_info.sDetailedTiming.sPixelClock));

    auto posSnapshot = SnapshotDisplayPositions();
    WaitForModesetSettle(100);

    int rc = adl.TimingSet(adapter, ddisplay, &mode_info, /*iForceUpdate=*/1);
    if (rc != ADL_OK && rc != ADL_OK_WARNING && rc != ADL_OK_MODE_CHANGE) {
        LOG() << "DisplayTimingHelper::TryAmd: ADL_Display_ModeTimingOverride_Set failed rc="
              << rc;
        return false;
    }

    // ADL_Display_ModeTimingOverride_Set adds the timing to the driver's
    // override list but doesn't necessarily switch into it; trigger the
    // modeset via the OS.
    if (!w_name.empty()) {
        DEVMODEW dm{};
        dm.dmSize             = sizeof(dm);
        dm.dmPelsWidth        = spec.active_w;
        dm.dmPelsHeight       = spec.active_h;
        dm.dmDisplayFrequency = static_cast<DWORD>(spec.refresh_hz + 0.5f);
        dm.dmBitsPerPel       = 32;
        dm.dmFields           = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        ChangeDisplaySettingsExW(w_name.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr);
    }

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);

    auto* state = new AmdState();
    state->adapter_index = adapter;
    state->display_index = ddisplay;
    state->device_name   = w_name;
    state->original_mode = original_mode;
    backend_state_ = state;

    LOG() << "DisplayTimingHelper::TryAmd: succeeded (adapter=" << adapter
          << " display=" << ddisplay << ")";
    return true;
}


void DisplayTimingHelper::RevertAmd()
{
    if (!backend_state_) return;
    auto* state = static_cast<AmdState*>(backend_state_);

    LOG() << "DisplayTimingHelper::RevertAmd: reverting (adapter="
          << state->adapter_index << " display=" << state->display_index << ")";

    auto posSnapshot = SnapshotDisplayPositions();

    // Restore the OS-side mode first so the desktop is back at the user's
    // original resolution by the time we tear the override out.
    if (!state->device_name.empty()) {
        RestoreSavedMode(state->device_name, state->original_mode);
        WaitForModesetSettle(300);
    }

    // Now remove our override from the driver's list. Best effort; both
    // Delete and a STANDARD_DRIVER_DEFAULT Set are valid revert strokes
    // depending on the ADL version.
    AdlSession adl;
    if (adl.Init()) {
        if (adl.HasDelete()) {
            ADLDisplayMode mode{};
            mode.iPelsWidth        = state->original_mode.dmPelsWidth;
            mode.iPelsHeight       = state->original_mode.dmPelsHeight;
            mode.iDisplayFrequency = state->original_mode.dmDisplayFrequency;
            mode.iBitsPerPel       = 32;
            adl.TimingDelete(state->adapter_index, state->display_index, &mode, 1);
        }
        ADLDisplayModeInfo info{};
        info.iTimingStandard = ADL_DL_MODETIMING_STANDARD_DRIVER_DEFAULT;
        info.iPelsWidth      = state->original_mode.dmPelsWidth;
        info.iPelsHeight     = state->original_mode.dmPelsHeight;
        info.iRefreshRate    = state->original_mode.dmDisplayFrequency;
        info.sDetailedTiming.iSize = sizeof(info.sDetailedTiming);
        adl.TimingSet(state->adapter_index, state->display_index, &info, 1);
    }

    WaitForModesetSettle(300);
    RestoreDisplayPositions(posSnapshot);
}


// ===========================================================================
// CRU fallback — ChangeDisplaySettingsExW
//
// This works when the user has pre-added the custom resolution via CRU
// (Custom Resolution Utility). The driver already knows the mode; we just
// need to switch to it.
// ===========================================================================

bool DisplayTimingHelper::TryCruFallback(const FramePackTimingSpec& spec,
                                          int32_t display_index)
{
    std::wstring device_name = ResolveDeviceName(display_index);
    if (device_name.empty()) {
        LOG() << "DisplayTimingHelper::TryCruFallback: could not resolve device name";
        return false;
    }

    // Save the current display mode for revert.
    DEVMODEW original{};
    original.dmSize = sizeof(original);
    if (!EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &original, 0)) {
        LOG() << "DisplayTimingHelper::TryCruFallback: EnumDisplaySettingsExW(current) failed";
        return false;
    }

    // Check if the target resolution exists in the mode list.
    // ChangeDisplaySettingsExW will fail if the mode hasn't been pre-added via CRU.
    bool mode_found = false;
    DEVMODEW candidate{};
    candidate.dmSize = sizeof(candidate);
    for (DWORD i = 0; EnumDisplaySettingsExW(device_name.c_str(), i, &candidate, 0); ++i) {
        if (candidate.dmPelsWidth  == spec.active_w &&
            candidate.dmPelsHeight == spec.active_h &&
            candidate.dmDisplayFrequency == static_cast<DWORD>(spec.refresh_hz + 0.5f)) {
            mode_found = true;
            break;
        }
    }

    if (!mode_found) {
        LOG() << "DisplayTimingHelper::TryCruFallback: mode "
              << spec.active_w << "x" << spec.active_h
              << "@" << spec.refresh_hz << "Hz not found in display mode list. "
              << "Use CRU to add it first.";
        return false;
    }

    LOG() << "DisplayTimingHelper::TryCruFallback: mode found, switching";

    auto posSnapshot = SnapshotDisplayPositions();

    DEVMODEW dm{};
    dm.dmSize             = sizeof(dm);
    dm.dmPelsWidth        = spec.active_w;
    dm.dmPelsHeight       = spec.active_h;
    dm.dmDisplayFrequency = static_cast<DWORD>(spec.refresh_hz + 0.5f);
    dm.dmBitsPerPel       = 32;
    dm.dmFields           = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;

    LONG result = ChangeDisplaySettingsExW(
        device_name.c_str(), &dm,
        nullptr,
        CDS_FULLSCREEN,
        nullptr);

    if (result != DISP_CHANGE_SUCCESSFUL) {
        LOG() << "DisplayTimingHelper::TryCruFallback: ChangeDisplaySettingsExW failed result="
              << result;
        return false;
    }

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);

    // Save state for revert.
    auto* state = new CruState();
    state->device_name  = device_name;
    state->original_mode = original;
    backend_state_ = state;

    LOG() << "DisplayTimingHelper::TryCruFallback: successfully switched to "
          << spec.active_w << "x" << spec.active_h
          << "@" << spec.refresh_hz << "Hz";
    return true;
}


void DisplayTimingHelper::RevertCruFallback()
{
    if (!backend_state_) return;
    auto* state = static_cast<CruState*>(backend_state_);

    LOG() << "DisplayTimingHelper::RevertCruFallback: reverting to original mode";

    auto posSnapshot = SnapshotDisplayPositions();

    RestoreSavedMode(state->device_name, state->original_mode);

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);
}

}  // namespace vrto3d
