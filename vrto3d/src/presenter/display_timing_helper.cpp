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
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "platform.h"
#include "vrto3dlib/debug_log.hpp"

// NVAPI include must be at file scope (before any C++ namespace) so its
// typedefs (NvU32, NvAPI_Status, …) land in the global namespace, not in
// vrto3d. This mirrors how nvstereo_dx9_presenter.cpp includes the header.
#include "nvapi.h"

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
};

struct DisplayTimingHelper::AmdState {
    int adapter_index = -1;
    int display_index = -1;
    // Original timing override data would go here; for now we just track
    // that we set a timing so Revert can clear it.
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
        case Backend::Nvidia:     RevertNvidia();      break;
        case Backend::Amd:        RevertAmd();         break;
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

// SEH-guarded probe for delay-loaded nvapi64.dll — identical pattern to
// NvStereoDx9Presenter::TryNvAPIInitializeSEH.
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
    RestoreDisplayPositions(posSnapshot);
}


// ===========================================================================
// AMD backend — ADL_Display_ModeTimingOverride_Set
// ===========================================================================

namespace {

// ADL function typedefs for delay-loading atiadlxx.dll / atiadlxy.dll.
using ADL_MAIN_CONTROL_CREATE_t  = int (__cdecl *)(void* (*)(int), int);
using ADL_MAIN_CONTROL_DESTROY_t = int (__cdecl *)();
using ADL_ADAPTER_NUMBEROFADAPTERS_GET_t = int (__cdecl *)(int*);

// ADL_Display_ModeTimingOverride_Set signature:
//   int ADL_Display_ModeTimingOverride_Set(int iAdapterIndex, int iDisplayIndex,
//                                          ADLDisplayModeX2* lpMode,
//                                          int iForceUpdate);
// We'd need the ADLDisplayModeX2 struct definition here. For now, we treat
// AMD as a future implementation placeholder.

HMODULE LoadAdlLibrary()
{
    HMODULE hmod = LoadLibraryW(L"atiadlxx.dll");
    if (!hmod) hmod = LoadLibraryW(L"atiadlxy.dll");
    return hmod;
}

}  // namespace


bool DisplayTimingHelper::TryAmd(const FramePackTimingSpec& spec,
                                  int32_t display_index)
{
    HMODULE hAdl = LoadAdlLibrary();
    if (!hAdl) {
        LOG() << "DisplayTimingHelper::TryAmd: ADL library not found (not AMD GPU)";
        return false;
    }

    LOG() << "DisplayTimingHelper::TryAmd: ADL library loaded";

    auto pfnCreate = reinterpret_cast<ADL_MAIN_CONTROL_CREATE_t>(
        GetProcAddress(hAdl, "ADL_Main_Control_Create"));
    auto pfnDestroy = reinterpret_cast<ADL_MAIN_CONTROL_DESTROY_t>(
        GetProcAddress(hAdl, "ADL_Main_Control_Destroy"));

    if (!pfnCreate || !pfnDestroy) {
        LOG() << "DisplayTimingHelper::TryAmd: ADL entry points not found";
        FreeLibrary(hAdl);
        return false;
    }

    // Initialize ADL with malloc callback.
    auto adl_malloc = [](int size) -> void* { return malloc(size); };
    int adl_status = pfnCreate(adl_malloc, 1);
    if (adl_status != 0) {
        LOG() << "DisplayTimingHelper::TryAmd: ADL_Main_Control_Create failed status="
              << adl_status;
        FreeLibrary(hAdl);
        return false;
    }

    // Look up ADL_Display_ModeTimingOverride_Set.
    using ADL_Display_ModeTimingOverride_Set_t =
        int (__cdecl *)(int iAdapterIndex, int iDisplayIndex,
                        void* lpMode, int iForceUpdate);

    auto pfnSetTiming = reinterpret_cast<ADL_Display_ModeTimingOverride_Set_t>(
        GetProcAddress(hAdl, "ADL_Display_ModeTimingOverride_Set"));

    if (!pfnSetTiming) {
        LOG() << "DisplayTimingHelper::TryAmd: ADL_Display_ModeTimingOverride_Set not found "
              << "— this ADL version may not support custom timing overrides";
        pfnDestroy();
        FreeLibrary(hAdl);
        return false;
    }

    // AMD users should pre-configure via CRU (Custom Resolution Utility).
    // Full ADLDisplayModeX2 implementation is a future task.
    LOG() << "DisplayTimingHelper::TryAmd: AMD timing override not yet fully implemented. "
          << "Use CRU (Custom Resolution Utility) to pre-add the frame-pack resolution.";

    pfnDestroy();
    FreeLibrary(hAdl);
    return false;
}


void DisplayTimingHelper::RevertAmd()
{
    if (!backend_state_) return;

    LOG() << "DisplayTimingHelper::RevertAmd: reverting AMD timing override";

    // TODO: Load ADL, call ADL_Display_ModeTimingOverride_Delete or reset to
    // the original timing.

    auto* state = static_cast<AmdState*>(backend_state_);
    (void)state;  // suppress unused warning until implemented
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

    LONG result = ChangeDisplaySettingsExW(
        state->device_name.c_str(),
        &state->original_mode,
        nullptr,
        CDS_FULLSCREEN,
        nullptr);

    if (result != DISP_CHANGE_SUCCESSFUL) {
        // Fallback: reset to default.
        LOG() << "DisplayTimingHelper::RevertCruFallback: revert failed result="
              << result << ", trying default";
        ChangeDisplaySettingsExW(state->device_name.c_str(), nullptr, nullptr, 0, nullptr);
    }

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);
}

}  // namespace vrto3d

