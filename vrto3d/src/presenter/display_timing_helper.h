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


#include <cstdint>
#include <string>

#include "vrto3dlib/stereo_config.h"

namespace vrto3d {

// ---------------------------------------------------------------------------
// DisplayTimingHelper — applies and reverts custom display timings for
// HDMI 1.4 frame-packing modes.
//
// Strategy priority:
//   1. NVIDIA  — NvAPI_DISP_TryCustomDisplay  (delay-loaded, safe on non-NV)
//   2. AMD     — ADL_Display_ModeTimingOverride_Set  (delay-loaded)
//   3. Fallback — ChangeDisplaySettingsExW  (for CRU pre-configured modes)
//
// Intel is intentionally not implemented: IGCL's ctlGetSetCustomMode only
// takes (width, height) and leaves timing derivation to the driver, so it
// can't produce HDMI 1.4 frame-pack timing on its own. Intel users should
// pre-configure via CRU and hit the CRU fallback.
//
// All three paths are non-fatal — if the modeset fails the caller can still
// create a window at the current desktop resolution and render TaB into it
// (the user just won't get the HDMI 3D InfoFrame).
// ---------------------------------------------------------------------------
class DisplayTimingHelper {
public:
    DisplayTimingHelper() = default;
    ~DisplayTimingHelper();

    // Apply frame-pack timing for the given OutputMode on the monitor
    // identified by display_index (0 = primary). Returns true on success.
    // On failure the display is left in its original mode.
    bool Apply(const FramePackTimingSpec& spec,
               int32_t display_index);

    // Revert to the original display mode. Safe to call even if Apply was
    // never called or failed.
    void Revert();

    // Was the custom timing successfully applied?
    bool IsActive() const { return active_; }

    // Which backend was used?
    enum class Backend { None, Nvidia, Amd, CruFallback };
    Backend GetBackend() const { return backend_; }

private:
    bool TryNvidia(const FramePackTimingSpec& spec, int32_t display_index);
    bool TryAmd(const FramePackTimingSpec& spec, int32_t display_index);
    bool TryCruFallback(const FramePackTimingSpec& spec, int32_t display_index);

    void RevertNvidia();
    void RevertAmd();
    void RevertCruFallback();

    bool     active_  = false;
    Backend  backend_ = Backend::None;

    // Saved state for revert — backend-specific opaque storage.
    // NVIDIA: display IDs used with TryCustomDisplay + original DEVMODE.
    // AMD: adapter/display indices + original DEVMODE.
    // CRU fallback: device name + original DEVMODE.
    struct NvidiaState;
    struct AmdState;
    struct CruState;
    void* backend_state_ = nullptr;   // owned, type depends on backend_
};

}  // namespace vrto3d

